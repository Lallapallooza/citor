// Regression tests for the detached-task lifecycle paths.
//
// Each test below corresponds to a specific bug or race and demonstrates
// that the engine no longer exhibits the broken behavior. The tests use
// child processes with timeouts so a hang reads as a failure rather than a
// permanent test stall.

#include <gtest/gtest.h>

#include "citor/thread_pool.h"
#include "test_support_subprocess.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

using namespace std::chrono_literals;
using namespace citor;
using citor_test_support::ChildOutcome;
using citor_test_support::runInChildWithTimeout;

// A detached task that owns the last shared_ptr<ThreadPool> triggers
// ~ThreadPool inside the closure's destructor. Before the fix, runDetached
// destroyed the closure before decrementing m_detachedInFlight, so the
// destructor's drain wait saw count==1 and self-deadlocked.
//
// The test runs in a child process so a hang reads as a TimedOut outcome
// rather than stalling the suite. The child waits for the deleter's flag
// to fire, proving the destructor actually completed; without the fix the
// detached thread deadlocks inside ~ThreadPool and the flag never sets.
TEST(RegressionDetachedLifecycle,
     DetachedTaskHoldingLastPoolRefDoesNotDeadlock) {
#ifndef __linux__
  GTEST_SKIP() << "subprocess timeout helper is Linux-only";
#else
  const ChildOutcome outcome = runInChildWithTimeout(
      [] {
        auto deleted = std::make_shared<std::atomic<bool>>(false);
        std::shared_ptr<ThreadPool> pool(
            new ThreadPool(2), [deleted](ThreadPool *p) {
              delete p;
              deleted->store(true, std::memory_order_release);
            });
        pool->submitDetached<HintsDefaults>([pool] {
          // Body keeps the pool ref alive across submission and is the last
          // owner once the outer caller resets its shared_ptr.
        });
        pool.reset();
        // Wait up to 1s for the deleter to fire; without the fix the
        // detached thread is stuck inside ~ThreadPool and never reaches
        // the deleter, so this loop never breaks and the parent's 2s
        // timeout fires.
        for (int i = 0; i < 1000; ++i) {
          if (deleted->load(std::memory_order_acquire)) {
            return;
          }
          std::this_thread::sleep_for(1ms);
        }
        // Hang the child so the parent's timeout reports TimedOut.
        for (;;) {
          std::this_thread::sleep_for(1s);
        }
      },
      2s);
  EXPECT_EQ(outcome, ChildOutcome::Exited0);
#endif
}

// A detached task that calls `lowLatencyScope()` must not deadlock when
// other threads are tied up in a long-running parallelFor. The original
// ctor spun forever waiting for every worker to publish a `hotSpinEpoch`
// ack, but a worker inside the user's body never reaches the spin loop,
// so the guard could hang forever.
TEST(RegressionDetachedLifecycle,
     DetachedLowLatencyScopeDoesNotHangOnBusyWorkers) {
#ifndef __linux__
  GTEST_SKIP() << "subprocess timeout helper is Linux-only";
#else
  const ChildOutcome outcome = runInChildWithTimeout(
      [] {
        ThreadPool pool(4);
        std::atomic<bool> taskRunning{true};
        std::atomic<bool> guardEntered{false};
        std::atomic<bool> bodyEntered{false};

        std::thread t([&] {
          pool.parallelFor<HintsDefaults>(
              0, 4, [&](std::size_t /*lo*/, std::size_t /*hi*/) {
                bodyEntered.store(true, std::memory_order_release);
                while (taskRunning.load(std::memory_order_acquire)) {
                  std::this_thread::yield();
                }
              });
        });
        while (!bodyEntered.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        std::thread guardThread([&] {
          auto guard = pool.lowLatencyScope();
          guardEntered.store(true, std::memory_order_release);
        });
        std::this_thread::sleep_for(50ms);
        taskRunning.store(false, std::memory_order_release);
        t.join();
        guardThread.join();
        if (!guardEntered.load(std::memory_order_acquire)) {
          for (;;) {
            std::this_thread::sleep_for(1s);
          }
        }
      },
      3s);
  EXPECT_EQ(outcome, ChildOutcome::Exited0);
#endif
}
