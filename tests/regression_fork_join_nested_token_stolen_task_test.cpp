// Regression test for the nested-`forkJoin` per-task gate. When the outer
// `forkJoin` has no token (drain instantiates with `HasToken=false`), an
// inner `forkJoin` whose token is already stopped must still have every
// peer-stolen inner task skip its body. The gate inside `runOneTaskImpl`
// has to consult `state.token` regardless of the outer drain's template
// parameter.

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "citor/cancellation.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::CancellationToken;
using citor::HintsDefaults;
using citor::ThreadPool;

TEST(RegressionForkJoinNestedTokenStolenTask,
     PeerStolenInnerTaskHonorsInnerTokenWhenOuterHasNone) {
  ThreadPool pool(4);
  ASSERT_GE(pool.participants(), 4U)
      << "test needs at least 4 participants so peers can steal";

  // Pre-stopped inner token. The framework's per-task gate must skip every
  // inner body. The OUTER forkJoin has no token, so the outer drain
  // instantiates as HasToken=false; the buggy code's compile-time elision
  // of the per-task token check on peer-stolen tasks lets stolen inner
  // tasks run despite the inner token being already stopped.
  CancellationToken innerTok = CancellationToken::makeOwned();
  innerTok.request_stop();

  std::atomic<int> bodiesEntered{0};

  pool.forkJoin<HintsDefaults>(
      [&] {
        // Inside the worker running task 1: spawn 12 inner tasks under
        // `innerTok`. Peers (3 other workers) probe this worker's deque
        // and steal inner tasks.
        pool.forkJoin<HintsDefaults>(
            innerTok,
            [&] { bodiesEntered.fetch_add(1, std::memory_order_relaxed); },
            [&] { bodiesEntered.fetch_add(1, std::memory_order_relaxed); },
            [&] { bodiesEntered.fetch_add(1, std::memory_order_relaxed); },
            [&] { bodiesEntered.fetch_add(1, std::memory_order_relaxed); },
            [&] { bodiesEntered.fetch_add(1, std::memory_order_relaxed); },
            [&] { bodiesEntered.fetch_add(1, std::memory_order_relaxed); },
            [&] { bodiesEntered.fetch_add(1, std::memory_order_relaxed); },
            [&] { bodiesEntered.fetch_add(1, std::memory_order_relaxed); },
            [&] { bodiesEntered.fetch_add(1, std::memory_order_relaxed); },
            [&] { bodiesEntered.fetch_add(1, std::memory_order_relaxed); },
            [&] { bodiesEntered.fetch_add(1, std::memory_order_relaxed); },
            [&] { bodiesEntered.fetch_add(1, std::memory_order_relaxed); });
      },
      [&] {
        // Task 2: just idle so a peer slot is dispatched to outer's drain
        // (where it'll probe task 1's worker deque and steal inner tasks).
        std::this_thread::yield();
      });

  // Invariant: with `innerTok` pre-stopped, the inner forkJoin's framework
  // must skip every inner task body. The framework's per-task gate
  // (`runOneTaskImpl`) is the sole defense for peer-stolen tasks. On the
  // buggy code, peers' gate compiled out the token check (template
  // HasToken=false from the outer drain) and ran bodies anyway.
  EXPECT_EQ(bodiesEntered.load(), 0)
      << "Peer-stolen inner tasks ran their bodies despite "
         "`innerTok.request_stop()`. The framework's per-task gate must "
         "consult `state.token.stop_requested()` regardless of the outer "
         "drain's HasToken template parameter.";
}
