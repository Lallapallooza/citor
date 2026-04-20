#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

#include "citor/cancellation.h"
#include "citor/chain.h"
#include "citor/cpos/parallel_chain.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::BarrierKind;
using citor::CancellationToken;
using citor::ChainHints;
using citor::globalStage;
using citor::makeStage;
using citor::perChunkStage;
using citor::reduceStage;
using citor::serialStage;
using citor::staticStage;
using citor::ThreadPool;

// Hint preset used by the tests at TU scope (not in an anonymous namespace) so clang-tidy treats
// every static-constexpr member as a public field of a named type rather than an unused constant.
struct ChainTestHints {
  static constexpr citor::Balance balance =
      citor::Balance::StaticUniform;
  static constexpr citor::Affinity affinity = citor::Affinity::None;
  static constexpr citor::Priority priority =
      citor::Priority::Throughput;
  static constexpr citor::Partition partition =
      citor::Partition::ContiguousRanges;
  static constexpr bool pipelineSameChunk = true;
  static constexpr bool fpDeterministicTree = true;
  static constexpr bool cancellationChecks = true;
  static constexpr std::size_t chunk = 0;
};

namespace {

// Stages run in submission order; each stage sees the prior stage's output for its slot. We use a
// per-slot accumulator that each stage writes; the final per-slot value must equal the sum of all
// stage indices.
TEST(ParallelChain, BasicSequentialStages) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 64;
  const std::size_t participants = pool.participants();

  std::vector<std::atomic<std::int64_t>> perSlotSum(participants);
  for (auto &v : perSlotSum) {
    v.store(0, std::memory_order_relaxed);
  }

  pool.parallelChain<ChainTestHints>(
      kN,
      staticStage(
          "a",
          [&](std::size_t stageIdx, std::uint32_t slot, std::size_t /*lo*/, std::size_t /*hi*/) {
            perSlotSum[slot].fetch_add(static_cast<std::int64_t>(stageIdx),
                                       std::memory_order_relaxed);
          }),
      staticStage(
          "b",
          [&](std::size_t stageIdx, std::uint32_t slot, std::size_t /*lo*/, std::size_t /*hi*/) {
            perSlotSum[slot].fetch_add(static_cast<std::int64_t>(stageIdx),
                                       std::memory_order_relaxed);
          }),
      staticStage("c", [&](std::size_t stageIdx, std::uint32_t slot, std::size_t /*lo*/,
                           std::size_t /*hi*/) {
        perSlotSum[slot].fetch_add(static_cast<std::int64_t>(stageIdx), std::memory_order_relaxed);
      }));

  // 0 + 1 + 2 = 3 per slot.
  for (std::size_t s = 0; s < participants; ++s) {
    EXPECT_EQ(perSlotSum[s].load(std::memory_order_relaxed), 3) << "slot=" << s;
  }
}

// Worker B can't start stage 2 (idx=1) until worker A finishes stage 1 (idx=0) when stage 0's
// barrier is Global. We enforce this by stalling slot 0 inside stage 0 and asserting slot != 0
// has not entered stage 1 until slot 0's stage 0 has completed.
TEST(ParallelChain, GlobalBarrierBetweenStages) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 64;

  std::atomic<std::uint64_t> stage0Slot0Done{0};
  std::atomic<std::uint64_t> earliestStage1Other{UINT64_MAX};
  std::atomic<std::uint64_t> tickClock{0};

  pool.parallelChain<ChainTestHints>(
      kN,
      globalStage("barrier-source",
                  [&](std::size_t /*stageIdx*/, std::uint32_t slot, std::size_t /*lo*/,
                      std::size_t /*hi*/) {
                    if (slot == 0U) {
                      // Slot 0 sleeps so other workers' stage 1 entry can be observed if the
                      // barrier is missing.
                      std::this_thread::sleep_for(std::chrono::milliseconds(20));
                      stage0Slot0Done.store(tickClock.fetch_add(1, std::memory_order_acq_rel) + 1,
                                            std::memory_order_release);
                    }
                  }),
      staticStage("consumer", [&](std::size_t /*stageIdx*/, std::uint32_t slot, std::size_t /*lo*/,
                                  std::size_t /*hi*/) {
        if (slot != 0U) {
          const std::uint64_t myTick = tickClock.fetch_add(1, std::memory_order_acq_rel) + 1;
          std::uint64_t prev = earliestStage1Other.load(std::memory_order_acquire);
          while (myTick < prev) {
            if (earliestStage1Other.compare_exchange_strong(prev, myTick, std::memory_order_acq_rel,
                                                            std::memory_order_acquire)) {
              break;
            }
          }
        }
      }));

  EXPECT_NE(stage0Slot0Done.load(std::memory_order_acquire), 0U);
  EXPECT_GT(earliestStage1Other.load(std::memory_order_acquire),
            stage0Slot0Done.load(std::memory_order_acquire))
      << "Global barrier failed: a non-producer slot entered stage 1 before slot 0 finished "
         "stage 0";
}

