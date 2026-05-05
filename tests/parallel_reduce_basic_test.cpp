#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#include "citor/cpos/parallel_reduce.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::HintsDefaults;
using citor::ThreadPool;

namespace {

// Per-chunk plain-double sum.
double mapPlainSum(const std::vector<double> &data, std::size_t lo,
                   std::size_t hi) {
  double s = 0.0;
  for (std::size_t i = lo; i < hi; ++i) {
    s += data[i];
  }
  return s;
}

} // namespace

// Basic correctness: parallel reduction sums to the expected total within float
// precision.
TEST(ParallelReduceBasic,
     SumOfArithmeticSequenceMatchesClosedFormWithinFloatPrecision) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 1000;
  std::vector<double> data(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    data[i] = static_cast<double>(i + 1);
  }
  const double expected =
      static_cast<double>(kN) * static_cast<double>(kN + 1) / 2.0;

  const double got = pool.parallelReduce<HintsDefaults>(
      0, kN, 0.0,
      [&](std::size_t lo, std::size_t hi) { return mapPlainSum(data, lo, hi); },
      [](double a, double b) { return a + b; });

  EXPECT_NEAR(got, expected, expected * 1e-12);
}

// Empty range returns the init value.
TEST(ParallelReduceBasic, EmptyRangeReturnsInitValueUnchanged) {
  ThreadPool pool(4);
  const int got = pool.parallelReduce<HintsDefaults>(
      0, 0, 42, [](std::size_t, std::size_t) { return 0; },
      [](int a, int b) { return a + b; });
  EXPECT_EQ(got, 42);
}
