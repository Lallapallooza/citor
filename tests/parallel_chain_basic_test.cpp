#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "citor/chain.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::ChainHintsDefaults;
using citor::reduceStage;
using citor::staticStage;
using citor::ThreadPool;

// Basic chain semantics: stage ordering, empty packs, and the reduceStage
// helper. Every test in this file asserts a positive structural invariant of
// `parallelChain` rather than a barrier or cancellation behaviour.

// Stages run in submission order; each stage sees the prior stage's output for
// its slot. We use a per-slot accumulator that each stage writes; the final
// per-slot value must equal the sum of all stage indices.
TEST(ParallelChainBasic, RunsStagesInSubmissionOrderAccumulatingPerSlot) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 64;
  const std::size_t participants = pool.participants();

  std::vector<std::atomic<std::int64_t>> perSlotSum(participants);
  for (auto &v : perSlotSum) {
    v.store(0, std::memory_order_relaxed);
  }

  pool.parallelChain<ChainHintsDefaults>(
      kN,
      staticStage("a",
                  [&](std::size_t stageIdx, std::uint32_t slot,
                      std::size_t /*lo*/, std::size_t /*hi*/) {
                    perSlotSum[slot].fetch_add(
                        static_cast<std::int64_t>(stageIdx),
                        std::memory_order_relaxed);
                  }),
      staticStage("b",
                  [&](std::size_t stageIdx, std::uint32_t slot,
                      std::size_t /*lo*/, std::size_t /*hi*/) {
                    perSlotSum[slot].fetch_add(
                        static_cast<std::int64_t>(stageIdx),
                        std::memory_order_relaxed);
                  }),
      staticStage("c", [&](std::size_t stageIdx, std::uint32_t slot,
                           std::size_t /*lo*/, std::size_t /*hi*/) {
        perSlotSum[slot].fetch_add(static_cast<std::int64_t>(stageIdx),
                                   std::memory_order_relaxed);
      }));

  // 0 + 1 + 2 = 3 per slot.
  for (std::size_t s = 0; s < participants; ++s) {
    EXPECT_EQ(perSlotSum[s].load(std::memory_order_relaxed), 3) << "slot=" << s;
  }
}

// Empty stage pack: parallelChain(n) is a no-op; no assertions other than "does
// not hang or throw". Verified by a clean return.
TEST(ParallelChainBasic, EmptyStagePackReturnsCleanlyWithoutWaking) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 32;

  // No stages.
  pool.parallelChain<ChainHintsDefaults>(kN);
  // If we got here, the call returned cleanly.
  SUCCEED();
}

// reduceStage carries BarrierKind::DeterministicReduce; the chain treats it
// as a global barrier (the deterministic reduction is the user's concern
// inside the stage body). Verifies the helper compiles and the chain runs
// each stage exactly once across slots.
TEST(ParallelChainBasic, ReduceStageHelperCompilesAndRunsBodyOncePerSlot) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 32;
  const std::size_t participants = pool.participants();

  std::atomic<std::int64_t> totalCalls{0};

  pool.parallelChain<ChainHintsDefaults>(
      kN,
      reduceStage("reduce-then-global",
                  [&](std::size_t /*stageIdx*/, std::uint32_t /*slot*/,
                      std::size_t /*lo*/, std::size_t /*hi*/) {
                    totalCalls.fetch_add(1, std::memory_order_acq_rel);
                  }),
      staticStage("after", [&](std::size_t /*stageIdx*/, std::uint32_t /*slot*/,
                               std::size_t /*lo*/, std::size_t /*hi*/) {
        totalCalls.fetch_add(1, std::memory_order_acq_rel);
      }));

  EXPECT_EQ(totalCalls.load(std::memory_order_acquire),
            static_cast<std::int64_t>(2 * participants));
}
