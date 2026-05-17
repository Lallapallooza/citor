// Regression tests for cross-pool synchronous dispatch and concurrent
// producers under a low-latency scope. Both pin fixes for races where a
// worker on pool A submits a sync call to pool B, and for the producer
// that races a low-latency scope tearing down on shutdown.

#include <gtest/gtest.h>

#include "citor/thread_pool.h"
#include "test_support_subprocess.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

using namespace std::chrono_literals;
using namespace citor;
using citor_test_support::ChildOutcome;
using citor_test_support::runInChildWithTimeout;

// Cross-standalone-pool synchronous dispatch must not deadlock. A worker
// on pool A calling B.parallelFor while a worker on pool B calls
// A.parallelFor previously waited indefinitely on each other's queues.
// The fix makes any cross-pool call fall through to inline-on-caller;
// tests that this happens within a bounded time.
TEST(RegressionCrossPoolDispatch,
     WorkerInOnePoolSubmittingSyncCallToAnotherPoolDoesNotDeadlock) {
#ifndef __linux__
  GTEST_SKIP();
#else
  const ChildOutcome outcome = runInChildWithTimeout(
      [] {
        ThreadPool poolA(2);
        ThreadPool poolB(2);
        // Sync flags forcing the classic mutex-inversion: t1 acquires A
        // and waits inside the body for t2 to confirm it acquired B before
        // attempting B; t2 mirrors with A. Without the cross-pool fall
        // through this is a guaranteed deadlock.
        std::atomic<bool> aHeld{false};
        std::atomic<bool> bHeld{false};
        std::thread t1([&] {
          poolA.parallelFor<StaticHints>(
              0, 64, [&](std::size_t /*lo*/, std::size_t /*hi*/) {
                aHeld.store(true, std::memory_order_release);
                while (!bHeld.load(std::memory_order_acquire)) {
                  std::this_thread::yield();
                }
                poolB.parallelFor<StaticHints>(0, 64,
                                               [](std::size_t, std::size_t) {});
              });
        });
        std::thread t2([&] {
          poolB.parallelFor<StaticHints>(
              0, 64, [&](std::size_t /*lo*/, std::size_t /*hi*/) {
                bHeld.store(true, std::memory_order_release);
                while (!aHeld.load(std::memory_order_acquire)) {
                  std::this_thread::yield();
                }
                poolA.parallelFor<StaticHints>(0, 64,
                                               [](std::size_t, std::size_t) {});
              });
        });
        t1.join();
        t2.join();
      },
      3s);
  EXPECT_EQ(outcome, ChildOutcome::Exited0);
#endif
}

// DispatchLease skips m_dispatchMutex whenever hotSpinDepth != 0. The
// "single producer" contract was enforced by convention only. With the
// fix, only the thread that owns the LowLatencyGuard skips the mutex;
// concurrent producers from other threads still serialize. This is a
// stress test that runs many small concurrent dispatches while a
// LowLatencyGuard is held; without the fix concurrent producers could
// race on the per-worker mailbox publish.
//
// Outcome under TSAN: pre-fix produces a data-race report on
// WorkerState::mailbox / mailboxDesc. Post-fix is clean.
TEST(
    RegressionCrossPoolDispatch,
    ConcurrentDispatchFromMultipleProducersUnderLowLatencyScopeIsRaceFreeOnMailbox) {
  ThreadPool pool(4);
  if (pool.participants() < 2U) {
    GTEST_SKIP();
  }
  auto guard = pool.lowLatencyScope();

  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  std::atomic<std::int64_t> sum{0};

  auto producer = [&] {
    ready.fetch_add(1, std::memory_order_release);
    while (!go.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (int i = 0; i < 200; ++i) {
      pool.parallelFor<HintsDefaults>(
          0, 64, [&](std::size_t lo, std::size_t hi) {
            for (std::size_t k = lo; k < hi; ++k) {
              sum.fetch_add(1, std::memory_order_relaxed);
            }
          });
    }
  };

  std::thread t1(producer);
  std::thread t2(producer);
  while (ready.load(std::memory_order_acquire) < 2) {
    std::this_thread::yield();
  }
  go.store(true, std::memory_order_release);
  t1.join();
  t2.join();

  // Each producer issues 200 dispatches over [0, 64); two producers, total
  // expected = 2 * 200 * 64 = 25600.
  EXPECT_EQ(sum.load(), 25600);
}
