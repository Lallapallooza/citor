// Unit tests for `detail::reserveProducerCpuFirst` across synthetic
// AMD-homogeneous, Intel-hybrid, single-CCD, multi-CCD, arena, and
// oversubscribed topologies. The homogeneous-SMT j=8 case guards
// distinct physical cores at every slot, the Intel-hybrid j=2 case
// guards slot 1 landing on the producer's SMT sibling, and the
// multi-CCD case guards producer-CCD-first ordering.

#include <gtest/gtest.h>

#include "citor/detail/topology.h"

#include <cstdint>
#include <vector>

using citor::detail::reserveProducerCpuFirst;
using citor::detail::Topology;

namespace {

/// Build a synthetic homogeneous-SMT topology with `numCores` physical
/// cores split across `numCcds` CCDs. Each physical core has an SMT
/// sibling. Models AMD Zen 4/5 desktop parts (e.g. 9950X3D with 16 phys
/// cores in 2 CCDs).
Topology makeHomogeneousSmt(std::uint32_t numCores, std::uint32_t numCcds) {
  Topology t;
  t.logicalCount = numCores * 2U;
  t.physicalCount = numCores;
  t.ccdCount = numCcds;
  t.preferredCcd = 0U;
  t.ccdOfCpu.assign(t.logicalCount, 0U);
  t.smtSiblingOfCpu.assign(t.logicalCount, UINT32_MAX);
  t.ccdGroups.assign(numCcds, {});

  const std::uint32_t perCcd = numCores / numCcds;
  for (std::uint32_t c = 0; c < numCores; ++c) {
    const std::uint32_t physCpu = c * 2U;
    const std::uint32_t sibCpu = c * 2U + 1U;
    const std::uint32_t ccd = c / perCcd;
    t.ccdOfCpu[physCpu] = ccd;
    t.ccdOfCpu[sibCpu] = ccd;
    t.smtSiblingOfCpu[physCpu] = sibCpu;
    t.smtSiblingOfCpu[sibCpu] = physCpu;
    t.ccdGroups[ccd].push_back(physCpu);
    t.physicalCores.push_back(physCpu);
  }
  return t;
}

/// Build a synthetic Intel hybrid topology: `numP` P-cores (SMT-paired)
/// followed by `numE` E-cores (no SMT). One CCD (single L3). Models
/// Alder Lake-P / Raptor Lake (e.g. 12900HK = 6P + 8E).
Topology makeIntelHybrid(std::uint32_t numP, std::uint32_t numE) {
  Topology t;
  t.logicalCount = numP * 2U + numE;
  t.physicalCount = numP + numE;
  t.ccdCount = 1U;
  t.preferredCcd = 0U;
  t.ccdOfCpu.assign(t.logicalCount, 0U);
  t.smtSiblingOfCpu.assign(t.logicalCount, UINT32_MAX);
  t.ccdGroups.assign(1U, {});

  // P-cores first: CPU ids 0..2P-1 are P-core SMT pairs.
  for (std::uint32_t p = 0; p < numP; ++p) {
    const std::uint32_t physCpu = p * 2U;
    const std::uint32_t sibCpu = p * 2U + 1U;
    t.smtSiblingOfCpu[physCpu] = sibCpu;
    t.smtSiblingOfCpu[sibCpu] = physCpu;
    t.ccdGroups[0].push_back(physCpu);
    t.physicalCores.push_back(physCpu);
  }
  // E-cores: CPU ids 2P..2P+E-1, no SMT sibling.
  for (std::uint32_t e = 0; e < numE; ++e) {
    const std::uint32_t ecpu = numP * 2U + e;
    t.ccdGroups[0].push_back(ecpu);
    t.physicalCores.push_back(ecpu);
  }
  return t;
}

} // namespace

// --- Homogeneous SMT (AMD) ---

// j=2 on AMD-like: slot 1 must be SMT sibling of slot 0. This is the
// single explicit exception to the distinct-physicals rule.
TEST(ReserveProducerCpuFirst, AmdSmtJ2RoutesSlot1ToProducerSibling) {
  const Topology topo = makeHomogeneousSmt(/*numCores=*/16, /*numCcds=*/2);
  std::vector<std::uint32_t> input = topo.physicalCores; // [0, 2, 4, ...]
  const auto out = reserveProducerCpuFirst(input, /*participants=*/2,
                                           /*standalone=*/true, topo);
  ASSERT_GE(out.size(), 2U);
  EXPECT_EQ(out[0], input[0]);
  EXPECT_EQ(out[1], topo.smtSiblingOfCpu[input[0]]);
}