// PerChunk barrier allows pipelining: stage s+1 chunk c can start before stage s of OTHER chunks
// finishes. We verify by counting overlap: at least one slot enters stage 1 before some other
// slot has finished stage 0. Compared against the global-barrier case where this never happens.
TEST(ParallelChain, PerChunkBarrierAllowsPipeline) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 64;
  const std::size_t participants = pool.participants();

  std::vector<std::atomic<std::uint8_t>> stage0Done(participants);
  std::vector<std::atomic<std::uint8_t>> stage1Started(participants);
  for (std::size_t i = 0; i < participants; ++i) {
    stage0Done[i].store(0, std::memory_order_relaxed);
    stage1Started[i].store(0, std::memory_order_relaxed);
  }

  std::atomic<std::uint32_t> overlapCount{0};

  pool.parallelChain<ChainTestHints>(
      kN,
      perChunkStage("produce",
                    [&](std::size_t /*stageIdx*/, std::uint32_t slot, std::size_t /*lo*/,
                        std::size_t /*hi*/) {
                      // Slot 0 takes the longest; this guarantees that under PerChunk, slots 1+ can
                      // move on without waiting on slot 0.
                      if (slot == 0U) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                      }
                      stage0Done[slot].store(1, std::memory_order_release);
                    }),
      staticStage("consume", [&](std::size_t /*stageIdx*/, std::uint32_t slot, std::size_t /*lo*/,
                                 std::size_t /*hi*/) {
        stage1Started[slot].store(1, std::memory_order_release);
        // Did some other slot complete stage 0 while slot 0 was still in-flight?
        for (std::size_t s = 0; s < stage0Done.size(); ++s) {
          if (s == slot) {
            continue;
          }
          if (stage0Done[s].load(std::memory_order_acquire) == 0U) {
            overlapCount.fetch_add(1, std::memory_order_acq_rel);
          }
        }
      }));

  // Every slot should have entered stage 1.
  for (std::size_t s = 0; s < participants; ++s) {
    EXPECT_EQ(stage1Started[s].load(std::memory_order_acquire), 1U);
    EXPECT_EQ(stage0Done[s].load(std::memory_order_acquire), 1U);
  }
  // Slot 0 sleeps; slots 1, 2, 3 should have observed at least one slot still in stage 0 when
  // they entered stage 1. Loose assertion: overlap > 0 means pipelining happened.
  EXPECT_GT(overlapCount.load(std::memory_order_acquire), 0U)
      << "PerChunk barrier failed to allow pipelining: every slot waited for every other slot";
}

// ProducerSerial: slot 0 (producer) executes the serial stage; other workers spin.
TEST(ParallelChain, ProducerSerialRunsOnSlot0) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 32;

  std::atomic<std::uint32_t> serialBodyCalls{0};
  std::atomic<std::uint32_t> serialBodySlot{UINT32_MAX};
  std::atomic<std::uint32_t> postSerialCalls{0};

  pool.parallelChain<ChainTestHints>(
      kN,
      // Stage 0: every slot runs. Post-stage barrier is ProducerSerial -> only slot 0 runs stage
      // 1.
      makeStage<BarrierKind::ProducerSerial>([&](std::size_t /*stageIdx*/, std::uint32_t /*slot*/,
                                                 std::size_t /*lo*/,
                                                 std::size_t /*hi*/) noexcept {}),
      // Stage 1: serial body. Should run only on slot 0.
      serialStage("serial",
                  [&](std::size_t /*stageIdx*/, std::uint32_t slot, std::size_t /*lo*/,
                      std::size_t /*hi*/) {
                    serialBodyCalls.fetch_add(1, std::memory_order_acq_rel);
                    serialBodySlot.store(slot, std::memory_order_release);
                  }),
      // Stage 2: every slot should run again (post-serial barrier is the serialStage's own
      // ProducerSerial which would mean only slot 0 runs stage 2 too -- but to test the spin we
      // reverse: use a Global stage after serial. The chain helpers all carry the post-stage
      // barrier as their kind. So the serialStage above declares ProducerSerial AFTER it. That
      // means stage 2 also runs only on slot 0. To verify "other workers spin" we need stage 1's
      // barrier to be NOT ProducerSerial. We rebuild this by using makeStage<Global> as the
      // serial wrapper and inferring from the count.
      staticStage("post", [&](std::size_t /*stageIdx*/, std::uint32_t /*slot*/, std::size_t /*lo*/,
                              std::size_t /*hi*/) {
        postSerialCalls.fetch_add(1, std::memory_order_acq_rel);
      }));

  // The serial body runs once, on slot 0.
  EXPECT_EQ(serialBodyCalls.load(std::memory_order_acquire), 1U);
  EXPECT_EQ(serialBodySlot.load(std::memory_order_acquire), 0U);
  // Stage 2's post-stage barrier is itself ProducerSerial in the helper -- so stage 2 only runs
  // on slot 0 because stage 1's barrier (ProducerSerial in serialStage) gates entry. So stage 2
  // is also slot-0 only.
  EXPECT_EQ(postSerialCalls.load(std::memory_order_acquire), 1U);
}

