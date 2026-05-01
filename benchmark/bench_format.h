#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <ios>
#include <optional>
#include <ostream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace citor::bench {

/// Process-global toggle for the tail-percentile output columns.
///
/// Bench TUs that can populate `BenchRow::tailNs` consult this flag before
/// doing so. The default is OFF so the current bench output is byte-identical
/// to runs that pre-date the flag; downstream awk parsers in the
/// downstream awk parsers key off `$2` (ns/op) and `$NF` (row name), which
/// both stay stable across the flag toggle. The flag is set from
/// `bench_main.cpp` based on the `--with-tail-percentiles` command-line
/// argument.
///
/// Mutable reference to the flag.
inline bool &tailPercentilesEnabled() {
  static bool enabled = false;
  return enabled;
}

/// Engine-name substring filter (populated from the `--engine` CLI flag).
///
/// `bench_main.cpp` writes the parsed substrings into the global vector at startup.
/// Per-pool measure functions check `engineEnabled(name)` at the top and early-return
/// a sentinel row when the predicate is false, so measurement is genuinely skipped
/// rather than just hidden from the formatted output.
inline std::vector<std::string> &engineFilters() {
  static std::vector<std::string> filters;
  return filters;
}

/// Process-global toggle for raw-sample export.
///
/// Set from `bench_main.cpp` when the `--export <path>` CLI flag (or the
/// `CITOR_BENCH_EXPORT` env fallback) is present. When ON, `finalizeRow` copies
/// the per-iteration ns vector into `BenchRow::rawSamplesNs` BEFORE its in-place
/// sort + p25/MAD reduction so the exporter can emit one record per measured
/// iteration alongside the existing terminal table. Default OFF: no copy
/// happens, the dispatch hot path and the bench's measurement code are
/// untouched, terminal output is byte-identical to a no-flag run.
///
/// Mutable reference to the flag.
inline bool &rawSampleExportEnabled() {
  static bool enabled = false;
  return enabled;
}

/// Returns true when |name| should be measured. Empty filter list means "all on";
/// otherwise true when at least one filter substring is contained in |name|.
inline bool engineEnabled(std::string_view name) {
  const auto &filters = engineFilters();
  if (filters.empty()) {
    return true;
  }
  for (const auto &filter : filters) {
    if (name.find(filter) != std::string_view::npos) {
      return true;
    }
  }
  return false;
}

/// One competitor's measurement row in a comparative bench table.
///
/// `relative` is computed by `formatTable` against the row whose `name` matches
/// the chosen baseline; raw fields are filled by the workload runner.
struct BenchRow {
  /// Pool name shown in the rightmost column.
  std::string name;

  /// When set, the row was filtered out via `--engine` and skipped before
  /// measurement ran. `formatTable` does not render skipped rows.
  bool skipped = false;

  /// Median wall time per dispatch in nanoseconds.
  double nsPerOp = 0.0;

  /// Operations per second (1e9 / `nsPerOp`).
  double opsPerSec = 0.0;

  /// Relative standard deviation of `nsPerOp` (percentage).
  double errPercent = 0.0;

  /// Optional tail-percentile triple (p25, p50, p99) in ns. Populated only by
  /// workloads that explicitly opt in (typically those that build an
  /// `hdr_histogram` over their cycle samples) and only when the global
  /// `tailPercentilesEnabled()` flag is true. When `std::nullopt`, `formatTable`
  /// renders no extra columns and the row is byte-identical to the pre-tail
  /// output.
  std::optional<std::array<double, 3>> tailNs;

  /// Per-iteration raw samples in ns, copied by `finalizeRow` ONLY when
  /// `rawSampleExportEnabled()` is set at the time `finalizeRow` runs. Empty in
  /// the default (no `--export`) path. Written by the JSON exporter in
  /// `bench_export.h`; never consulted by the terminal formatter.
  std::vector<double> rawSamplesNs;
};

/// A complete comparison table for one workload.
///
/// The workload name appears in the header row; the rows vector is rendered as
/// the body. Ordering of `rows` is preserved by `formatTable`; the function
/// appends a prefix `relative` column computed against the row whose name
/// matches |baselineName|.
struct BenchTable {
  /// Free-form workload identifier shown in the header (e.g.
  /// `empty_fan_out_j16_hot`).
  std::string workload;

  /// Measurement rows; rendered in the supplied order.
  std::vector<BenchRow> rows;
};

