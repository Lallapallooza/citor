// Driver entry point for the parallel pool comparative bench.
//
// The bench harness measures dispatch latency for each competitor pool on the
// same workload using `__rdtscp`-bracketed cycle stamps. The harness deliberately
// does not use `chrono::steady_clock` for the cycle samples; `clock_gettime` is
// used only by the calibration step that converts cycle deltas to wall-clock
// nanoseconds.
//
// The driver is minimal: each workload TU registers its name and
// runner, the driver iterates them in registration order, prints the resulting
// `BenchTable` for each, and returns 0 on success.

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <thread>

#include "bench_format.h"
#include "bench_registry.h"
#include "cycle_clock.h"
#include "harness.h"

#ifdef CITOR_BENCH_HAS_OPENMP
#include <omp.h>
#endif

namespace {

/// Print a one-shot calibration banner so users can see what the cycles-per-ns
/// constant ended up at on their host. Useful when debugging suspect numbers.
void printCalibrationBanner(const citor::bench::CyclesPerNanosecond &cal) {
  std::cout << "TSC calibration: " << cal.value << " cycles/ns ("
            << (cal.value > 0.0 ? 1000.0 / cal.value : 0.0) << " ns/cycle)\n";
}

} // namespace

int main(int argc, char ** /*argv*/) {
  using namespace citor::bench;

  const CyclesPerNanosecond cal = calibrateCyclesPerNs();
  if (argc > 1) {
    // The bench accepts no CLI arguments yet; warn so a stray flag does not
    // silently affect a result the caller is about to compare.
    std::cerr << "parallel_bench: ignoring CLI arguments (none accepted)\n";
  }
  printCalibrationBanner(cal);
  printChecklist(std::cout);
  std::cout << '\n';

#ifdef CITOR_BENCH_HAS_OPENMP
  // OpenMP rows construct an `OpenMpRunner` carrying the requested participant
  // count, but per-call `num_threads(...)` clauses still consult the global
  // OMP setting for the worker pool's lazy spin-up. Set the global to the
  // largest expected participant count once at startup so the OpenMP runtime
  // pre-spawns enough idle workers for every workload's `j` value.
  omp_set_num_threads(16);
#endif

  // Inter-cell cool-off. After a 100 ms-budget cell that pegged 16 cores, the package
  // accumulates thermal load and the next cell starts in a different operating point.
  // Sleeping briefly between cells lets thermals, THP defrag scans, and any pending IRQ
  // migrations settle so successive cells start from a comparable baseline. The cost is
  // additive wall time, not bench correctness.
  constexpr auto kInterCellCoolOff = std::chrono::milliseconds(100);

  bool anyRan = false;
  bool firstCell = true;
  for (const auto &reg : registry()) {
    if (!firstCell) {
      std::this_thread::sleep_for(kInterCellCoolOff);
    }
    firstCell = false;
    const std::uint64_t rssBeforeKb = readPeakRssKb();
    const RusageSample rusageBefore = readRusage();
    try {
      const BenchTable table = reg.run(cal);
      formatTable(table, /*baselineName=*/"citor::ThreadPool", std::cout);
    } catch (const std::exception &ex) {
      // A workload may legitimately refuse to run (e.g. when the host's CPU affinity
      // mask collapses the pool to a single participant and the workload would otherwise
      // measure the inline-fallback path). Print a sentinel row so the misconfiguration
      // is visible instead of faking a passing number, and continue with the next workload.
      std::cout << "workload: " << reg.name << " SKIPPED: " << ex.what() << '\n';
    }
    const std::uint64_t rssAfterKb = readPeakRssKb();
    const RusageSample rusageAfter = readRusage();
    const std::uint64_t userDeltaUs = rusageAfter.userUs - rusageBefore.userUs;
    const std::uint64_t systemDeltaUs = rusageAfter.systemUs - rusageBefore.systemUs;
    std::cout << "[METRICS] " << reg.name << "  peak_rss_kb=" << rssAfterKb
              << " (delta=" << (rssAfterKb >= rssBeforeKb ? rssAfterKb - rssBeforeKb : 0U)
              << ")  user_us=" << userDeltaUs << "  system_us=" << systemDeltaUs << '\n';
    std::cout << '\n';
    std::cout.flush();
    anyRan = true;
  }

  if (!anyRan) {
    std::cerr << "parallel_bench: no workloads registered; check link order\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
