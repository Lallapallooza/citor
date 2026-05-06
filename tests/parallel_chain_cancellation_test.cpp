#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "citor/cancellation.h"
#include "citor/chain.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::CancellationToken;
using citor::ChainHintsDefaults;
using citor::globalStage;
using citor::ThreadPool;

// Cancellation propagation through `parallelChain`. A stop request raised
// inside one stage must abort every subsequent stage's body without invoking
// it on any slot.

// Cancellation: a stopped token aborts subsequent stages cleanly. We trigger
// the stop from inside stage 1 (slot 0) and assert stage 2 never runs.
TEST(ParallelChainCancellation,
     MidFlightStopAbortsLaterStagesWithoutInvokingTheirBodies) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 32;

  CancellationToken tok = CancellationToken::makeOwned();
  std::atomic<std::uint32_t> stage1Calls{0};
  std::atomic<std::uint32_t> stage2Calls{0};

  pool.parallelChainWithToken<ChainHintsDefaults>(
      kN, tok,
      globalStage("stage0",
                  [&](std::size_t /*stageIdx*/, std::uint32_t /*slot*/,
                      std::size_t /*lo*/, std::size_t /*hi*/) noexcept {}),
      // Stage 1 stops the token from slot 0.
      globalStage("stage1",
                  [&tok, &stage1Calls](std::size_t /*stageIdx*/,
                                       std::uint32_t slot, std::size_t /*lo*/,
                                       std::size_t /*hi*/) {
                    stage1Calls.fetch_add(1, std::memory_order_acq_rel);
                    if (slot == 0U) {
                      tok.request_stop();
                    }
                  }),
      // Stage 2 should NOT run on any slot once the token was stopped at the
      // prior boundary.
      globalStage("stage2",
                  [&stage2Calls](std::size_t /*stageIdx*/,
                                 std::uint32_t /*slot*/, std::size_t /*lo*/,
                                 std::size_t /*hi*/) {
                    stage2Calls.fetch_add(1, std::memory_order_acq_rel);
                  }));

  // Stage 1 ran on every slot before stop.
  EXPECT_GE(stage1Calls.load(std::memory_order_acquire), 1U);
  // Stage 2 did not run.
  EXPECT_EQ(stage2Calls.load(std::memory_order_acquire), 0U);
}