/// Format a number with |sigDigits| significant digits and SI suffix.
///
/// Used for the `op/s` column where values span 4-9 decades depending on the
/// workload (1 / sec for cold runs vs 100 M+ / sec for cycle-counter idle).
///
/// value      Raw number to format.
/// sigDigits  Significant digit budget (typically 3).
/// Formatted string, e.g. `"2.0M"` for `2'000'000`.
inline std::string formatSiNumber(double value, int sigDigits = 3) {
  struct SiSuffix {
    double scale;
    const char *suffix;
  };
  static constexpr std::array<SiSuffix, 4> kSuffixes{{
      {.scale = 1e9, .suffix = "G"},
      {.scale = 1e6, .suffix = "M"},
      {.scale = 1e3, .suffix = "k"},
      {.scale = 1.0, .suffix = ""},
  }};
  for (const auto &entry : kSuffixes) {
    if (std::fabs(value) >= entry.scale) {
      std::ostringstream oss;
      oss << std::fixed << std::setprecision(sigDigits - 1) << (value / entry.scale)
          << entry.suffix;
      return oss.str();
    }
  }
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(sigDigits) << value;
  return oss.str();
}

/// Render a `BenchTable` as the comparison block used by the harness.
///
/// The output format mirrors nanobench-style comparison tables:
///
/// ```
/// relative   ns/op    op/s      err%   workload: empty_fan_out_j16_hot
/// 100.0%     500      2.0M      0.5%   citor::ThreadPool
/// 4.6%       10800    93k       0.8%   BS::thread_pool
/// ```
///
/// `relative` is computed as `100.0 * baseline_ns / row_ns` so a row faster than
/// the baseline lands above 100 %, slower rows below. When the baseline name is
/// not present in the rows the table still renders but every relative cell is
/// blank.
///
/// table         Workload-scoped rows to render.
/// baselineName  Row whose `nsPerOp` defines the 100 % reference; the
///                      harness uses `citor::ThreadPool`.
/// out           Stream the formatted table is written to.
inline void formatTable(const BenchTable &table, std::string_view baselineName, std::ostream &out) {
  double baselineNs = 0.0;
  for (const auto &row : table.rows) {
    // Workloads that exercise multiple primitives within citor (e.g. chain
    // tables include both `citor::ThreadPool::parallelChain` and
    // `citor::ThreadPool::parallelFor x7`) suffix the row name. Match on
    // `baselineName` as a prefix so the relative column renders for every
    // table without forcing every workload to use a single canonical name.
    if (std::string_view{row.name}.starts_with(baselineName)) {
      baselineNs = row.nsPerOp;
      break;
    }
  }

  // Tail-percentile columns are emitted only when at least one row populated
  // `tailNs`. Workloads that do not populate the optional render byte-identical
  // to the pre-tail format; downstream awk parsers that key off `$2` and `$NF`
  // remain stable in either mode.
  bool anyTail = false;
  for (const auto &row : table.rows) {
    if (row.tailNs.has_value()) {
      anyTail = true;
      break;
    }
  }

  // Header row.
  out << std::left << std::setw(11) << "relative" << std::setw(10) << "ns/op" << std::setw(10)
      << "op/s" << std::setw(8) << "err%";
  if (anyTail) {
    out << std::setw(10) << "p25" << std::setw(10) << "p50" << std::setw(10) << "p99";
  }
  out << "workload: " << table.workload << '\n';

  for (const auto &row : table.rows) {
    std::string relative;
    if (baselineNs > 0.0 && row.nsPerOp > 0.0) {
      std::ostringstream oss;
      oss << std::fixed << std::setprecision(1) << (100.0 * baselineNs / row.nsPerOp) << "%";
      relative = oss.str();
    } else {
      relative = "-";
    }

    std::ostringstream nsOss;
    nsOss << std::fixed << std::setprecision(0) << row.nsPerOp;

    std::ostringstream errOss;
    errOss << std::fixed << std::setprecision(1) << row.errPercent << "%";

    out << std::left << std::setw(11) << relative << std::setw(10) << nsOss.str() << std::setw(10)
        << formatSiNumber(row.opsPerSec) << std::setw(8) << errOss.str();
    if (anyTail) {
      auto renderTail = [](const std::optional<std::array<double, 3>> &tail, std::size_t idx) {
        if (!tail.has_value()) {
          return std::string{"-"};
        }
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(0) << (*tail)[idx];
        return oss.str();
      };
      out << std::setw(10) << renderTail(row.tailNs, 0U) << std::setw(10)
          << renderTail(row.tailNs, 1U) << std::setw(10) << renderTail(row.tailNs, 2U);
    }
    out << row.name << '\n';
  }
}

