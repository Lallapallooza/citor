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

// Process-global toggle for the tail-percentile output columns. Default OFF
// keeps output byte-identical for downstream awk parsers that key off ns/op
// and row name. Set from bench_main.cpp via --with-tail-percentiles.
inline bool &tailPercentilesEnabled() {
  static bool enabled = false;
  return enabled;
}

// Engine-name substring filter populated from the --engine CLI flag. Per-pool
// measure functions early-return a sentinel row when engineEnabled() is false,
// so measurement is genuinely skipped, not just hidden.
inline std::vector<std::string> &engineFilters() {
  static std::vector<std::string> filters;
  return filters;
}

// Process-global toggle for raw-sample export. When ON, finalizeRow copies the
// per-iteration ns vector into BenchRow::rawSamplesNs BEFORE its in-place sort,
// so the exporter can emit one record per measured iteration. Default OFF: no
// copy, terminal output byte-identical.
inline bool &rawSampleExportEnabled() {
  static bool enabled = false;
  return enabled;
}

// Returns true when |name| should be measured. Empty filter list means "all
// on"; otherwise true when at least one filter substring is contained in
// |name|.
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

// One competitor's measurement row in a comparative bench table.
struct BenchRow {
  // Pool name shown in the rightmost column.
  std::string name;

  // When set, the row was filtered out via --engine and skipped before
  // measurement ran. formatTable does not render skipped rows.
  bool skipped = false;

  // Median wall time per dispatch in nanoseconds.
  double nsPerOp = 0.0;

  // Operations per second (1e9 / nsPerOp).
  double opsPerSec = 0.0;

  // Relative standard deviation of nsPerOp (percentage).
  double errPercent = 0.0;

  // Optional tail-percentile triple (p25, p50, p99) in ns, populated when the
  // workload opts in and tailPercentilesEnabled() is true.
  std::optional<std::array<double, 3>> tailNs;

  // Per-iteration raw samples in ns. Populated by finalizeRow only when
  // rawSampleExportEnabled() is set; consumed by the JSON exporter.
  std::vector<double> rawSamplesNs;
};

// Comparison table for one workload. formatTable preserves row order and
// computes the relative column against |baselineName|.
struct BenchTable {
  // Free-form workload identifier shown in the header.
  std::string workload;

  // Measurement rows; rendered in the supplied order.
  std::vector<BenchRow> rows;
};

// Format |value| with |sigDigits| significant digits and an SI suffix. Used
// for the op/s column where values span many decades.
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
      oss << std::fixed << std::setprecision(sigDigits - 1)
          << (value / entry.scale) << entry.suffix;
      return oss.str();
    }
  }
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(sigDigits) << value;
  return oss.str();
}

// Render |table| as a nanobench-style comparison block. relative is computed
// as 100.0 * baseline_ns / row_ns; a row faster than the baseline lands above
// 100 %. When |baselineName| is not present every relative cell is blank.
inline void formatTable(const BenchTable &table, std::string_view baselineName,
                        std::ostream &out) {
  double baselineNs = 0.0;
  for (const auto &row : table.rows) {
    // Match on |baselineName| as a prefix so workloads that exercise multiple
    // primitives within citor (e.g. chain tables) can suffix the row name.
    if (std::string_view{row.name}.starts_with(baselineName)) {
      baselineNs = row.nsPerOp;
      break;
    }
  }

  // Tail-percentile columns are emitted only when at least one row populated
  // tailNs.
  bool anyTail = false;
  for (const auto &row : table.rows) {
    if (row.tailNs.has_value()) {
      anyTail = true;
      break;
    }
  }

  // Header row.
  out << std::left << std::setw(11) << "relative" << std::setw(10) << "ns/op"
      << std::setw(10) << "op/s" << std::setw(8) << "err%";
  if (anyTail) {
    out << std::setw(10) << "p25" << std::setw(10) << "p50" << std::setw(10)
        << "p99";
  }
  out << "workload: " << table.workload << '\n';

  for (const auto &row : table.rows) {
    std::string relative;
    if (baselineNs > 0.0 && row.nsPerOp > 0.0) {
      std::ostringstream oss;
      oss << std::fixed << std::setprecision(1)
          << (100.0 * baselineNs / row.nsPerOp) << "%";
      relative = oss.str();
    } else {
      relative = "-";
    }

    std::ostringstream nsOss;
    nsOss << std::fixed << std::setprecision(0) << row.nsPerOp;

    std::ostringstream errOss;
    errOss << std::fixed << std::setprecision(1) << row.errPercent << "%";

    out << std::left << std::setw(11) << relative << std::setw(10)
        << nsOss.str() << std::setw(10) << formatSiNumber(row.opsPerSec)
        << std::setw(8) << errOss.str();
    if (anyTail) {
      auto renderTail = [](const std::optional<std::array<double, 3>> &tail,
                           std::size_t idx) {
        if (!tail.has_value()) {
          return std::string{"-"};
        }
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(0) << (*tail)[idx];
        return oss.str();
      };
      out << std::setw(10) << renderTail(row.tailNs, 0U) << std::setw(10)
          << renderTail(row.tailNs, 1U) << std::setw(10)
          << renderTail(row.tailNs, 2U);
    }
    out << row.name << '\n';
  }
}