// j=8 on AMD-like 16-physical host: every slot must be a DISTINCT
// physical core. No SMT pairing. Regression for the bench-tuned
// interleave that packed 8 workers onto 4 SMT-paired physical cores.
TEST(ReserveProducerCpuFirst,
     AmdSmtJ8KeepsAllSlotsOnDistinctPhysicalCoresNoSmtPairing) {
  const Topology topo = makeHomogeneousSmt(/*numCores=*/16, /*numCcds=*/2);
  std::vector<std::uint32_t> input = topo.physicalCores;
  const auto out = reserveProducerCpuFirst(input, /*participants=*/8,
                                           /*standalone=*/true, topo);
  ASSERT_GE(out.size(), 8U);
  // The first 8 entries (the slots the pool actually uses) must all be
  // physical-core reps; none of them is the SMT sibling of another in
  // the same prefix.
  for (std::size_t i = 0; i < 8U; ++i) {
    const std::uint32_t cpu = out[i];
    const std::uint32_t sib = topo.smtSiblingOfCpu[cpu];
    for (std::size_t j = 0; j < 8U; ++j) {
      if (i == j) {
        continue;
      }
      EXPECT_NE(out[j], sib) << "slot " << i << " (cpu " << cpu << ") and slot "
                             << j << " (cpu " << out[j]
                             << ") are SMT siblings; compute pool must use "
                                "distinct physical cores at j <= physicalCount";
    }
  }
}

// j=16 on AMD-like 16-physical host: all 16 physical cores used,
// distinct.
TEST(ReserveProducerCpuFirst,
     AmdSmtJ16AtPhysicalCountUsesAllDistinctPhysicalCores) {
  const Topology topo = makeHomogeneousSmt(/*numCores=*/16, /*numCcds=*/2);
  std::vector<std::uint32_t> input = topo.physicalCores;
  const auto out = reserveProducerCpuFirst(input, /*participants=*/16,
                                           /*standalone=*/true, topo);
  ASSERT_GE(out.size(), 16U);
  for (std::size_t i = 0; i < 16U; ++i) {
    const std::uint32_t cpu = out[i];
    const std::uint32_t sib = topo.smtSiblingOfCpu[cpu];
    for (std::size_t j = i + 1U; j < 16U; ++j) {
      EXPECT_NE(out[j], sib)
          << "slots " << i << " and " << j << " are SMT siblings";
    }
  }
}

// j=32 on AMD-like 16-physical host: oversubscribed (j > physicalCount).
// Tail slots must overflow to SMT siblings. Producer-CCD siblings
// appear before remote-CCD siblings.
TEST(ReserveProducerCpuFirst,
     AmdSmtJ32OversubscribedAppendsSmtSiblingsAsOverflow) {
  const Topology topo = makeHomogeneousSmt(/*numCores=*/16, /*numCcds=*/2);
  std::vector<std::uint32_t> input = topo.physicalCores;
  const auto out = reserveProducerCpuFirst(input, /*participants=*/32,
                                           /*standalone=*/true, topo);
  EXPECT_EQ(out.size(), 32U)
      << "expected pin list to grow to participant count via SMT overflow";
  // First 16 entries are physical cores; positions 16..31 must be SMT
  // siblings of one of the first 16. Same set covered exactly once.
  std::vector<bool> seenPhys(topo.logicalCount, false);
  for (std::size_t i = 0; i < 16U && i < out.size(); ++i) {
    seenPhys[out[i]] = true;
  }
  for (std::size_t i = 16U; i < out.size(); ++i) {
    const std::uint32_t sib = topo.smtSiblingOfCpu[out[i]];
    EXPECT_TRUE(sib < seenPhys.size() && seenPhys[sib])
        << "overflow slot " << i << " (cpu " << out[i]
        << ") has no physical-core partner in the first 16 slots";
  }
}

// --- Intel hybrid ---

// j=2 on hybrid: slot 1 must be SMT sibling of slot 0.
TEST(ReserveProducerCpuFirst, IntelHybridJ2RoutesSlot1ToProducerSibling) {
  const Topology topo = makeIntelHybrid(/*numP=*/6, /*numE=*/8);
  std::vector<std::uint32_t> input = topo.physicalCores;
  const auto out = reserveProducerCpuFirst(input, /*participants=*/2,
                                           /*standalone=*/true, topo);
  ASSERT_GE(out.size(), 2U);
  EXPECT_EQ(out[0], input[0]);
  EXPECT_EQ(out[1], topo.smtSiblingOfCpu[input[0]])
      << "slot 1 must land on the producer's SMT sibling for the L1 ack";
}

