#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "citor/hints.h"
#include "citor/pool_group.h"
#include "citor/thread_pool.h"

using citor::HintsDefaults;
using citor::PoolGroup;
using citor::ThreadPool;

TEST(PoolGroupArena, WorkerStampsArenaIndexThatMatchesItsHostArena) {
  PoolGroup &group = PoolGroup::global();
  if (group.ccdCount() < std::size_t{2}) {
    GTEST_SKIP() << "host has only one CCD; arena isolation requires two CCDs";
  }

  // Workers on arena 0 must report `arenaIndex == 0` from inside their body.
  // Workers on arena 1 must not appear in arena 0's TLS view -- a worker can
  // only carry its own arena's token.
  std::atomic<std::uint32_t> observed{
      std::numeric_limits<std::uint32_t>::max()};
  group.arena(0).parallelFor<HintsDefaults>(
      std::size_t{0}, std::size_t{1024},
      [&observed](std::size_t /*lo*/, std::size_t /*hi*/) {
        const std::uint32_t hint = ThreadPool::currentArenaIndexHint();
        // Producer-side bodies during the inline path
        // would report the sentinel; only background
        // workers stamp their own arena.
        if (hint != std::numeric_limits<std::uint32_t>::max()) {
          observed.store(hint, std::memory_order_relaxed);
        }
      });
  // At least one background worker must have run; the observed token must be 0
  // (arena 0).
  EXPECT_EQ(observed.load(std::memory_order_acquire), 0U);
}

TEST(PoolGroupArena, SyncCallFromOneArenaIntoAnotherFallsThroughToInlinePath) {
  PoolGroup &group = PoolGroup::global();
  if (group.ccdCount() < std::size_t{2}) {
    GTEST_SKIP() << "deadlock-guard test requires at least two CCD arenas";
  }

  // Counts how many times the body of the inner parallelFor on arena 1 ran on a
  // background worker (i.e., a worker whose `currentArenaIndexHint() == 1`)
  // when it was invoked from a worker of arena 0. The cross-arena guard must
  // make this zero: the inner call falls through to inline-on-caller, so the
  // body runs on the same arena-0 worker that issued the call.
  //
  // The outer body skips the producer slot (slot 0 runs on the test thread,
  // which is not a worker on any arena and so is allowed to dispatch into arena
  // 1 normally) so the assertion exercises only the cross-arena
  // worker-to-worker case the deadlock guard targets.
  std::atomic<std::uint64_t> arena1WorkerWakes{0};
  std::atomic<std::uint64_t> outerWorkerInvocations{0};

  group.arena(0).parallelFor<HintsDefaults>(
      std::size_t{0}, std::size_t{8},
      [&arena1WorkerWakes, &outerWorkerInvocations,
       &group](std::size_t /*lo*/, std::size_t /*hi*/) {
        if (ThreadPool::currentArenaIndexHint() != 0U) {
          // Producer-side body or non-worker context; the cross-arena guard
          // does not apply here, so dispatching into arena 1 from this site is
          // permitted.
          return;
        }
        outerWorkerInvocations.fetch_add(1, std::memory_order_relaxed);
        group.arena(1).parallelFor<HintsDefaults>(
            std::size_t{0}, std::size_t{4096},
            [&arena1WorkerWakes](std::size_t /*ilo*/, std::size_t /*ihi*/) {
              if (ThreadPool::currentArenaIndexHint() == 1U) {
                arena1WorkerWakes.fetch_add(1, std::memory_order_relaxed);
              }
            });
      });

  EXPECT_GT(outerWorkerInvocations.load(std::memory_order_acquire), 0U);
  EXPECT_EQ(arena1WorkerWakes.load(std::memory_order_acquire), 0U);
}

TEST(PoolGroupArena, SingleCcdHostRunsEveryParallelForEndToEndOnArena0) {
  PoolGroup &group = PoolGroup::global();
  if (group.ccdCount() != std::size_t{1}) {
    GTEST_SKIP()
        << "host has " << group.ccdCount()
        << " CCDs; single-CCD acceptance only applies on hosts with one CCD";
  }
  // On a one-CCD host the only path is `arena(0)`; verify it runs work
  // end-to-end.
  std::atomic<std::uint64_t> total{0};
  group.arena(0).parallelFor<HintsDefaults>(
      std::size_t{0}, std::size_t{1024},
      [&total](std::size_t lo, std::size_t hi) {
        total.fetch_add(hi - lo, std::memory_order_relaxed);
      });
  EXPECT_EQ(total.load(std::memory_order_acquire), 1024U);
}
