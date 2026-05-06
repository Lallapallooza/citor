#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "citor/chain.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::ChainHintsDefaults;
using citor::globalStage;
using citor::staticStage;
using citor::ThreadPool;

// Slot-range partition contracts for `parallelChain`. Static stages partition
// the input range exactly once across slots; dynamic-global stages emit the
// expected chunk count and publish stage outputs before the next stage reads.

// Hint preset used by the dynamic-global partition test. Lives at TU scope
// (not in an anonymous namespace) so clang-tidy treats every static-constexpr
// member as a public field of a named type rather than an unused constant.
struct DynamicChainTestHints : citor::DynamicChainHints {
  static constexpr std::size_t chunk = 1;
};

// Slot range partition: union of every slot's [lo, hi) covers [0, n) exactly
// once per stage.
TEST(ParallelChainPartition,
     UnionOfSlotRangesCoversFullRangeExactlyOncePerStage) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 100;
  std::vector<std::atomic<std::uint32_t>> coverage(kN);
  for (auto &c : coverage) {
    c.store(0, std::memory_order_relaxed);
  }

  pool.parallelChain<ChainHintsDefaults>(
      kN,
      staticStage("cov", [&](std::size_t /*stageIdx*/, std::uint32_t /*slot*/,
                             std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
          coverage[i].fetch_add(1, std::memory_order_acq_rel);
        }
      }));

  for (std::size_t i = 0; i < kN; ++i) {
    EXPECT_EQ(coverage[i].load(std::memory_order_acquire), 1U) << "i=" << i;
  }
}

// Dynamic-chain mode opts out of same-chunk pipelining: each globally
// synchronized stage is split into chunks that participants claim from a
// counter. The global barrier must still publish all stage-0 writes before
// stage 1 can observe them.
TEST(
    ParallelChainPartition,
    DynamicGlobalStagesEmitExpectedChunkCountAndPublishStageOutputBeforeNextStageReads) {
  ThreadPool pool(4);
  if (pool.participants() < 2U) {
    GTEST_SKIP() << "single-participant pool collapses to inline path; the "
                    "dynamic-global stage partitioning this test asserts does "
                    "not apply";
  }
  constexpr std::size_t kN = 97;
  constexpr std::size_t kExpectedChunks =
      (kN + DynamicChainTestHints::chunk - 1U) / DynamicChainTestHints::chunk;
  const std::size_t participants = pool.participants();

  std::vector<std::atomic<std::uint32_t>> stage0Coverage(kN);
  std::vector<std::atomic<std::uint32_t>> stage1Coverage(kN);
  std::vector<std::atomic<std::uint32_t>> stage2Coverage(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    stage0Coverage[i].store(0, std::memory_order_relaxed);
    stage1Coverage[i].store(0, std::memory_order_relaxed);
    stage2Coverage[i].store(0, std::memory_order_relaxed);
  }

  std::atomic<std::uint32_t> badSlot{0};
  std::atomic<std::uint32_t> staleStage1Reads{0};
  std::atomic<std::uint32_t> staleStage2Reads{0};
  std::atomic<std::uint32_t> stage0Calls{0};
  std::atomic<std::uint32_t> stage1Calls{0};
  std::atomic<std::uint32_t> stage2Calls{0};

  pool.parallelChain<DynamicChainTestHints>(
      kN,
      globalStage("dynamic-0",
                  [&](std::size_t /*stageIdx*/, std::uint32_t slot,
                      std::size_t lo, std::size_t hi) {
                    if (slot >= participants) {
                      badSlot.fetch_add(1, std::memory_order_acq_rel);
                    }
                    stage0Calls.fetch_add(1, std::memory_order_acq_rel);
                    for (std::size_t i = lo; i < hi; ++i) {
                      stage0Coverage[i].fetch_add(1, std::memory_order_acq_rel);
                    }
                  }),
      globalStage(
          "dynamic-1",
          [&](std::size_t /*stageIdx*/, std::uint32_t slot, std::size_t lo,
              std::size_t hi) {
            if (slot >= participants) {
              badSlot.fetch_add(1, std::memory_order_acq_rel);
            }
            stage1Calls.fetch_add(1, std::memory_order_acq_rel);
            for (std::size_t i = lo; i < hi; ++i) {
              if (stage0Coverage[i].load(std::memory_order_acquire) != 1U) {
                staleStage1Reads.fetch_add(1, std::memory_order_acq_rel);
              }
              stage1Coverage[i].fetch_add(1, std::memory_order_acq_rel);
            }
          }),
      globalStage("dynamic-2", [&](std::size_t /*stageIdx*/, std::uint32_t slot,
                                   std::size_t lo, std::size_t hi) {
        if (slot >= participants) {
          badSlot.fetch_add(1, std::memory_order_acq_rel);
        }
        stage2Calls.fetch_add(1, std::memory_order_acq_rel);
        for (std::size_t i = lo; i < hi; ++i) {
          if (stage1Coverage[i].load(std::memory_order_acquire) != 1U) {
            staleStage2Reads.fetch_add(1, std::memory_order_acq_rel);
          }
          stage2Coverage[i].fetch_add(1, std::memory_order_acq_rel);
        }
      }));

  EXPECT_EQ(badSlot.load(std::memory_order_acquire), 0U);
  EXPECT_EQ(staleStage1Reads.load(std::memory_order_acquire), 0U);
  EXPECT_EQ(staleStage2Reads.load(std::memory_order_acquire), 0U);
  EXPECT_EQ(stage0Calls.load(std::memory_order_acquire), kExpectedChunks);
  EXPECT_EQ(stage1Calls.load(std::memory_order_acquire), kExpectedChunks);
  EXPECT_EQ(stage2Calls.load(std::memory_order_acquire), kExpectedChunks);
  for (std::size_t i = 0; i < kN; ++i) {
    EXPECT_EQ(stage0Coverage[i].load(std::memory_order_acquire), 1U)
        << "stage0 i=" << i;
    EXPECT_EQ(stage1Coverage[i].load(std::memory_order_acquire), 1U)
        << "stage1 i=" << i;
    EXPECT_EQ(stage2Coverage[i].load(std::memory_order_acquire), 1U)
        << "stage2 i=" << i;
  }
}
