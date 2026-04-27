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
// LLVM libomp's Intel-compatibility extension: lets the runtime's "blocktime"
// (the duration worker threads spin after completing a parallel region before
// parking) be set programmatically. Declared as a free C function so we don't
// pull in `omp-tools.h` for one symbol.
extern "C" void kmp_set_blocktime(int milliseconds);
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
  /// When set, populate `BenchRow::tailNs` (p25/p50/p99) on workloads that
  /// support it and emit the three extra columns. Default OFF so existing
  /// (they key off `$2` ns/op and `$NF` row name; the tail columns sit
  /// between `err%` and the row name).
  bool withTailPercentiles = false;
};

void printUsage(std::ostream &out) {
  out << "usage: parallel_bench [--filter SUBSTR] [--filter=SUBSTR] [--list]\n"
      << "                      [--with-tail-percentiles]\n"
      << "       parallel_bench SUBSTR\n"
      << '\n'
      << "  --filter SUBSTR          Run only workloads whose name contains SUBSTR.\n"
      << "                           May be repeated; matches are OR-ed.\n"
      << "  --list                   Print matching workload names and exit.\n"
      << "  --with-tail-percentiles  Populate p25/p50/p99 columns on workloads that\n"
      << "                           build an `hdr_histogram` over their cycle\n"
      << "                           samples. OFF by default; downstream awk\n"
      << "                           parsers see the pre-tail column layout.\n";
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
    if (arg == "--with-tail-percentiles") {
      opts.withTailPercentiles = true;
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
  tailPercentilesEnabled() = opts.withTailPercentiles;
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

#ifdef CITOR_BENCH_HAS_OPENMP
  // OpenMP rows construct an `OpenMpRunner` carrying the requested participant
  // count, but per-call `num_threads(...)` clauses still consult the global
  // OMP setting for the worker pool's lazy spin-up. Set the global to the
  // largest expected participant count once at startup so the OpenMP runtime
  // pre-spawns enough idle workers for every workload's `j` value.
  omp_set_num_threads(16);
  // libomp's default `KMP_BLOCKTIME=200000us` keeps worker threads spinning
  // for 200 ms after every parallel region before parking. The cold-fan-out
  // cell's 30 ms cool-off is shorter than that default, so by default libomp
  // workers NEVER park during the bench's cool-off window: they stay pinned
  // at 100 % CPU between dispatches and the cell measures hot dispatch on
  // libomp while measuring cold (park-then-wake) dispatch on every other
  // pool. That's a policy mismatch, not a fairness comparison. Force
  // `kmp_set_blocktime(0)` so libomp parks promptly between dispatches and
  // every pool's cold-cell number is comparable. Hot-path cells are not
  // affected by parking choice (workers see the next dispatch before any
  // park budget can fire). The checklist below reports the resulting
  // blocktime so the printed value reflects the post-override state, not libomp's
  // default.
  kmp_set_blocktime(0);
#endif

  // Checklist runs AFTER `kmp_set_blocktime(0)` so the reported libomp
  // blocktime reflects the bench's policy override, not libomp's default.
  printChecklist(std::cout);
  std::cout << '\n';

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
