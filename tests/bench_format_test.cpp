// Unit test for the bench-harness `bench_format.h` helpers that are tricky
// enough to warrant explicit coverage: `bootstrapMedianCiPercent` switches
// estimators at n=30, and `formatTable` toggles tail-percentile columns based
// on whether any row's `tailNs` is populated. The rest of `bench_format.h` is
// formatting glue covered by the bench's regression baseline.

#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "../benchmark/bench_format.h"

namespace {

TEST(BenchFormat, BootstrapBelowMinReturnsZero) {
  // n < 30 falls back to the parametric stddev path; the bootstrap helper
  // must return zero so the caller can detect the small-n case without
  // negotiating a sentinel.
  std::vector<double> samples;
  for (int i = 0; i < 29; ++i) {
    samples.push_back(static_cast<double>(100 + i));
  }
  EXPECT_EQ(citor::bench::bootstrapMedianCiPercent(samples), 0.0);
}

TEST(BenchFormat, BootstrapAboveMinReturnsHalfWidth) {
  // n >= 30 should produce a finite, positive half-width on a non-constant
  // sample. Tight constant-width samples should give a near-zero half-width
  // (the percentile bracket of a constant distribution is a point).
  std::vector<double> samples;
  for (int i = 0; i < 64; ++i) {
    samples.push_back(static_cast<double>(100 + (i % 8)));
  }
  const double pct = citor::bench::bootstrapMedianCiPercent(samples);
  EXPECT_GT(pct, 0.0);
  EXPECT_LT(pct, 100.0);
}

TEST(BenchFormat, BootstrapDegenerateInputReturnsZero) {
  // Constant samples produce a zero-width CI; non-finite mean (n < 2,
  // empty) also returns 0.0 so the err% column does not render NaN.
  std::vector<double> empty;
  EXPECT_EQ(citor::bench::bootstrapMedianCiPercent(empty), 0.0);

  std::vector<double> constant(64, 200.0);
  // The bootstrap on identical samples must produce a half-width of zero
  // (every resample's median is identical). This is the property the
  // err% column relies on for "stable" runs.
  EXPECT_DOUBLE_EQ(citor::bench::bootstrapMedianCiPercent(constant), 0.0);
}

TEST(BenchFormat, FormatTableNoTailColumnsWhenAllNullopt) {
  // The header must be byte-identical to the pre-tail format when every
  // row's `tailNs` is `std::nullopt`. Downstream awk parsers in the
  // downstream awk parsers key off `$2` (ns/op) and `$NF` (row name); the
  // header order being stable is the contract those parsers depend on.
  citor::bench::BenchTable table{.workload = "wl",
                                 .rows = {
                                     {.name = "citor::ThreadPool",
                                      .nsPerOp = 100.0,
                                      .opsPerSec = 1e7,
                                      .errPercent = 0.5,
                                      .tailNs = std::nullopt},
                                     {.name = "oneTBB",
                                      .nsPerOp = 200.0,
                                      .opsPerSec = 5e6,
                                      .errPercent = 0.7,
                                      .tailNs = std::nullopt},
                                 }};
  std::ostringstream oss;
  citor::bench::formatTable(table, "citor::ThreadPool", oss);
  const std::string out = oss.str();
  EXPECT_NE(out.find("workload: wl"), std::string::npos);
  EXPECT_EQ(out.find("p25"), std::string::npos);
  EXPECT_EQ(out.find("p50"), std::string::npos);
  EXPECT_EQ(out.find("p99"), std::string::npos);
}

TEST(BenchFormat, FormatTableEmitsTailColumnsWhenAnyRowPopulated) {
  citor::bench::BenchTable table{.workload = "wl",
                                 .rows = {
                                     {.name = "citor::ThreadPool",
                                      .nsPerOp = 100.0,
                                      .opsPerSec = 1e7,
                                      .errPercent = 0.5,
                                      .tailNs = std::array<double, 3>{90.0, 100.0, 250.0}},
                                     {.name = "oneTBB",
                                      .nsPerOp = 200.0,
                                      .opsPerSec = 5e6,
                                      .errPercent = 0.7,
                                      .tailNs = std::nullopt},
                                 }};
  std::ostringstream oss;
  citor::bench::formatTable(table, "citor::ThreadPool", oss);
  const std::string out = oss.str();
  EXPECT_NE(out.find("p25"), std::string::npos);
  EXPECT_NE(out.find("p50"), std::string::npos);
  EXPECT_NE(out.find("p99"), std::string::npos);
  // Rows with `tailNs == nullopt` render `-` placeholders in those slots so
  // every row has the same column count.
  EXPECT_NE(out.find('-'), std::string::npos);
}

} // namespace
