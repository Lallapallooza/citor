#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::HintsDefaults;
using citor::KahanReduceHints;
using citor::ThreadPool;

namespace {

double driveOneReduction(ThreadPool &pool, const std::vector<double> &data) {
  return pool.parallelReduce<HintsDefaults>(
      0, data.size(), 0.0,
      [&data](std::size_t lo, std::size_t hi) {
        double s = 0.0;
        for (std::size_t i = lo; i < hi; ++i) {
          s += data[i];
        }
        return s;
      },
      [](double a, double b) { return a + b; });
}

double driveOneKahanReduction(ThreadPool &pool,
                              const std::vector<double> &data) {
  return pool.parallelReduce<KahanReduceHints>(
      0, data.size(), 0.0,
      [&data](std::size_t lo, std::size_t hi) {
        double s = 0.0;
        for (std::size_t i = lo; i < hi; ++i) {
          s += data[i];
        }
        return s;
      },
      [](double a, double b) { return a + b; });
}

} // namespace

// Stress-test bit-identity across many repeated reductions at fixed worker
// count.
TEST(ParallelReduceDeterminism, FixedBlockBitIdenticalUnderRepeats) {
  constexpr std::size_t kN = 5000;
  constexpr int kRepeats = 1000;
  std::vector<double> data(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    data[i] = std::sin(static_cast<double>(i) * 0.0013);
  }

  ThreadPool pool(8);
  const double reference = driveOneReduction(pool, data);
  for (int rep = 1; rep < kRepeats; ++rep) {
    const double current = driveOneReduction(pool, data);
    ASSERT_EQ(std::bit_cast<std::uint64_t>(reference),
              std::bit_cast<std::uint64_t>(current))
        << "FixedBlock parallelReduce reproducibility broke at repetition "
        << rep;
  }
}

// Stress-test bit-identity across worker counts using the named
// KahanReduceHints hint preset.
TEST(ParallelReduceDeterminism, KahanReduceBitIdenticalAcrossManyJobs) {
  constexpr std::size_t kN = 5000;
  constexpr int kRepeats = 200;
  std::vector<double> data(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    data[i] = std::cos(static_cast<double>(i) * 0.0019);
  }

  std::vector<double> referencesPerJ;
  for (const std::size_t j : {std::size_t{1}, std::size_t{2}, std::size_t{4},
                              std::size_t{8}, std::size_t{16}}) {
    ThreadPool pool(j);
    const double reference = driveOneKahanReduction(pool, data);
    for (int rep = 1; rep < kRepeats; ++rep) {
      const double current = driveOneKahanReduction(pool, data);
      ASSERT_EQ(std::bit_cast<std::uint64_t>(reference),
                std::bit_cast<std::uint64_t>(current))
          << "KahanReduceHints reproducibility broke at j=" << j
          << " rep=" << rep;
    }
    referencesPerJ.push_back(reference);
  }
  for (std::size_t i = 1; i < referencesPerJ.size(); ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(referencesPerJ[i]),
              std::bit_cast<std::uint64_t>(referencesPerJ[0]))
        << "KahanReduceHints cross-`nJobs` bit-identity broken at idx=" << i;
  }
}
