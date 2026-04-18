#pragma once

#include <array>
#include <cmath>
#include <iomanip>
#include <ios>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace citor::bench {

/// One competitor's measurement row in a comparative bench table.
///
/// `relative` is computed by `formatTable` against the row whose `name` matches
/// the chosen baseline; raw fields are filled by the workload runner.
struct BenchRow {
  /// Pool name shown in the rightmost column.
  std::string name;

  /// Median wall time per dispatch in nanoseconds.
  double nsPerOp = 0.0;

  /// Operations per second (1e9 / `nsPerOp`).
  double opsPerSec = 0.0;

  /// Relative standard deviation of `nsPerOp` (percentage).
  double errPercent = 0.0;
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
    if (row.name == baselineName) {
      baselineNs = row.nsPerOp;
      break;
    }
  }

  // Header row.
  out << std::left << std::setw(11) << "relative" << std::setw(10) << "ns/op" << std::setw(10)
      << "op/s" << std::setw(8) << "err%" << "workload: " << table.workload << '\n';

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
        << formatSiNumber(row.opsPerSec) << std::setw(8) << errOss.str() << row.name << '\n';
  }
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

} // namespace citor::bench
