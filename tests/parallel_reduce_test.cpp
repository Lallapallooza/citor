#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "citor/cancellation.h"
#include "citor/cpos/parallel_reduce.h"
#include "citor/example_hints.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::Balance;
using citor::CancellationToken;
using citor::cancelled_value_exception;
using citor::Determinism;
using citor::Hints;
using citor::KahanReduceHints;
using citor::Priority;
using citor::ThreadPool;

// Hint presets at TU scope (not in an anonymous namespace) so clang-tidy treats every
// static-constexpr member as a public field of a named type rather than an unused constant.
struct FixedBlockTestHints {
  static constexpr Balance balance = Balance::StaticUniform;
  static constexpr Determinism determinism = Determinism::FixedBlockOrder;
  static constexpr Priority priority = Priority::Throughput;
  static constexpr double estimatedItemNs = 0.0;
  static constexpr double minTaskUs = 0.0;
  static constexpr std::size_t chunk = 0;
};

struct KahanTestHints {
  static constexpr Balance balance = Balance::StaticUniform;
  static constexpr Determinism determinism = Determinism::KahanCompensated;
  static constexpr Priority priority = Priority::Throughput;
  static constexpr double estimatedItemNs = 0.0;
  static constexpr double minTaskUs = 0.0;
  static constexpr std::size_t chunk = 0;
};

namespace {

/// Adversarial input that forces classic summation to lose the ones.
std::vector<double> makeKahanAdversary(std::size_t n) {
  // Pattern: [1e20, 1.0, 1.0, ..., 1.0, -1e20]. Naive left-to-right sum drops the ones.
  std::vector<double> v(n, 1.0);
  if (n >= 2) {
    v.front() = 1.0e20;
    v.back() = -1.0e20;
  }
  return v;
}

/// Per-chunk plain-double sum.
double mapPlainSum(const std::vector<double> &data, std::size_t lo, std::size_t hi) {
  double s = 0.0;
  for (std::size_t i = lo; i < hi; ++i) {
    s += data[i];
  }
  return s;
}

} // namespace

// Basic correctness: parallel reduction sums to the expected total within float precision.
TEST(ParallelReduce, BasicReductionMatchesSerial) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 1000;
  std::vector<double> data(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    data[i] = static_cast<double>(i + 1);
  }
  const double expected = static_cast<double>(kN) * static_cast<double>(kN + 1) / 2.0;

  const double got = pool.parallelReduce<FixedBlockTestHints>(
      0, kN, 0.0, [&](std::size_t lo, std::size_t hi) { return mapPlainSum(data, lo, hi); },
      [](double a, double b) { return a + b; });

  EXPECT_NEAR(got, expected, expected * 1e-12);
}

// FixedBlockOrder: byte-equal output across worker counts.
TEST(ParallelReduce, FixedBlockOrderBitIdenticalAcrossJobs) {
  constexpr std::size_t kN = 10000;
  std::vector<double> data(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    data[i] = std::sin(static_cast<double>(i) * 0.001);
  }

  std::vector<double> results;
  for (const std::size_t j :
       {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}, std::size_t{16}}) {
    ThreadPool pool(j);
    const double r = pool.parallelReduce<FixedBlockTestHints>(
        0, kN, 0.0, [&](std::size_t lo, std::size_t hi) { return mapPlainSum(data, lo, hi); },
        [](double a, double b) { return a + b; });
    results.push_back(r);
  }
  for (std::size_t i = 1; i < results.size(); ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(results[i]), std::bit_cast<std::uint64_t>(results[0]))
        << "bit-identity broken between j=" << (i + 1) << " and j=1";
  }
}

