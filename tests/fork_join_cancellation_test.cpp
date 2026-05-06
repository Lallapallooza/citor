#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

#include "citor/cancellation.h"
#include "citor/cpos/fork_join.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::Balance;
using citor::CancellationToken;
using citor::HintsDefaults;
using citor::ThreadPool;

// Hint preset for fork-join tests. forkJoin uses its own Chase-Lev deque path;
// the engine does not consult `Balance` on this codepath, so the preset only
// inherits HintsDefaults. The cross-CCD case is exercised via the bundled
// `CcdLocalForkJoinHints` preset.
struct ForkJoinTestHints : HintsDefaults {};

// Cancellation token stopped before forkJoin call: every task observes
// cancellation at entry, the join still rendezvous, no exception.
TEST(ForkJoinCancellation, PreStoppedTokenSkipsAllBodies) {
  ThreadPool pool(4);
  CancellationToken tok = CancellationToken::makeOwned();
  tok.request_stop();

  std::atomic<int> ranCount{0};
  pool.forkJoin<ForkJoinTestHints>(
      tok, [&]() { ranCount.fetch_add(1, std::memory_order_relaxed); },
      [&]() { ranCount.fetch_add(1, std::memory_order_relaxed); },
      [&]() { ranCount.fetch_add(1, std::memory_order_relaxed); },
      [&]() { ranCount.fetch_add(1, std::memory_order_relaxed); });
  // No body ran (cancellation flag is set before any task admit), and no UB.
  EXPECT_EQ(ranCount.load(), 0);
}

// Cancellation requested mid-flight: partial completion is acceptable, but no
// UB.
TEST(ForkJoinCancellation,
     MidFlightStopReturnsAfterInflightTasksFinishWithoutDeadlocking) {
  ThreadPool pool(4);
  CancellationToken tok = CancellationToken::makeOwned();
  std::atomic<int> ranCount{0};
  std::atomic<bool> firstObserved{false};

  std::atomic<int> spinSink{0};
  auto longishTask = [&]() {
    ranCount.fetch_add(1, std::memory_order_relaxed);
    firstObserved.store(true, std::memory_order_release);
    // Spin briefly to give the producer time to request stop while other tasks
    // may still be queued. The duration is bounded to keep test runtime small.
    for (int i = 0; i < 200000; ++i) {
      spinSink.fetch_add(1, std::memory_order_relaxed);
    }
  };

  // Spawn a controller thread that requests stop once the first task has begun
  // executing.
  std::thread controller([&]() {
    while (!firstObserved.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    tok.request_stop();
  });

  pool.forkJoin<ForkJoinTestHints>(tok, longishTask, longishTask, longishTask,
                                   longishTask, longishTask, longishTask,
                                   longishTask, longishTask);
  controller.join();
  // No assertion on the exact count; cancellation is best-effort. The test
  // passes if forkJoin returns at all (no deadlock) and no UB was triggered.
  SUCCEED();
}
