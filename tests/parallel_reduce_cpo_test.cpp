#include <gtest/gtest.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "citor/cpos/parallel_reduce.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::Balance;
using citor::Determinism;
using citor::Hints;
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

// Member-call and CPO-call equivalence on the same inputs.
TEST(ParallelReduceCpo, MemberCallAndCpoCallProduceBitIdenticalSum) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 2048;
  std::vector<double> data(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    data[i] = static_cast<double>(i);
  }

  const double resultMember = pool.parallelReduce<HintsDefaults>(
      0, kN, 0.0,
      [&](std::size_t lo, std::size_t hi) { return mapPlainSum(data, lo, hi); },
      [](double a, double b) { return a + b; });
  const double resultCpo =
      citor::parallelReduce.template operator()<HintsDefaults>(
          pool, 0, kN, 0.0,
          [&](std::size_t lo, std::size_t hi) {
            return mapPlainSum(data, lo, hi);
          },
          [](double a, double b) { return a + b; });
  EXPECT_EQ(std::bit_cast<std::uint64_t>(resultMember),
            std::bit_cast<std::uint64_t>(resultCpo));
}

// Runtime-hint sibling parallelReduceRuntime mirrors the member-template
// behavior on identical inputs.
TEST(ParallelReduceCpo, RuntimeHintsSiblingProducesSameSumAsCompileTimeHints) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 1024;
  std::vector<double> data(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    data[i] = static_cast<double>(i) * 0.5;
  }

  const double compileResult = pool.parallelReduce<HintsDefaults>(
      0, kN, 0.0,
      [&](std::size_t lo, std::size_t hi) { return mapPlainSum(data, lo, hi); },
      [](double a, double b) { return a + b; });

  Hints runtimeHints;
  runtimeHints.balance = Balance::StaticUniform;
  runtimeHints.determinism = Determinism::FixedBlockOrder;
  runtimeHints.estimatedItemNs = 0.0;
  runtimeHints.minTaskUs = 0.0;
  runtimeHints.chunk = 0;
  const double runtimeResult = pool.parallelReduceRuntime(
      0, kN, 0.0,
      [&](std::size_t lo, std::size_t hi) { return mapPlainSum(data, lo, hi); },
      [](double a, double b) { return a + b; }, runtimeHints);
  EXPECT_EQ(std::bit_cast<std::uint64_t>(compileResult),
            std::bit_cast<std::uint64_t>(runtimeResult));
}