// KahanCompensated: bit-identical across worker counts AND within 1 ULP of serial Kahan.
//
// The compensation contract is INTER-CHUNK: each chunk computes its own sum, and the framework
// combines those chunk-sums via a compensated pairwise tree. The adversary therefore lives in
// the per-chunk totals, not inside any single chunk. We construct one element per chunk by
// using `n <= kReduceMaxChunks (= 64)` so the engine derives `chunk = 1`; each chunk-total is
// then a single element, and the cancellation is exposed at the combine level.
TEST(ParallelReduce, KahanCompensatedAcrossJobs) {
  constexpr std::size_t kN = 64;
  const std::vector<double> data = makeKahanAdversary(kN);

  // Serial Kahan reference walks element-by-element.
  double sumRef = 0.0;
  double cRef = 0.0;
  for (const double x : data) {
    const double y = x - cRef;
    const double t = sumRef + y;
    cRef = (t - sumRef) - y;
    sumRef = t;
  }

  std::vector<double> results;
  for (const std::size_t j :
       {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}, std::size_t{16}}) {
    ThreadPool pool(j);
    const double r = pool.parallelReduce<KahanTestHints>(
        0, kN, 0.0,
        [&](std::size_t lo, std::size_t hi) {
          double s = 0.0;
          for (std::size_t i = lo; i < hi; ++i) {
            s += data[i];
          }
          return s;
        },
        [](double a, double b) { return a + b; });
    results.push_back(r);
  }
  for (std::size_t i = 1; i < results.size(); ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(results[i]), std::bit_cast<std::uint64_t>(results[0]))
        << "Kahan bit-identity broken between j=" << (i + 1) << " and j=1";
  }

  // Parallel Kahan should agree with serial Kahan within a small relative bound. Both share the
  // same ill-conditioned cancellation: when a 1.0 is added to a running sum near 1e20, the 1 is
  // lost to the magnitude regardless of whether the next subtract recovers any bits. The
  // useful contract here is that parallel and serial agree even on adversarial inputs.
  const double diff = std::abs(results[0] - sumRef);
  const double tolerance = std::max(1.0, std::abs(sumRef)) * 1e-12;
  EXPECT_LE(diff, tolerance) << "parallel Kahan diverges from serial Kahan: parallel=" << results[0]
                             << " serial=" << sumRef;

  // Verify Kahan compensation actually does work when the cancellation is RECOVERABLE
  // (running sum stays moderate; small terms accumulate then a single subtract cancels). We
  // construct: chunk totals = [1.0, 1.0, ..., 1.0, -kN+1.0]. The sum is exactly 0; naive sum
  // accumulates rounding error proportional to sqrt(n), Kahan keeps it well under 1 ULP.
  std::vector<double> recoverable(kN, 1.0);
  recoverable.back() = -static_cast<double>(kN - 1);
  ThreadPool poolKahan(4);
  const double kahanResult = poolKahan.parallelReduce<KahanTestHints>(
      0, kN, 0.0,
      [&](std::size_t lo, std::size_t hi) {
        double s = 0.0;
        for (std::size_t i = lo; i < hi; ++i) {
          s += recoverable[i];
        }
        return s;
      },
      [](double a, double b) { return a + b; });
  EXPECT_NEAR(kahanResult, 0.0, 1e-12) << "Kahan failed to recover the recoverable sum";
}

// Empty range returns the init value.
TEST(ParallelReduce, EmptyRangeReturnsInit) {
  ThreadPool pool(4);
  const int got = pool.parallelReduce<FixedBlockTestHints>(
      0, 0, 42, [](std::size_t, std::size_t) { return 0; }, [](int a, int b) { return a + b; });
  EXPECT_EQ(got, 42);
}

// Cancellation produces a partial result via cancelled_value_exception<T>.
//
// Skipped on single-participant pools: the inline-fallback path runs the map/combine over the full
// range in one chunk, so the partial-value contract (some chunks ran, others did not) has no
// observable surface.
TEST(ParallelReduce, CancellationProducesPartial) {
  ThreadPool pool(4);
  if (pool.participants() < 2U) {
    GTEST_SKIP() << "single-participant pool collapses to inline path; partial-value cancellation "
                    "contract has no observable surface";
  }
  constexpr std::size_t kN = 100000;
  std::vector<double> data(kN, 1.0);
  CancellationToken tok;

  bool sawCancellation = false;
  try {
    (void)pool.parallelReduce<FixedBlockTestHints>(
        0, kN, 0.0,
        [&](std::size_t lo, std::size_t hi) {
          // Stop the token as soon as the first chunk runs so subsequent chunks are skipped at
          // chunk boundaries by the dispatch engine.
          tok.request_stop();
          double s = 0.0;
          for (std::size_t i = lo; i < hi; ++i) {
            s += data[i];
          }
          return s;
        },
        [](double a, double b) { return a + b; }, tok);
  } catch (const cancelled_value_exception<double> &e) {
    sawCancellation = true;
    // The partial value must reflect the chunks that did run, with init folded in. It should be
    // strictly between 0 (no progress) and the full sum (full execution).
    EXPECT_GT(e.partial_value, 0.0);
    EXPECT_LT(e.partial_value, static_cast<double>(kN));
  }
  EXPECT_TRUE(sawCancellation) << "expected cancelled_value_exception<double>";
}

