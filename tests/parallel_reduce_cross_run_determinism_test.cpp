#include <gtest/gtest.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::HintsDefaults;
using citor::KahanReduceHints;
using citor::ThreadPool;

namespace {

// Build the same data buffer every time from a fixed RNG seed. Two process
// invocations that hit this function with the same `seed` must observe
// byte-identical buffers; this is the foundation for the cross-run determinism
// gate -- the data feed is process-independent so the only remaining source of
// variation is the reduction engine itself.
std::vector<double> seededData(std::size_t n, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::uniform_real_distribution<double> dist(-1.0, 1.0);
  std::vector<double> out(n);
  for (std::size_t i = 0; i < n; ++i) {
    out[i] = dist(rng);
  }
  return out;
}

// Drive one parallelReduce<FixedBlockHints> over `data`. The combine identity
// is `+`.
double driveFixedBlockReduce(ThreadPool &pool,
                             const std::vector<double> &data) {
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

// Drive one parallelReduce<KahanReduceHints> (KahanCompensated) over `data`.
double driveKahanReduce(ThreadPool &pool, const std::vector<double> &data) {
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

// Cross-run determinism: a fixed seed feeds a deterministic data buffer; for
// each `nJobs` in {1,2,4,8,16}, repeat the reduction 10 times and assert the
// f64 bit pattern is stable. Within a single process invocation the result must
// be byte-identical across repeats; across two process invocations the same
// property holds because (a) the seed produces the same buffer, (b) the
// reduction engine's chunk shape is `n`-determined, and (c) the chunk-id
// pairwise tree's combine order is fixed. The within-nJobs repeats here
// therefore validate the cross-run invariant up to the inputs.
TEST(ParallelReduceCrossRunDeterminism, FixedBlockOrderStableAcrossRepeats) {
  constexpr std::size_t kN = 4096;
  constexpr int kRepeats = 10;
  constexpr std::uint64_t kSeed = 0x4243'4445'4647'4849ULL;
  const std::vector<double> data = seededData(kN, kSeed);

  for (const std::size_t j : {std::size_t{1}, std::size_t{2}, std::size_t{4},
                              std::size_t{8}, std::size_t{16}}) {
    ThreadPool pool(j);
    const double reference = driveFixedBlockReduce(pool, data);
    for (int rep = 1; rep < kRepeats; ++rep) {
      const double current = driveFixedBlockReduce(pool, data);
      ASSERT_EQ(std::bit_cast<std::uint64_t>(reference),
                std::bit_cast<std::uint64_t>(current))
          << "FixedBlockOrder cross-run repeatability broke at j=" << j
          << " rep=" << rep;
    }
  }
}

// Cross-run determinism for the Kahan path: the same data and seed produce the
// same Kahan tree result on every invocation at every `nJobs`.
TEST(ParallelReduceCrossRunDeterminism, KahanCompensatedStableAcrossRepeats) {
  constexpr std::size_t kN = 4096;
  constexpr int kRepeats = 10;
  constexpr std::uint64_t kSeed = 0x5152'5354'5556'5758ULL;
  const std::vector<double> data = seededData(kN, kSeed);

  for (const std::size_t j : {std::size_t{1}, std::size_t{2}, std::size_t{4},
                              std::size_t{8}, std::size_t{16}}) {
    ThreadPool pool(j);
    const double reference = driveKahanReduce(pool, data);
    for (int rep = 1; rep < kRepeats; ++rep) {
      const double current = driveKahanReduce(pool, data);
      ASSERT_EQ(std::bit_cast<std::uint64_t>(reference),
                std::bit_cast<std::uint64_t>(current))
          << "Kahan cross-run repeatability broke at j=" << j << " rep=" << rep;
    }
  }
}

// Cross-`nJobs` bit identity at fixed seed: at the same input, every `nJobs`
// produces the same result. This complements the existing
// `parallel_reduce_determinism_test.cpp` cross-`nJobs` gate by sourcing the
// inputs from a fixed RNG seed -- the gate is unchanged but the data feed
// matches the cross-process scenario.
TEST(ParallelReduceCrossRunDeterminism, FixedBlockOrderBitIdenticalAcrossJobs) {
  constexpr std::size_t kN = 8192;
  constexpr std::uint64_t kSeed = 0x6162'6364'6566'6768ULL;
  const std::vector<double> data = seededData(kN, kSeed);

  std::vector<double> resultsPerJ;
  for (const std::size_t j : {std::size_t{1}, std::size_t{2}, std::size_t{4},
                              std::size_t{8}, std::size_t{16}}) {
    ThreadPool pool(j);
    resultsPerJ.push_back(driveFixedBlockReduce(pool, data));
  }
  for (std::size_t i = 1; i < resultsPerJ.size(); ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(resultsPerJ[i]),
              std::bit_cast<std::uint64_t>(resultsPerJ[0]))
        << "FixedBlockOrder cross-`nJobs` bit-identity broke at idx=" << i;
  }
}

// Cross-`nJobs` bit identity for the Kahan path under a fixed seed.
TEST(ParallelReduceCrossRunDeterminism,
     KahanCompensatedBitIdenticalAcrossJobs) {
  constexpr std::size_t kN = 8192;
  constexpr std::uint64_t kSeed = 0x7172'7374'7576'7778ULL;
  const std::vector<double> data = seededData(kN, kSeed);

  std::vector<double> resultsPerJ;
  for (const std::size_t j : {std::size_t{1}, std::size_t{2}, std::size_t{4},
                              std::size_t{8}, std::size_t{16}}) {
    ThreadPool pool(j);
    resultsPerJ.push_back(driveKahanReduce(pool, data));
  }
  for (std::size_t i = 1; i < resultsPerJ.size(); ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(resultsPerJ[i]),
              std::bit_cast<std::uint64_t>(resultsPerJ[0]))
        << "Kahan cross-`nJobs` bit-identity broke at idx=" << i;
  }
}
