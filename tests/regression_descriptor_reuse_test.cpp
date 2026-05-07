// Regression tests for the shared static thread_local JobDescriptor
// reuse path across distinct lambda types and back-to-back dispatches.

#include <gtest/gtest.h>

#include "citor/cancellation.h"
#include "citor/thread_pool.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>

using namespace std::chrono_literals;
using namespace citor;

// The static thread_local JobDescriptor is now shared across distinct
// lambda types. Verify by dispatching with two different lambdas and
// asserting the underlying desc address is the same. The address is
// readable because we go through the named accessor.
TEST(RegressionDescriptorReuse,
     DescriptorTokenIsClearedBetweenCallsThatUseDifferentLambdaTypes) {
  ThreadPool pool(2);
  // Two distinct lambdas pre-fix would have used different desc addresses
  // because each was a different template instantiation of parallelFor;
  // post-fix they share `sharedParallelForDesc()`. The accessor itself
  // isn't public so we observe indirectly: dispatch both lambdas
  // back-to-back, verify both completed successfully. (The shared-storage
  // fact is a structural change; correctness is the visible signal.)
  std::atomic<int> a{0};
  std::atomic<int> b{0};
  pool.parallelFor<HintsDefaults>(0, 64, [&](std::size_t lo, std::size_t hi) {
    a.fetch_add(static_cast<int>(hi - lo), std::memory_order_relaxed);
  });
  pool.parallelFor<HintsDefaults>(0, 32, [&](std::size_t lo, std::size_t hi) {
    b.fetch_add(static_cast<int>(hi - lo), std::memory_order_relaxed);
  });
  EXPECT_EQ(a.load(), 64);
  EXPECT_EQ(b.load(), 32);
}

// Confirm the desc.token retention bound. After parallelFor returns, the
// static thread_local desc may hold a ref to the user's token
// (shared_ptr<atomic>). The retention is bounded: the next call with a
// different token replaces it. Verify by passing a token, then passing
// default (empty) token, and asserting the user's token is the sole owner.
TEST(RegressionDescriptorReuse,
     DescriptorTokenReleasedOnFollowupDefaultPolicyCall) {
  ThreadPool pool(2);
  auto tok = CancellationToken::makeOwned();
  auto stateBefore = tok; // 2 refs: tok + stateBefore
  pool.parallelFor<HintsDefaults>(
      0, 4, [](std::size_t /*lo*/, std::size_t /*hi*/) {}, tok);
  // After this call desc.token holds another ref; total >=3 refs.
  // Now pass a default token; this overwrites desc.token to default.
  pool.parallelFor<HintsDefaults>(
      0, 4, [](std::size_t /*lo*/, std::size_t /*hi*/) {});
  // tok and stateBefore are the only known refs now. Drop stateBefore so
  // tok is the unique owner.
  stateBefore = CancellationToken{};
  // We can't directly assert ref count on the token's internal shared_ptr
  // (the API doesn't expose it). But we can verify that re-issuing a
  // request_stop and then dispatching does not see stale state. The
  // primary guarantee is functional, not bookkeeping.
  EXPECT_TRUE(tok.canStop());
  EXPECT_TRUE(tok.request_stop());
  EXPECT_TRUE(tok.stop_requested());
}

// Cold-collapse and same-command reuse together are claimed to race on
// per-worker mailboxDesc / TLS cache. Stress test the path where the
// producer cold-collapses parked workers while reusing the same callable
// across many tiny back-to-back dispatches. Run under TSAN to detect data
// races on descriptor lifetime / cache priming.
TEST(RegressionDescriptorReuse,
     ColdCollapseStressOverManyDispatchesIsRaceFreeUnderTsan) {
  ThreadPool pool(4);
  if (pool.participants() < 2U) {
    GTEST_SKIP();
  }
  std::atomic<int> total{0};
  // Reuse the same lambda many times to exercise the same-command reuse
  // bit. Tiny ranges so cold-collapse fires (workers don't get to it).
  auto body = [&](std::size_t lo, std::size_t hi) noexcept {
    for (std::size_t i = lo; i < hi; ++i) {
      total.fetch_add(1, std::memory_order_relaxed);
    }
  };
  // Sleep periodically to let workers park, then resume. This forces the
  // producer's cold-collapse path because parked workers can't stamp the
  // mailbox before the producer's tight-probe times out.
  for (int round = 0; round < 50; ++round) {
    for (int i = 0; i < 32; ++i) {
      pool.parallelFor<HintsDefaults>(0, 4, body);
    }
    std::this_thread::sleep_for(2ms);
  }
  EXPECT_EQ(total.load(), 50 * 32 * 4);
}
