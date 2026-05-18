// Driver entry point for the two-pool BLAS-coexistence comparative bench.
//
// Mirrors `bench_main.cpp` but lives in a SEPARATE executable target
// (`parallel_bench_two_pool`). The standard `parallel_bench` calls
// `kmp_set_blocktime(0)` once at startup and runs every cell against that
// policy; the two-pool bench needs to run the SAME workload twice (once at
// libomp's default blocktime and once at zero) and the runtime's
// `KMP_BLOCKTIME` global is process-wide. Sharing a process with the
// standard cells would leak each cell's blocktime override into the other
// cells' measurements.
//
// Driver responsibilities are kept minimal: every workload TU registers
// itself via `registerWorkload`; this driver iterates the registry, prints
// the resulting `BenchTable` for each, and returns 0 on success.

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>
#include <omp.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

#include "../harness/bench_format.h"
#include "../harness/bench_registry.h"
#include "../harness/cycle_clock.h"
#include "../harness/harness.h"

namespace {

void printCalibrationBanner(const citor::bench::CyclesPerNanosecond &cal) {
  std::cout << "TSC calibration: " << cal.value << " cycles/ns ("
            << (cal.value > 0.0 ? 1000.0 / cal.value : 0.0) << " ns/cycle)\n";
}

struct CliOptions {
  std::vector<std::string_view> filters;
  bool listOnly = false;
};

void printUsage(std::ostream &out) {
  out << "usage: parallel_bench_two_pool [--filter SUBSTR] [--filter=SUBSTR] "
         "[--list]\n"
      << "       parallel_bench_two_pool SUBSTR\n";
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
        std::cerr << "parallel_bench_two_pool: --filter requires a substring\n";
        return false;
      }
      opts.filters.emplace_back(argv[++i]);
      continue;
    }
    constexpr std::string_view kFilterPrefix = "--filter=";
    if (arg.substr(0, kFilterPrefix.size()) == kFilterPrefix) {
      const std::string_view value = arg.substr(kFilterPrefix.size());
      if (value.empty()) {
        std::cerr << "parallel_bench_two_pool: --filter requires a non-empty "
                     "substring\n";
        return false;
      }
      opts.filters.push_back(value);
      continue;
    }
    if (!arg.empty() && arg.front() == '-') {
      std::cerr << "parallel_bench_two_pool: unknown option '" << arg << "'\n";
      printUsage(std::cerr);
      return false;
    }
    opts.filters.push_back(arg);
  }
  return true;
}

bool matchesFilters(std::string_view name,
                    const std::vector<std::string_view> &filters) {
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
    std::cerr << "parallel_bench_two_pool: no workloads registered; check link "
                 "order\n";
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

  // Pre-spawn libomp's worker pool at the participant count the bench
  // assumes (16). Each cell will subsequently override `KMP_BLOCKTIME` per
  // measurement; the worker count is set once here so the secondary's
  // `parallel for` clauses observe a stable `omp_get_max_threads()`.
  omp_set_num_threads(16);

  // The bench-fixture checklist is reused so the gates that matter for the
  // two-pool measurement (governor, boost, smt, aslr, tsan, libomp
  // blocktime) print at the top of the run. Note: the libomp blocktime gate
  // reports the value at checklist time, NOT each cell's per-cell override;
  // the per-cell value is captured inside the workload TU and reflected in
  // the cell's row name.
  std::cout << "[NOTE] parallel_bench_two_pool tests both KMP_BLOCKTIME values "
               "per cell.\n"
            << "[NOTE] The libomp_blocktime=0 checklist gate below reads FAIL "
               "by design; each cell\n"
            << "[NOTE] sets blocktime explicitly. See row labels for the "
               "per-cell value.\n";
  printChecklist(std::cout);
  std::cout << '\n';

  // Inter-cell cool-off must cover libomp's default KMP_BLOCKTIME=200ms so the
  // libomp worker pool has fully parked between cells. A shorter cool-off
  // leaves cell N's libomp workers spinning when cell N+1 starts, which
  // corrupts cell N+1's measurement (the kmp_set_blocktime(0) call quiesces
  // workers that were still spinning from the previous cell, not workers that
  // came out of a cold pool).
  constexpr auto kInterCellCoolOff = std::chrono::milliseconds(300);

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
      std::cout << "workload: " << reg.name << " SKIPPED: " << ex.what()
                << '\n';
    }
    const std::uint64_t rssAfterKb = readPeakRssKb();
    const RusageSample rusageAfter = readRusage();
    const std::uint64_t userDeltaUs = rusageAfter.userUs - rusageBefore.userUs;
    const std::uint64_t systemDeltaUs =
        rusageAfter.systemUs - rusageBefore.systemUs;
    std::cout << "[METRICS] " << reg.name << "  peak_rss_kb=" << rssAfterKb
              << " (delta="
              << (rssAfterKb >= rssBeforeKb ? rssAfterKb - rssBeforeKb : 0U)
              << ")  user_us=" << userDeltaUs << "  system_us=" << systemDeltaUs
              << '\n';
    std::cout << '\n';
    std::cout.flush();
    anyRan = true;
  }

  if (!anyRan) {
    std::cerr << "parallel_bench_two_pool: no workloads matched filter\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