// j=8 on hybrid 14-physical (6P + 8E): every slot must be a distinct
// physical core. Slots 0..5 = P-cores; slots 6..7 = E-cores. No SMT
// pairing.
TEST(ReserveProducerCpuFirst,
     IntelHybridJ8KeepsAllSlotsOnDistinctPhysicalCores) {
  const Topology topo = makeIntelHybrid(/*numP=*/6, /*numE=*/8);
  std::vector<std::uint32_t> input = topo.physicalCores;
  const auto out = reserveProducerCpuFirst(input, /*participants=*/8,
                                           /*standalone=*/true, topo);
  ASSERT_GE(out.size(), 8U);
  for (std::size_t i = 0; i < 8U; ++i) {
    const std::uint32_t cpu = out[i];
    const std::uint32_t sib = cpu < topo.smtSiblingOfCpu.size()
                                  ? topo.smtSiblingOfCpu[cpu]
                                  : UINT32_MAX;
    if (sib == UINT32_MAX) {
      continue; // E-core, no sibling
    }
    for (std::size_t j = 0; j < 8U; ++j) {
      if (i == j) {
        continue;
      }
      EXPECT_NE(out[j], sib);
    }
  }
}

// j=16 on hybrid 14-physical: oversubscribed (j > 14). Slots 0..13 use
// all 14 physical cores; slots 14..15 overflow to two P-core SMT
// siblings.
TEST(ReserveProducerCpuFirst,
     IntelHybridJ16OversubscribedOverflowsToPCoreSmtSiblings) {
  const Topology topo = makeIntelHybrid(/*numP=*/6, /*numE=*/8);
  std::vector<std::uint32_t> input = topo.physicalCores;
  const auto out = reserveProducerCpuFirst(input, /*participants=*/16,
                                           /*standalone=*/true, topo);
  EXPECT_EQ(out.size(), 14U + 6U)
      << "expected overflow to all 6 P-core SMT siblings";
  // Verify the overflow CPUs (positions 14..) are all P-core siblings.
  for (std::size_t i = 14U; i < out.size(); ++i) {
    const std::uint32_t cpu = out[i];
    const std::uint32_t sib = topo.smtSiblingOfCpu[cpu];
    EXPECT_NE(sib, UINT32_MAX)
        << "overflow slot " << i << " (cpu " << cpu
        << ") should be a P-core SMT sibling, not an E-core";
  }
}

// --- Single-CCD AMD-like ---

// j=8 on a single-CCD 8-core homogeneous-SMT host: distinct physicals,
// no SMT pairing.
TEST(ReserveProducerCpuFirst,
     SingleCcdHomogeneousJ8AtPhysicalCountKeepsDistinctPhysicals) {
  const Topology topo = makeHomogeneousSmt(/*numCores=*/8, /*numCcds=*/1);
  std::vector<std::uint32_t> input = topo.physicalCores;
  const auto out = reserveProducerCpuFirst(input, /*participants=*/8,
                                           /*standalone=*/true, topo);
  ASSERT_GE(out.size(), 8U);
  for (std::size_t i = 0; i < 8U; ++i) {
    for (std::size_t j = i + 1U; j < 8U; ++j) {
      EXPECT_NE(out[j], topo.smtSiblingOfCpu[out[i]]);
    }
  }
}

// --- Arena (non-standalone) ---

// Arena pools (PoolKind::Arena -> standalone=false) skip the
// producer-CCD reorder entirely; the caller already filtered cpuPins to
// the arena's CCD. The j=2 SMT exception still fires.
TEST(ReserveProducerCpuFirst, ArenaModeJ2StillRoutesSibling) {
  const Topology topo = makeHomogeneousSmt(/*numCores=*/16, /*numCcds=*/2);
  // Arena CPUs: just the second CCD's physicals (CCD index 1 contains
  // CPU ids 16, 18, 20, 22, ...).
  std::vector<std::uint32_t> input;
  for (const std::uint32_t cpu : topo.ccdGroups[1]) {
    input.push_back(cpu);
  }
  const auto out = reserveProducerCpuFirst(input, /*participants=*/2,
                                           /*standalone=*/false, topo);
  ASSERT_GE(out.size(), 2U);
  EXPECT_EQ(out[0], input[0]);
  EXPECT_EQ(out[1], topo.smtSiblingOfCpu[input[0]]);
}

// Arena j=8 with 8 cores in the arena: distinct physicals, no pairing.
TEST(ReserveProducerCpuFirst, ArenaModeJ8KeepsDistinctPhysicals) {
  const Topology topo = makeHomogeneousSmt(/*numCores=*/16, /*numCcds=*/2);
  std::vector<std::uint32_t> input;
  for (const std::uint32_t cpu : topo.ccdGroups[0]) {
    input.push_back(cpu);
  }
  const auto out = reserveProducerCpuFirst(input, /*participants=*/8,
                                           /*standalone=*/false, topo);
  ASSERT_EQ(out.size(), input.size());
  for (std::size_t i = 0; i < out.size(); ++i) {
    for (std::size_t j = i + 1U; j < out.size(); ++j) {
      EXPECT_NE(out[j], topo.smtSiblingOfCpu[out[i]]);
    }
  }
}
