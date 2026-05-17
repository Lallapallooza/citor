#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <vector>

#include "citor/detail/coherence_probe.h"
#include "citor/detail/topology.h"

using citor::detail::CoherenceProbe;
using citor::detail::detectTopology;
using citor::detail::runCoherenceProbe;
using citor::detail::Topology;

namespace {

// Four CPUs exercise the disjoint-pair scheduler and cluster-detection
// path with bounded wall time on every host.
constexpr std::size_t kSmokeProbeCpuCap = 4;

std::vector<std::uint32_t> cappedPhysicalCores(const Topology &topo) noexcept {
  std::vector<std::uint32_t> cpus;
  cpus.reserve(kSmokeProbeCpuCap);
  for (const std::uint32_t cpu : topo.physicalCores) {
    if (cpus.size() >= kSmokeProbeCpuCap) {
      break;
    }
    cpus.push_back(cpu);
  }
  return cpus;
}

} // namespace

// A 4-CPU probe finishes well under a second on every supported host.
// A hang is the canary that `coherenceProbePin` is a no-op on the
// current platform: helpers oversubscribe the host and the round-robin
// schedule never converges to a stable median.
TEST(CoherenceProbe, FourCpuProbeMeetsTimeBudget) {
  const Topology topo = detectTopology();
  if (topo.physicalCores.size() < 2U) {
    GTEST_SKIP() << "Need at least two physical cores to run the probe; "
                    "process affinity reports "
                 << topo.physicalCores.size();
  }

  const std::vector<std::uint32_t> probeCpus = cappedPhysicalCores(topo);
  ASSERT_GE(probeCpus.size(), std::size_t{2});

  const auto t0 = std::chrono::steady_clock::now();
  const CoherenceProbe probe =
      runCoherenceProbe(probeCpus, topo.ccdGroups, /*roundTrips=*/256U);
  const auto t1 = std::chrono::steady_clock::now();
  const double elapsedMs =
      std::chrono::duration<double, std::milli>(t1 - t0).count();

  // Walking N-1 disjoint-pair rounds with 256 round-trips per pair on a
  // four-CPU subset finishes in tens of milliseconds on bare metal. The
  // 1500 ms cap is loose enough to tolerate a debug build, a virtualised
  // host with vCPU stealing, and a TSan run, while still failing fast if
  // the pin path is broken and the probe never settles.
  EXPECT_LT(elapsedMs, 1500.0)
      << "Probe of " << probeCpus.size() << " CPUs took " << elapsedMs
      << " ms; pin path may be a no-op on this host";

  EXPECT_TRUE(probe.valid);
  EXPECT_TRUE(probe.matrix.valid);
  EXPECT_EQ(probe.matrix.cpus.size(), probeCpus.size());
  EXPECT_EQ(probe.matrix.matrix.size(), probeCpus.size());
  for (const auto &row : probe.matrix.matrix) {
    EXPECT_EQ(row.size(), probeCpus.size());
  }
}

// The matrix is symmetric (we set `[i][j] == [j][i]` after each pair
// measurement) and the diagonal is zero (we never measure a CPU against
// itself). A broken pin path can yield asymmetric or non-zero-diagonal
// output.
TEST(CoherenceProbe, MatrixIsSymmetricWithZeroDiagonal) {
  const Topology topo = detectTopology();
  if (topo.physicalCores.size() < 2U) {
    GTEST_SKIP() << "Need at least two physical cores";
  }

  const std::vector<std::uint32_t> probeCpus = cappedPhysicalCores(topo);
  const CoherenceProbe probe =
      runCoherenceProbe(probeCpus, topo.ccdGroups, /*roundTrips=*/256U);
  ASSERT_TRUE(probe.matrix.valid);

  for (std::size_t i = 0; i < probe.matrix.matrix.size(); ++i) {
    EXPECT_EQ(probe.matrix.matrix[i][i], 0.0) << "diagonal cpu " << i;
    for (std::size_t j = i + 1; j < probe.matrix.matrix.size(); ++j) {
      EXPECT_DOUBLE_EQ(probe.matrix.matrix[i][j], probe.matrix.matrix[j][i])
          << "asymmetric pair " << i << "," << j;
    }
  }
}