// Verify that non-producer workers actually spin on slot 0's `done` during the serial stage --
// stage 2's body must not run on any slot until slot 0 has finished publishing the serial result.
// We make the serial body sleep, store a marker, then sleep again, and use a per-slot timestamp
// in stage 2 to confirm every non-producer's stage-2 entry happens AFTER the producer's
// post-serial release-store. The previous tests asserted shape (counts, slot identity) but not
// the during-serial wait; a regression that turned `waitProducerSerialBarrier` into an
// immediate return would still pass them.
TEST(ParallelChain, ProducerSerialNonProducersSpinUntilSerialCompletes) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 32;
  const std::size_t participants = pool.participants();
  if (participants <= 1U) {
    GTEST_SKIP() << "single-participant pool collapses to inline path; spin verification N/A";
  }

  std::atomic<std::uint64_t> serialPublishTick{0};
  std::vector<std::atomic<std::uint64_t>> stage2EntryTick(participants);
  for (auto &t : stage2EntryTick) {
    t.store(0, std::memory_order_relaxed);
  }
  std::atomic<std::uint64_t> tickClock{0};

  pool.parallelChain<ChainTestHints>(
      kN,
      // Stage 0: drives the post-stage ProducerSerial barrier so stage 1 is serial on slot 0.
      makeStage<BarrierKind::ProducerSerial>([&](std::size_t /*stageIdx*/, std::uint32_t /*slot*/,
                                                 std::size_t /*lo*/,
                                                 std::size_t /*hi*/) noexcept {}),
      // Stage 1: serial body on slot 0. Sleep so non-producers must spin; record the
      // post-sleep release tick. Post-stage barrier is Global so stage 2 fans back out and we
      // can compare each slot's entry tick against the producer's release tick.
      makeStage<BarrierKind::Global>([&](std::size_t /*stageIdx*/, std::uint32_t slot,
                                         std::size_t /*lo*/, std::size_t /*hi*/) {
        if (slot == 0U) {
          std::this_thread::sleep_for(std::chrono::milliseconds(15));
          serialPublishTick.store(tickClock.fetch_add(1, std::memory_order_acq_rel) + 1,
                                  std::memory_order_release);
        }
      }),
      // Stage 2: every slot stamps the tick at which it entered. Non-producer slots' ticks
      // must all be strictly greater than the producer's serial-body release tick.
      staticStage("post-serial-fanout", [&](std::size_t /*stageIdx*/, std::uint32_t slot,
                                            std::size_t /*lo*/, std::size_t /*hi*/) {
        const std::uint64_t entry = tickClock.fetch_add(1, std::memory_order_acq_rel) + 1;
        stage2EntryTick[slot].store(entry, std::memory_order_release);
      }));

  const std::uint64_t publishTick = serialPublishTick.load(std::memory_order_acquire);
  EXPECT_GT(publishTick, 0U) << "serial body did not run on slot 0";
  for (std::size_t s = 1; s < participants; ++s) {
    const std::uint64_t entry = stage2EntryTick[s].load(std::memory_order_acquire);
    EXPECT_GT(entry, 0U) << "slot " << s << " never entered stage 2";
    EXPECT_GT(entry, publishTick) << "slot " << s << " entered stage 2 (tick=" << entry
                                  << ") before slot 0 published the serial result (tick="
                                  << publishTick << "); ProducerSerial spin barrier did not hold";
  }
}

// Verify that ProducerSerial only gates the stage immediately following it; with a non-serial
// barrier following, the next stage runs on every slot.
TEST(ParallelChain, ProducerSerialOnlyAffectsNextStage) {
