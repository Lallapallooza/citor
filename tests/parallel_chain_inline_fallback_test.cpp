#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "citor/chain.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::ChainHintsDefaults;
using citor::staticStage;
using citor::ThreadPool;

// Inline-fallback path for `parallelChain`: a single-participant pool runs
// every stage on slot 0 with the full input range.

// Single-participant inline path: stages run on slot 0 only with the full
// range.
TEST(ParallelChainInlineFallback,
     SingleParticipantPoolRunsEveryStageOnSlot0WithFullRange) {
  ThreadPool pool(1);
  constexpr std::size_t kN = 16;

  std::atomic<std::uint32_t> stageCalls{0};
  std::atomic<std::size_t> observedHi{0};

  pool.parallelChain<ChainHintsDefaults>(
      kN, staticStage("inline-only",
                      [&](std::size_t /*stageIdx*/, std::uint32_t slot,
                          std::size_t lo, std::size_t hi) {
                        EXPECT_EQ(slot, 0U);
                        EXPECT_EQ(lo, 0U);
                        observedHi.store(hi, std::memory_order_release);
                        stageCalls.fetch_add(1, std::memory_order_acq_rel);
                      }));

  EXPECT_EQ(stageCalls.load(std::memory_order_acquire), 1U);
  EXPECT_EQ(observedHi.load(std::memory_order_acquire), kN);
}
