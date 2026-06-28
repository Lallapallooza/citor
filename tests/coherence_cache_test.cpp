#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "citor/coherence_cache.h"
#include "citor/detail/coherence_probe.h"
#include "citor/thread_pool.h"

using citor::exportCoherenceProbe;
using citor::importCoherenceProbe;
using citor::ThreadPool;
using citor::detail::CoherenceProbe;
using citor::detail::deserializeCoherenceProbe;
using citor::detail::seedCoherenceProbeCache;
using citor::detail::serializeCoherenceProbe;

namespace {

// A fully structurally consistent probe: NxN matrix over `cpus`, per-cpu
// cluster ids, and a numClusters x numClusters distance matrix. The
// distinctive ratio and cluster count make a seeded copy recognisable when a
// later pool reports it, proving the pool reused the seed rather than probing.
CoherenceProbe makeSentinelProbe(const std::vector<std::uint32_t> &cpus) {
  const std::size_t n = cpus.size();
  CoherenceProbe probe;
  probe.valid = true;
  probe.maxCrossOverIntraRatio = 13.75;

  probe.matrix.valid = true;
  probe.matrix.cpus = cpus;
  probe.matrix.matrix.assign(n, std::vector<double>(n, 0.0));
  for (std::size_t i = 0; i < n; ++i) {
    for (std::size_t j = 0; j < n; ++j) {
      probe.matrix.matrix[i][j] =
          (i == j) ? 0.0 : static_cast<double>((i + 1U) * (j + 1U));
    }
  }

  constexpr std::uint32_t kSentinelClusters = 2U;
  probe.clusters.numClusters = kSentinelClusters;
  probe.clusters.clusterIdOfCpuIndex.resize(n);
  for (std::size_t i = 0; i < n; ++i) {
    probe.clusters.clusterIdOfCpuIndex[i] =
        static_cast<std::uint32_t>(i % kSentinelClusters);
  }
  probe.clusters.clusterDistanceNs.assign(
      kSentinelClusters, std::vector<double>(kSentinelClusters, 0.0));
  probe.clusters.clusterDistanceNs[0][0] = 1.0;
  probe.clusters.clusterDistanceNs[0][1] = 22.0;
  probe.clusters.clusterDistanceNs[1][0] = 22.0;
  probe.clusters.clusterDistanceNs[1][1] = 2.0;
  return probe;
}

void expectProbesEqual(const CoherenceProbe &a, const CoherenceProbe &b) {
  EXPECT_EQ(a.valid, b.valid);
  EXPECT_EQ(a.maxCrossOverIntraRatio, b.maxCrossOverIntraRatio);
  EXPECT_EQ(a.matrix.valid, b.matrix.valid);
  EXPECT_EQ(a.matrix.cpus, b.matrix.cpus);
  ASSERT_EQ(a.matrix.matrix.size(), b.matrix.matrix.size());
  for (std::size_t i = 0; i < a.matrix.matrix.size(); ++i) {
    EXPECT_EQ(a.matrix.matrix[i], b.matrix.matrix[i]) << "row " << i;
  }
  EXPECT_EQ(a.clusters.numClusters, b.clusters.numClusters);
  EXPECT_EQ(a.clusters.clusterIdOfCpuIndex, b.clusters.clusterIdOfCpuIndex);
  EXPECT_EQ(a.clusters.clusterDistanceNs, b.clusters.clusterDistanceNs);
}

} // namespace

// A real pool's probe survives a serialise then parse round trip with every
// field intact: the bytes are non-empty and the reconstructed probe matches
// the source.
TEST(CoherenceCache, RoundTripPreservesProbe) {
  const ThreadPool pool(8);
  if (!pool.coherenceProbe().valid) {
    GTEST_SKIP() << "host pool ran no coherence probe (participants "
                 << pool.participants() << ")";
  }

  const std::vector<std::byte> bytes = exportCoherenceProbe(pool);
  EXPECT_FALSE(bytes.empty());

  CoherenceProbe parsed;
  ASSERT_TRUE(
      deserializeCoherenceProbe(std::span<const std::byte>(bytes), parsed));
  expectProbesEqual(pool.coherenceProbe(), parsed);
}

// Seeding the cache under a pool's cpuset makes the next pool with that cpuset
// adopt the seeded probe verbatim instead of running its own calibration.
TEST(CoherenceCache, SeedThenSkipReusesSeededProbe) {
  const ThreadPool reference(8);
  if (reference.participants() < 2U || !reference.coherenceProbe().valid) {
    GTEST_SKIP() << "host pool ran no coherence probe (participants "
                 << reference.participants() << ")";
  }

  const std::vector<std::uint32_t> cpus =
      reference.coherenceProbe().matrix.cpus;
  const CoherenceProbe sentinel = makeSentinelProbe(cpus);
  seedCoherenceProbeCache(cpus, sentinel);

  const ThreadPool seeded(8);
  ASSERT_EQ(seeded.coherenceProbe().matrix.cpus, cpus);
  expectProbesEqual(sentinel, seeded.coherenceProbe());
}

// Importing a blob produced by the serialiser seeds the cache and reports
// success; the embedded cpuset becomes a live cache key.
TEST(CoherenceCache, ImportSeedsCacheFromBlob) {
  const std::vector<std::uint32_t> cpus = {100U, 101U, 102U, 103U};
  const CoherenceProbe sentinel = makeSentinelProbe(cpus);
  const std::vector<std::byte> bytes = serializeCoherenceProbe(sentinel);

  EXPECT_TRUE(importCoherenceProbe(std::span<const std::byte>(bytes)));

  const CoherenceProbe cached = citor::detail::cachedCoherenceProbe(cpus, {});
  expectProbesEqual(sentinel, cached);
}

// Malformed input never throws and never seeds: empty, truncated, and
// wrong-magic blobs all report failure.
TEST(CoherenceCache, ImportRejectsMalformedInput) {
  const std::vector<std::uint32_t> cpus = {200U, 201U, 202U, 203U};
  const CoherenceProbe sentinel = makeSentinelProbe(cpus);
  const std::vector<std::byte> valid = serializeCoherenceProbe(sentinel);

  bool emptyResult = true;
  EXPECT_NO_THROW(emptyResult =
                      importCoherenceProbe(std::span<const std::byte>{}));
  EXPECT_FALSE(emptyResult);

  std::vector<std::byte> truncated(valid.begin(), valid.begin() + 6);
  bool truncatedResult = true;
  EXPECT_NO_THROW(truncatedResult = importCoherenceProbe(
                      std::span<const std::byte>(truncated)));
  EXPECT_FALSE(truncatedResult);

  std::vector<std::byte> wrongMagic = valid;
  wrongMagic[0] = static_cast<std::byte>(0xFFU);
  wrongMagic[1] = static_cast<std::byte>(0xFFU);
  bool wrongMagicResult = true;
  EXPECT_NO_THROW(wrongMagicResult = importCoherenceProbe(
                      std::span<const std::byte>(wrongMagic)));
  EXPECT_FALSE(wrongMagicResult);
}