/// Reduce a sample vector into a `BenchRow` using the lower-quartile (p25)
///        as the headline statistic.
///
/// The bench formerly reported the median (p50). Median is stable under symmetric
/// outliers but fragile on bimodal distributions: when the host is in a state
/// where roughly half the iters are stalled by THP defrag, NUMA balancing, IRQ
/// migration, or CPU-boost throttling, the median lands on the slow mode for
/// one cell and the fast mode for another, even at the same workload. The lower
/// quartile is below the slow mode for any bimodal distribution where the slow
/// cluster is at most three times the size of the fast cluster, so it converges
/// to the actual hardware-floor cost the comparison wants to surface. Stalls
/// still pollute samples, but they no longer contaminate the headline.
///
/// name     Pool display name copied into the row.
/// samples  Per-iteration ns measurements; mutated in place via sort.
/// Populated `BenchRow` with `nsPerOp` set to the p25, `opsPerSec` set
///         to `1e9 / nsPerOp`, and `errPercent` set to the relative standard
///         deviation across the full sample vector.
[[nodiscard]] inline BenchRow finalizeRow(std::string name, std::vector<double> &samples) {
  if (samples.empty()) {
    return BenchRow{.name = std::move(name),
                    .nsPerOp = 0.0,
                    .opsPerSec = 0.0,
                    .errPercent = 0.0,
                    .tailNs = std::nullopt,
                    .rawSamplesNs = {}};
  }
  // Snapshot raw samples in their natural (chronological) order BEFORE the
  // in-place sort below, so the exporter can record per-iteration ns in the
  // order they were measured. This copy is conditional on the export flag and
  // only executed under `--export` -- the default no-flag path does zero extra
  // work.
  std::vector<double> rawSnapshot;
  if (rawSampleExportEnabled()) {
    rawSnapshot = samples;
  }
  std::sort(samples.begin(), samples.end());
  const std::size_t pIdx = samples.size() / 4U;
  const double p25 = samples[pIdx];
  const double opsPerSec = p25 > 0.0 ? 1.0e9 / p25 : 0.0;
  // Headline is p25 (lower-quartile, stable under bimodal tails). The error stat
  // reports the median absolute deviation (MAD) of a CENTERED 10-percentile
  // WINDOW around p25 (samples in `[p20, p30]`), scaled to a Gaussian-
  // comparable sigma (factor 1.4826) and expressed as a percentage of `p25`.
  //
  // Why centered around p25 instead of bottom-quartile or bottom-decile:
  // both lower-tail windows include part of the distribution that is
  // BELOW the headline -- on adapters with wide fast clusters or bimodal
  // schedules (dp / Eigen / task at the dispatch-floor cells) that lower
  // tail dominates the MAD even though it does not affect the stability of
  // p25 itself. The `[p20, p30]` window is the literal "how much can p25
  // wiggle" question the comparative read asks: it spans samples that
  // could plausibly become the next run's p25, and its MAD is therefore an
  // upper bound on the run-to-run drift of the headline. Empirically this
  // pulls every granularity cell under 10 % MAD across consecutive runs.
  const std::size_t windowStart = (samples.size() * 20U) / 100U;
  const std::size_t windowEnd = (samples.size() * 30U) / 100U;
  double errPct = 0.0;
  if (windowEnd > windowStart && (windowEnd - windowStart) >= 4U && p25 > 0.0) {
    const std::size_t windowSize = windowEnd - windowStart;
    const double windowMedian = samples[windowStart + (windowSize / 2U)];
    std::vector<double> deviations;
    deviations.reserve(windowSize);
    for (std::size_t i = windowStart; i < windowEnd; ++i) {
      deviations.push_back(std::fabs(samples[i] - windowMedian));
    }
    std::sort(deviations.begin(), deviations.end());
    const double mad = deviations[windowSize / 2U];
    constexpr double kMadToSigma = 1.4826;
    errPct = 100.0 * kMadToSigma * mad / p25;
  }
  BenchRow row{
      .name = std::move(name),
      .nsPerOp = p25,
      .opsPerSec = opsPerSec,
      .errPercent = errPct,
      .tailNs = std::nullopt,
      .rawSamplesNs = {},
  };
  // When the tail-percentile flag is on, populate tailNs from the already-
  // sorted samples. The convention matches `parallel_for_cold_dispatch_bench.cpp`
  // (the only TU previously consuming `BenchRow::tailNs`): {p25, p50, p99}
  // in nanoseconds. Linear interpolation between sample indices is overkill
  // for the bench's sample sizes (10-50); pick the floor index.
  if (tailPercentilesEnabled() && !samples.empty()) {
    auto pickPct = [&samples](double pct) -> double {
      const std::size_t maxIdx = samples.size() - 1U;
      const std::size_t idx = static_cast<std::size_t>((pct / 100.0) * static_cast<double>(maxIdx));
      return samples[idx];
    };
    row.tailNs = std::array<double, 3>{pickPct(25.0), pickPct(50.0), pickPct(99.0)};
  }
  if (!rawSnapshot.empty()) {
    row.rawSamplesNs = std::move(rawSnapshot);
  }
  return row;
}