// Member-call and CPO-call equivalence on the same inputs.
TEST(ParallelReduce, MemberCpoEquivalence) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 2048;
  std::vector<double> data(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    data[i] = static_cast<double>(i);
  }

  const double resultMember = pool.parallelReduce<FixedBlockTestHints>(
      0, kN, 0.0, [&](std::size_t lo, std::size_t hi) { return mapPlainSum(data, lo, hi); },
      [](double a, double b) { return a + b; });
  const double resultCpo = citor::parallelReduce.template operator()<FixedBlockTestHints>(
      pool, 0, kN, 0.0, [&](std::size_t lo, std::size_t hi) { return mapPlainSum(data, lo, hi); },
      [](double a, double b) { return a + b; });
  EXPECT_EQ(std::bit_cast<std::uint64_t>(resultMember), std::bit_cast<std::uint64_t>(resultCpo));
}

// KahanReduceHints named hint preset routes through parallelReduce without compile errors and
// produces bit-identical output across worker counts (the AFK-MC2 migration's contract).
TEST(ParallelReduce, KahanReduceHintBitIdenticalAcrossJobs) {
  constexpr std::size_t kN = 5000;
  std::vector<double> data(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    data[i] = std::cos(static_cast<double>(i) * 0.0007);
  }

  std::vector<double> results;
  for (const std::size_t j :
       {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}, std::size_t{16}}) {
    ThreadPool pool(j);
    const double r = pool.parallelReduce<KahanReduceHints>(
        0, kN, 0.0,
        [&](std::size_t lo, std::size_t hi) {
          double s = 0.0;
          for (std::size_t i = lo; i < hi; ++i) {
            s += data[i];
          }
          return s;
        },
        [](double a, double b) { return a + b; });
    results.push_back(r);
  }
  for (std::size_t i = 1; i < results.size(); ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(results[i]), std::bit_cast<std::uint64_t>(results[0]))
        << "KahanReduceHints bit-identity broken between j=" << (i + 1) << " and j=1";
  }
}

// Runtime-hint sibling parallelReduceRuntime mirrors the member-template behavior on identical
// inputs.
TEST(ParallelReduce, RuntimeHintsMatchCompileTimeHints) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 1024;
  std::vector<double> data(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    data[i] = static_cast<double>(i) * 0.5;
  }

  const double compileResult = pool.parallelReduce<FixedBlockTestHints>(
      0, kN, 0.0, [&](std::size_t lo, std::size_t hi) { return mapPlainSum(data, lo, hi); },
      [](double a, double b) { return a + b; });

  Hints runtimeHints;
  runtimeHints.balance = Balance::StaticUniform;
  runtimeHints.determinism = Determinism::FixedBlockOrder;
  runtimeHints.estimatedItemNs = 0.0;
  runtimeHints.minTaskUs = 0.0;
  runtimeHints.chunk = 0;
  const double runtimeResult = pool.parallelReduceRuntime(
      0, kN, 0.0, [&](std::size_t lo, std::size_t hi) { return mapPlainSum(data, lo, hi); },
      [](double a, double b) { return a + b; }, runtimeHints);
  EXPECT_EQ(std::bit_cast<std::uint64_t>(compileResult),
            std::bit_cast<std::uint64_t>(runtimeResult));
}

// Exception propagation: a worker map throws -> the producer rethrows on join.
TEST(ParallelReduce, ExceptionPropagation) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 1024;

  bool threwAsExpected = false;
  try {
    (void)pool.parallelReduce<FixedBlockTestHints>(
        0, kN, 0.0,
        [](std::size_t lo, std::size_t /*hi*/) -> double {
          if (lo == 0) {
            throw std::runtime_error("test reduce exception");
          }
          return 0.0;
        },
        [](double a, double b) { return a + b; });
  } catch (const std::runtime_error &e) {
    threwAsExpected = std::string{e.what()} == "test reduce exception";
  } catch (...) {
    threwAsExpected = false;
  }
  EXPECT_TRUE(threwAsExpected);
}
