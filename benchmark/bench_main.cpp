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
#include <string_view>
#include <thread>
#include <vector>

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

struct CliOptions {
  std::vector<std::string_view> filters;
  bool listOnly = false;
};

void printUsage(std::ostream &out) {
  out << "usage: parallel_bench [--filter SUBSTR] [--filter=SUBSTR] [--list]\n"
      << "       parallel_bench SUBSTR\n";
}

bool parseArgs(int argc, char **argv, CliOptions &opts) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};
    if (arg == "--help" || arg == "-h") {
      printUsage(std::cout);
      std::exit(EXIT_SUCCESS);
    }
    if (arg == "--list") {
      opts.listOnly = true;
      continue;
    }
    if (arg == "--filter" || arg == "-f") {
      if (i + 1 >= argc) {
        std::cerr << "parallel_bench: --filter requires a substring\n";
        return false;
      }
      opts.filters.emplace_back(argv[++i]);
      continue;
    }
    constexpr std::string_view kFilterPrefix = "--filter=";
    if (arg.substr(0, kFilterPrefix.size()) == kFilterPrefix) {
      const std::string_view value = arg.substr(kFilterPrefix.size());
      if (value.empty()) {
        std::cerr << "parallel_bench: --filter requires a non-empty substring\n";
        return false;
      }
      opts.filters.push_back(value);
      continue;
    }
    if (!arg.empty() && arg.front() == '-') {
      std::cerr << "parallel_bench: unknown option '" << arg << "'\n";
      printUsage(std::cerr);
      return false;
    }
    opts.filters.push_back(arg);
  }
  return true;
}

bool matchesFilters(std::string_view name, const std::vector<std::string_view> &filters) {
  if (filters.empty()) {
    return true;
  }
  for (const std::string_view filter : filters) {
    if (name.find(filter) != std::string_view::npos) {
      return true;
    }
  }
  return false;
}

} // namespace

int main(int argc, char **argv) {
  using namespace citor::bench;

  CliOptions opts;
  if (!parseArgs(argc, argv, opts)) {
    return EXIT_FAILURE;
  }
  if (registry().empty()) {
    std::cerr << "parallel_bench: no workloads registered; check link order\n";
    return EXIT_FAILURE;
  }
  if (opts.listOnly) {
    for (const auto &reg : registry()) {
      if (matchesFilters(reg.name, opts.filters)) {
        std::cout << reg.name << '\n';
      }
    }
    return EXIT_SUCCESS;
  }

  const CyclesPerNanosecond cal = calibrateCyclesPerNs();
  printCalibrationBanner(cal);
  if (!opts.filters.empty()) {
    std::cout << "Workload filter:";
    for (const std::string_view filter : opts.filters) {
      std::cout << ' ' << filter;
    }
    std::cout << "\n";
  }
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
    if (!matchesFilters(reg.name, opts.filters)) {
      continue;
    }
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
    std::cerr << "parallel_bench: no workloads matched filter\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
