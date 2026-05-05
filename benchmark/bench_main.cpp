// Driver entry point for the parallel pool comparative bench.
//
// The bench harness measures dispatch latency for each competitor pool on the
// same workload using `__rdtscp`-bracketed cycle stamps. The harness
// deliberately does not use `chrono::steady_clock` for the cycle samples;
// `clock_gettime` is used only by the calibration step that converts cycle
// deltas to wall-clock nanoseconds.
//
// The driver is minimal by design: each workload TU registers its name and
// runner, the driver iterates them in registration order, prints the resulting
// `BenchTable` for each, and returns 0 on success.

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__linux__) && defined(__GLIBC__)
#include <malloc.h>
#endif

#include "bench_export.h"
#include "bench_format.h"
#include "bench_registry.h"
#include "cycle_clock.h"
#include "harness.h"

#ifdef CITOR_BENCH_HAS_OPENMP
#include <omp.h>
#endif

namespace {

// Print a one-shot calibration banner so users can see what the cycles-per-ns
// constant ended up at on their host.
void printCalibrationBanner(const citor::bench::CyclesPerNanosecond &cal) {
  std::cout << "TSC calibration: " << cal.value << " cycles/ns ("
            << (cal.value > 0.0 ? 1000.0 / cal.value : 0.0) << " ns/cycle)\n";
}

struct CliOptions {
  std::vector<std::string_view> filters;
  // Engine-name substring filters. When non-empty, only rows whose name
  // contains at least one substring are rendered; measurement still runs.
  std::vector<std::string_view> engineFilters;
  bool listOnly = false;
  // When set, populate BenchRow::tailNs (p25/p50/p99) and emit three extra
  // columns. Default OFF keeps the pre-tail layout for downstream parsers.
  bool withTailPercentiles = false;
  // When non-empty, the bench writes a single JSON document with raw samples
  // and provenance to this path. From --export, fallback CITOR_BENCH_EXPORT.
  std::string exportPath;
  // Pretty-print the JSON export (default compact). Set via
  // CITOR_BENCH_EXPORT_PRETTY=1.
  bool exportPretty = false;
};

void printUsage(std::ostream &out) {
  out << "usage: parallel_bench [--filter SUBSTR] [--engine SUBSTR] [--list]\n"
      << "                      [--with-tail-percentiles] [--export PATH]\n"
      << "       parallel_bench SUBSTR\n"
      << '\n'
      << "  --filter SUBSTR          Run only workloads whose name contains "
         "SUBSTR.\n"
      << "                           May be repeated; matches are OR-ed.\n"
      << "  --engine SUBSTR          Only render rows whose engine/pool name "
         "contains\n"
      << "                           SUBSTR. May be repeated; matches are "
         "OR-ed.\n"
      << "                           Substring of the row's display name "
         "(e.g.\n"
      << "                           `citor`, `oneTBB`, `[Static]`). "
         "Measurement\n"
      << "                           still runs for every pool.\n"
      << "  --list                   Print matching workload names and exit.\n"
      << "  --with-tail-percentiles  Populate p25/p50/p99 columns on workloads "
         "that\n"
      << "                           build an `hdr_histogram` over their "
         "cycle\n"
      << "                           samples. OFF by default; downstream awk\n"
      << "                           parsers see the pre-tail column layout.\n"
      << "  --export PATH            Write a single JSON document to PATH "
         "carrying\n"
      << "                           per-iteration raw samples and full "
         "provenance\n"
      << "                           (host, kernel, governor, TSC freq, citor "
         "commit,\n"
      << "                           checklist gates, etc.). Falls back to\n"
      << "                           CITOR_BENCH_EXPORT=PATH env var. Set\n"
      << "                           CITOR_BENCH_EXPORT_PRETTY=1 for indented "
         "JSON.\n";
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
    if (arg.starts_with(kFilterPrefix)) {
      const std::string_view value = arg.substr(kFilterPrefix.size());
      if (value.empty()) {
        std::cerr
            << "parallel_bench: --filter requires a non-empty substring\n";
        return false;
      }
      opts.filters.push_back(value);
      continue;
    }
    if (arg == "--engine" || arg == "-e") {
      if (i + 1 >= argc) {
        std::cerr << "parallel_bench: --engine requires a substring\n";
        return false;
      }
      opts.engineFilters.emplace_back(argv[++i]);
      continue;
    }
    if (arg == "--export") {
      if (i + 1 >= argc) {
        std::cerr << "parallel_bench: --export requires a path\n";
        return false;
      }
      opts.exportPath = argv[++i];
      continue;
    }
    constexpr std::string_view kExportPrefix = "--export=";
    if (arg.starts_with(kExportPrefix)) {
      const std::string_view value = arg.substr(kExportPrefix.size());
      if (value.empty()) {
        std::cerr << "parallel_bench: --export requires a non-empty path\n";
        return false;
      }
      opts.exportPath.assign(value);
      continue;
    }
    constexpr std::string_view kEnginePrefix = "--engine=";
    if (arg.starts_with(kEnginePrefix)) {
      const std::string_view value = arg.substr(kEnginePrefix.size());
      if (value.empty()) {
        std::cerr
            << "parallel_bench: --engine requires a non-empty substring\n";
        return false;
      }
      opts.engineFilters.push_back(value);
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
  tailPercentilesEnabled() = opts.withTailPercentiles;
  {
    auto &filters = engineFilters();
    filters.clear();
    for (const std::string_view f : opts.engineFilters) {
      filters.emplace_back(f);
    }
  }
  // CLI flag wins; env var is the fallback for CI / wrapper-script use.
  if (opts.exportPath.empty()) {
    if (const char *envPath = std::getenv("CITOR_BENCH_EXPORT");
        envPath != nullptr && envPath[0] != '\0') {
      opts.exportPath = envPath;
    }
  }
  if (const char *prettyEnv = std::getenv("CITOR_BENCH_EXPORT_PRETTY");
      prettyEnv != nullptr && prettyEnv[0] != '\0' && prettyEnv[0] != '0') {
    opts.exportPretty = true;
  }
  // Toggle the global flag BEFORE any cell runs, so `finalizeRow` snapshots raw
  // samples on every call. When `exportPath` is empty the flag stays off and
  // every cell runs on the no-extra-work path identical to pre-export bench
  // builds.
  rawSampleExportEnabled() = !opts.exportPath.empty();
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
  // blocktime, not libomp's default.
  kmp_set_blocktime(0);
#endif

  // Checklist runs AFTER `kmp_set_blocktime(0)` so the reported libomp
  // blocktime reflects the bench's policy override, not libomp's default.
  printChecklist(std::cout);
  std::cout << '\n';

  // Inter-cell cool-off. After a 100 ms-budget cell that pegged 16 cores, the
  // package accumulates thermal load and the next cell starts in a different
  // operating point. Sleeping briefly between cells lets thermals, THP defrag
  // scans, and any pending IRQ migrations settle so successive cells start from
  // a comparable baseline. The cost is additive wall time, not bench
  // correctness.
  constexpr auto kInterCellCoolOff = std::chrono::milliseconds(100);
  // Inter-cell heap reset. Recursive-task workloads (skynet, strassen DaC,
  // cilksort) churn millions of small allocations and frees per cell; over a
  // 90+ cell sweep the glibc arena state fragments enough that successive
  // recursive-task cells appear 5-10x slower than they do in isolation
  // (citor's parallelFor/Reduce don't allocate per-task, so they are not
  // affected; same with oneTBB's task arenas; the symmetry breaks for
  // libfork, dispenso fork-join, and citor's `forkJoin` recursive children
  // which DO allocate). `malloc_trim(0)` returns released-but-not-yet-freed
  // arenas to the OS so each cell starts from a comparable allocator state.
  // No-op on non-glibc hosts (musl, etc.).
  const auto resetAllocatorState = []() {
#if defined(__linux__) && defined(__GLIBC__)
    (void)malloc_trim(0);
#endif
  };

  bool anyRan = false;
  bool firstCell = true;
  // Tables are retained across cells only when --export is on, so the JSON
  // writer can emit one record per (workload, pool, rep). When --export is
  // off this vector stays empty and the cell-loop body's `BenchTable` lives
  // only for the duration of formatting, matching the prior behavior.
  std::vector<BenchTable> exportTables;
  for (const auto &reg : registry()) {
    if (!matchesFilters(reg.name, opts.filters)) {
      continue;
    }
    if (!firstCell) {
      resetAllocatorState();
      std::this_thread::sleep_for(kInterCellCoolOff);
    }
    firstCell = false;
    const std::uint64_t rssBeforeKb = readPeakRssKb();
    const RusageSample rusageBefore = readRusage();
    try {
      BenchTable table = reg.run(cal);
      // Drop sentinel rows that were skipped before measurement via
      // `engineEnabled` (populated from `--engine`).
      table.rows.erase(
          std::remove_if(table.rows.begin(), table.rows.end(),
                         [](const BenchRow &row) { return row.skipped; }),
          table.rows.end());
      if (!table.rows.empty()) {
        formatTable(table, /*baselineName=*/"citor::ThreadPool", std::cout);
      }
      if (!opts.exportPath.empty() && !table.rows.empty()) {
        exportTables.push_back(std::move(table));
      }
    } catch (const std::exception &ex) {
      // A workload may legitimately refuse to run (e.g. when the host's CPU
      // affinity mask collapses the pool to a single participant and the
      // workload would otherwise measure the inline-fallback path). Print a
      // sentinel row so the misconfiguration is visible instead of faking a
      // passing number, and continue with the next workload.
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
    std::cerr << "parallel_bench: no workloads matched filter\n";
    return EXIT_FAILURE;
  }
  if (!opts.exportPath.empty()) {
    const ExportContext context = probeContext(cal);
    const bool ok = writeJsonExport(opts.exportPath, context, exportTables,
                                    opts.exportPretty);
    if (!ok) {
      std::cerr << "parallel_bench: --export to '" << opts.exportPath
                << "' failed\n";
      return EXIT_FAILURE;
    }
    std::cout << "[EXPORT] wrote " << opts.exportPath << '\n';
  }
  return EXIT_SUCCESS;
}
