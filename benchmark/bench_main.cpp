// Driver entry point for the parallel pool comparative bench.
//
// The bench harness measures dispatch latency for each competitor pool on the
// same workload using `__rdtscp`-bracketed cycle stamps. The harness deliberately
// does not use `chrono::steady_clock` for the cycle samples; `clock_gettime` is
// used only by the calibration step that converts cycle deltas to wall-clock
// nanoseconds.
//
// The driver is minimal by design: each workload TU registers its name and
// runner, the driver iterates them in registration order, prints the resulting
// `BenchTable` for each, and returns 0 on success.

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <cstdlib>
#include <exception>
#include <iostream>

#include "bench_format.h"
#include "bench_registry.h"
#include "cycle_clock.h"

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

#ifdef CITOR_BENCH_HAS_OPENMP
  // OpenMP rows construct an `OpenMpRunner` carrying the requested participant
  // count, but per-call `num_threads(...)` clauses still consult the global
  // OMP setting for the worker pool's lazy spin-up. Set the global to the
  // largest expected participant count once at startup so the OpenMP runtime
  // pre-spawns enough idle workers for every workload's `j` value.
  omp_set_num_threads(16);
#endif

  bool anyRan = false;
  for (const auto &reg : registry()) {
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
    std::cout << '\n';
    anyRan = true;
  }

  if (!anyRan) {
    std::cerr << "parallel_bench: no workloads registered; check link order\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
