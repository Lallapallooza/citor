// Regression test for pool-specific `LowLatencyGuard` TLS. A thread that
// owns LL on pool A must not skip pool B's dispatch mutex just because
// another thread happens to hold LL on B; the TLS record carries the
// pool pointer so the fast-path skip only fires when the dispatched pool
// matches the LL owner's pool.

#include <gtest/gtest.h>

#include "citor/thread_pool.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

using namespace citor;

// T2 holds LL on poolB passively (no dispatches). T1 holds LL on poolA
// AND dispatches on poolB. T3 dispatches on poolB without any LL. Pre-fix
// T1 skipped B's mutex (because B's hotSpinDepth was non-zero from T2's
// guard and T1's pool-agnostic depth was non-zero from A's guard), racing
// T3 on B's mailbox publish. Post-fix T1's TLS pool == &poolA != &poolB,
// so T1 takes B's mutex and serializes with T3.
//
// Limitation: the assertion `sum == 6400` cannot witness the unsynchronised
// publish on its own -- the dispatched body's atomic fetch_add still
// increments correctly even under torn `activeJob` / mailbox publish, so
// the visible total stays correct. This test is a smoke check that the
// cross-pool LL code paths do not crash or deadlock. The actual race
// witness requires TSAN (`-DCITOR_BUILD_WITH_SANITIZER=ON`), which the
// citor CI runs in a dedicated job; on a TSAN build, the pre-fix code
// reports a data race on `WorkerState::mailbox` / `mailboxDesc`, while
// the post-fix code is clean.
TEST(RegressionLowLatencyPoolSpecific,
     NonOwnerDispatchOnPoolBTakesMutexEvenWhenOtherThreadHoldsBsLowLatency) {
  ThreadPool poolA(4);
  ThreadPool poolB(4);
  if (poolA.participants() < 2U || poolB.participants() < 2U) {
    GTEST_SKIP();
  }

  std::atomic<int> ready{0};
  std::atomic<bool> goRun{false};
  std::atomic<bool> stop{false};
  std::atomic<std::int64_t> sum{0};

  // T2: pure passive LL holder on B. Engages the guard, signals ready,
  // then waits for the dispatchers to finish before releasing it. T2
  // never dispatches on B during the window, so the "LL owner is the
  // sole dispatcher under LL" contract is honoured for B.
  auto threadPassiveOnB = [&] {
    auto guard = poolB.lowLatencyScope();
    ready.fetch_add(1, std::memory_order_release);
    while (!stop.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  };

  // T1: holds LL on A, dispatches on B. Without the fix T1 would skip
  // B's mutex (its agnostic-depth check sees A's depth, B's
  // hotSpinDepth).
  auto threadOwnerOnAButDispatchingOnB = [&] {
    auto guard = poolA.lowLatencyScope();
    ready.fetch_add(1, std::memory_order_release);
    while (!goRun.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (int i = 0; i < 100; ++i) {
      poolB.parallelFor<HintsDefaults>(
          0, 32, [&](std::size_t lo, std::size_t hi) {
            for (std::size_t k = lo; k < hi; ++k) {
              sum.fetch_add(1, std::memory_order_relaxed);
            }
          });
    }
  };

  // T3: no LL anywhere; dispatches on B. Always takes B's mutex. The
  // mutex must serialize T1 against T3 on every dispatch.
  auto threadDispatchOnBNoLl = [&] {
    ready.fetch_add(1, std::memory_order_release);
    while (!goRun.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (int i = 0; i < 100; ++i) {
      poolB.parallelFor<HintsDefaults>(
          0, 32, [&](std::size_t lo, std::size_t hi) {
            for (std::size_t k = lo; k < hi; ++k) {
              sum.fetch_add(1, std::memory_order_relaxed);
            }
          });
    }
  };

  std::thread t2(threadPassiveOnB);
  std::thread t1(threadOwnerOnAButDispatchingOnB);
  std::thread t3(threadDispatchOnBNoLl);
  while (ready.load(std::memory_order_acquire) < 3) {
    std::this_thread::yield();
  }
  goRun.store(true, std::memory_order_release);
  t1.join();
  t3.join();
  stop.store(true, std::memory_order_release);
  t2.join();

  // Two dispatchers * 100 dispatches * 32 increments = 6400.
  EXPECT_EQ(sum.load(), 6400);
}

// Same-pool nested LowLatencyGuard scopes must stack the depth correctly:
// the inner guard's dtor restores the outer's TLS state without clearing
// it, so subsequent dispatches inside the outer scope still observe the
// LL fast path.
TEST(RegressionLowLatencyPoolSpecific, NestedSamePoolGuardsRestoreOuterScope) {
  ThreadPool pool(4);
  if (pool.participants() < 2U) {
    GTEST_SKIP();
  }
  auto outer = pool.lowLatencyScope();
  {
    auto inner = pool.lowLatencyScope();
    std::atomic<std::int64_t> sink{0};
    pool.parallelFor<HintsDefaults>(0, 64, [&](std::size_t lo, std::size_t hi) {
      for (std::size_t k = lo; k < hi; ++k) {
        sink.fetch_add(1, std::memory_order_relaxed);
      }
    });
    EXPECT_EQ(sink.load(), 64);
  }
  // Inner dropped; outer still alive. Dispatch should still succeed.
  std::atomic<std::int64_t> sink2{0};
  pool.parallelFor<HintsDefaults>(0, 64, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t k = lo; k < hi; ++k) {
      sink2.fetch_add(1, std::memory_order_relaxed);
    }
  });
  EXPECT_EQ(sink2.load(), 64);
}