/// Compute the relative standard deviation of |samples| as a percentage.
///
/// Used by the bench drivers when nanobench is bypassed (e.g. when the harness
/// needs raw cycle samples that nanobench does not expose). Returns 0.0 for
/// sample sizes below 2 and `NaN` for non-finite means.
///
/// samples  Per-iteration measurements in arbitrary units.
/// Standard deviation expressed as a percentage of the mean.
inline double relativeStdDevPercent(const std::vector<double> &samples) {
  if (samples.size() < 2) {
    return 0.0;
  }
  double sum = 0.0;
  for (double v : samples) {
    sum += v;
  }
  const double mean = sum / static_cast<double>(samples.size());
  if (!std::isfinite(mean) || mean == 0.0) {
    return 0.0;
  }
  double sqSum = 0.0;
  for (double v : samples) {
    const double d = v - mean;
    sqSum += d * d;
  }
  const double variance = sqSum / static_cast<double>(samples.size() - 1);
  return 100.0 * std::sqrt(variance) / std::fabs(mean);
}

/// Centered 95 % bootstrap confidence interval half-width for the median
///        of |samples|, expressed as a percentage of that median.
///
/// For sample sizes below `kBootstrapMinSamples` (30) the parametric
/// `relativeStdDevPercent` is the better estimator: bootstrap percentile
/// coverage at small n is biased even on Gaussian data (Kalibera & Jones,
/// ISMM 2013), and the ns/op samples this bench collects are heavy-tailed in
/// the cold-cell regime where the bootstrap window would underweight rare
/// stalls. For n >= 30 the bootstrap is stable under multimodality and outliers;
/// the parametric stddev mis-reports under heavy-tailed distributions because
/// the second moment is inflated by the tail.
///
/// The helper returns 0.0 for sample sizes below the bootstrap threshold so
/// the caller can fall back to `relativeStdDevPercent` for the small-n path
/// without negotiating a sentinel; both helpers return zero for degenerate
/// inputs (n < 2, mean == 0, NaN), which keeps the err% column output at
/// `0.0%` in the same edge cases the parametric path already covers.
///
/// samples  Per-iteration ns measurements; not modified.
/// resamples Number of bootstrap resamples to draw. 1024 gives a
///                  ~3 % standard error on the half-width estimate, which is
///                  small enough to be invisible in the rendered err% cell.
/// seed     RNG seed for the resample loop. The bench wants
///                 reproducible CI estimates across runs, so this defaults to
///                 a fixed value rather than `std::random_device`.
/// Bootstrap-CI half-width as a percentage of the sample median, or
///         0.0 when |samples| has fewer than `kBootstrapMinSamples` entries
///         or when the median is zero / non-finite.
constexpr std::size_t kBootstrapMinSamples = 30;

inline double bootstrapMedianCiPercent(std::vector<double> samples, std::size_t resamples = 1024,
                                       std::uint64_t seed = 0xC1709F22ABULL) {
  if (samples.size() < kBootstrapMinSamples) {
    return 0.0;
  }
  std::vector<double> sorted = samples;
  std::sort(sorted.begin(), sorted.end());
  const double median = sorted[sorted.size() / 2U];
  if (!std::isfinite(median) || median == 0.0) {
    return 0.0;
  }
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<std::size_t> pick(0U, samples.size() - 1U);
  std::vector<double> medians;
  medians.reserve(resamples);
  std::vector<double> resample(samples.size());
  for (std::size_t r = 0; r < resamples; ++r) {
    for (std::size_t i = 0; i < samples.size(); ++i) {
      resample[i] = samples[pick(rng)];
    }
    std::nth_element(resample.begin(), resample.begin() + (resample.size() / 2U), resample.end());
    medians.push_back(resample[resample.size() / 2U]);
  }
  std::sort(medians.begin(), medians.end());
  const std::size_t lowIdx = (medians.size() * 25U) / 1000U;
  const std::size_t highIdx = (medians.size() * 975U) / 1000U;
  const double low = medians[lowIdx];
  const double high = medians[highIdx];
  const double halfWidth = (high - low) * 0.5;
  return 100.0 * halfWidth / std::fabs(median);
}

} // namespace citor::bench
