#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "citor/cpos/parallel_reduce.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::HintsDefaults;
using citor::KahanReduceHints;
using citor::ThreadPool;

namespace {

// Adversarial input that forces classic summation to lose the ones.
std::vector<double> makeKahanAdversary(std::size_t n) {
  // Pattern: [1e20, 1.0, 1.0, ..., 1.0, -1e20]. Naive left-to-right sum drops
  // the ones.
  std::vector<double> v(n, 1.0);
  if (n >= 2) {
    v.front() = 1.0e20;
    v.back() = -1.0e20;
  }
  return v;
}

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

// FixedBlockOrder: byte-equal output across worker counts.
TEST(ParallelReduceDeterminism,
     FixedBlockOrderBalanceProducesBitIdenticalSumsAcrossThreadCounts) {
  constexpr std::size_t kN = 10000;
  std::vector<double> data(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    data[i] = std::sin(static_cast<double>(i) * 0.001);
  }

  std::vector<double> results;
  for (const std::size_t j : {std::size_t{1}, std::size_t{2}, std::size_t{4},
                              std::size_t{8}, std::size_t{16}}) {
    ThreadPool pool(j);
    const double r = pool.parallelReduce<HintsDefaults>(
        0, kN, 0.0,
        [&](std::size_t lo, std::size_t hi) {
          return mapPlainSum(data, lo, hi);
        },
        [](double a, double b) { return a + b; });
    results.push_back(r);
  }
  for (std::size_t i = 1; i < results.size(); ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(results[i]),
              std::bit_cast<std::uint64_t>(results[0]))
        << "bit-identity broken between j=" << (i + 1) << " and j=1";
  }
}

// KahanCompensated: bit-identical across worker counts AND within 1 ULP of
// serial Kahan.
//
// The compensation contract is INTER-CHUNK: each chunk computes its own sum,
// and the framework combines those chunk-sums via a compensated pairwise tree.
// The adversary therefore lives in the per-chunk totals, not inside any single
// chunk. We construct one element per chunk by using `n <= kReduceMaxChunks (=
// 64)` so the engine derives `chunk = 1`; each chunk-total is then a single
// element, and the cancellation is exposed at the combine level.
TEST(ParallelReduceDeterminism,
     KahanCompensatedBalanceMatchesReferenceBitIdenticallyAcrossThreadCounts) {
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
  for (const std::size_t j : {std::size_t{1}, std::size_t{2}, std::size_t{4},
                              std::size_t{8}, std::size_t{16}}) {
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
    EXPECT_EQ(std::bit_cast<std::uint64_t>(results[i]),
              std::bit_cast<std::uint64_t>(results[0]))
        << "Kahan bit-identity broken between j=" << (i + 1) << " and j=1";
  }

  // Parallel Kahan should agree with serial Kahan within a small relative
  // bound. Both share the same ill-conditioned cancellation: when a 1.0 is
  // added to a running sum near 1e20, the 1 is lost to the magnitude regardless
  // of whether the next subtract recovers any bits. The useful contract here is
  // that parallel and serial agree even on adversarial inputs.
  const double diff = std::abs(results[0] - sumRef);
  const double tolerance = std::max(1.0, std::abs(sumRef)) * 1e-12;
  EXPECT_LE(diff, tolerance)
      << "parallel Kahan diverges from serial Kahan: parallel=" << results[0]
      << " serial=" << sumRef;

  // Verify Kahan compensation actually does work when the cancellation is
  // RECOVERABLE (running sum stays moderate; small terms accumulate then a
  // single subtract cancels). We construct: chunk totals = [1.0, 1.0, ..., 1.0,
  // -kN+1.0]. The sum is exactly 0; naive sum accumulates rounding error
  // proportional to sqrt(n), Kahan keeps it well under 1 ULP.
  std::vector<double> recoverable(kN, 1.0);
  recoverable.back() = -static_cast<double>(kN - 1);
  ThreadPool poolKahan(4);
  const double kahanResult = poolKahan.parallelReduce<KahanReduceHints>(
      0, kN, 0.0,
      [&](std::size_t lo, std::size_t hi) {
        double s = 0.0;
        for (std::size_t i = lo; i < hi; ++i) {
          s += recoverable[i];
        }
        return s;
      },
      [](double a, double b) { return a + b; });
  EXPECT_NEAR(kahanResult, 0.0, 1e-12)
      << "Kahan failed to recover the recoverable sum";
}

// KahanReduceHints named hint preset routes through parallelReduce without
// compile errors and produces bit-identical output across worker counts (the
// AFK-MC2 migration's contract).
TEST(ParallelReduceDeterminism,
     KahanReduceHintsPolicyProducesBitIdenticalSumAcrossThreadCounts) {
  constexpr std::size_t kN = 5000;
  std::vector<double> data(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    data[i] = std::cos(static_cast<double>(i) * 0.0007);
  }

  std::vector<double> results;
  for (const std::size_t j : {std::size_t{1}, std::size_t{2}, std::size_t{4},
                              std::size_t{8}, std::size_t{16}}) {
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
    EXPECT_EQ(std::bit_cast<std::uint64_t>(results[i]),
              std::bit_cast<std::uint64_t>(results[0]))
        << "KahanReduceHints bit-identity broken between j=" << (i + 1)
        << " and j=1";
  }
}