// Reduce |samples| into a BenchRow using the lower quartile (p25) as the
// headline statistic. p25 is below the slow mode of any bimodal distribution
// whose slow cluster is at most 3x the size of the fast cluster, so it
// converges to the hardware-floor cost. |samples| is mutated in place (sort).
[[nodiscard]] inline BenchRow finalizeRow(std::string name,
                                          std::vector<double> &samples) {
  if (samples.empty()) {
    return BenchRow{.name = std::move(name),
                    .nsPerOp = 0.0,
                    .opsPerSec = 0.0,
                    .errPercent = 0.0,
                    .tailNs = std::nullopt,
                    .rawSamplesNs = {}};
  }
  // Snapshot raw samples in chronological order BEFORE the in-place sort, so
  // the exporter can record per-iteration ns in measurement order. Conditional
  // on the export flag; the default path does no extra work.
  std::vector<double> rawSnapshot;
  if (rawSampleExportEnabled()) {
    rawSnapshot = samples;
  }
  std::sort(samples.begin(), samples.end());
  const std::size_t pIdx = samples.size() / 4U;
  const double p25 = samples[pIdx];
  const double opsPerSec = p25 > 0.0 ? 1.0e9 / p25 : 0.0;
  // err% reports MAD of the [p10, p50] window around p25, scaled to a
  // Gaussian-comparable sigma (1.4826). The window spans the lower half of
  // the sample distribution, capturing the spread that affects p25's
  // run-to-run stability. The narrower [p20, p30] window the prior
  // implementation used silently returned 0% for the standard kIterations=20
  // case (only 2 window samples; below the >= 4 statistical-validity gate),
  // hiding bimodal variance behind a `0.0%` headline. The [p10, p50] window
  // is wide enough to compute a meaningful MAD even at kIterations=12 and
  // surfaces the asymmetric spread when warmup-tail iterations differ from
  // the steady state.
  const std::size_t windowStart = (samples.size() * 10U) / 100U;
  const std::size_t windowEnd = (samples.size() * 50U) / 100U;
  double errPct = 0.0;
  if (windowEnd > windowStart && (windowEnd - windowStart) >= 2U && p25 > 0.0) {
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
  // When the tail-percentile flag is on, populate tailNs from the sorted
  // samples as {p25, p50, p99} in ns. Picks the floor index; linear
  // interpolation is overkill for the bench's sample sizes.
  if (tailPercentilesEnabled() && !samples.empty()) {
    auto pickPct = [&samples](double pct) -> double {
      const std::size_t maxIdx = samples.size() - 1U;
      const std::size_t idx =
          static_cast<std::size_t>((pct / 100.0) * static_cast<double>(maxIdx));
      return samples[idx];
    };
    row.tailNs =
        std::array<double, 3>{pickPct(25.0), pickPct(50.0), pickPct(99.0)};
  }
  if (!rawSnapshot.empty()) {
    row.rawSamplesNs = std::move(rawSnapshot);
  }
  return row;
}

// Relative standard deviation of |samples| as a percentage. Returns 0.0 for
// sample sizes below 2 or non-finite means.
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

// Centered 95 % bootstrap CI half-width for the median of |samples|, as a
// percentage of that median. Returns 0.0 for n < kBootstrapMinSamples so the
// caller can fall back to relativeStdDevPercent without negotiating a sentinel.
// Bootstrap is stable under multimodality and heavy tails; parametric stddev
// over-reports because the tail inflates the second moment (Kalibera & Jones,
// ISMM 2013).
constexpr std::size_t kBootstrapMinSamples = 30;

inline double bootstrapMedianCiPercent(std::vector<double> samples,
                                       std::size_t resamples = 1024,
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
    std::nth_element(resample.begin(),
                     resample.begin() + (resample.size() / 2U), resample.end());
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
