#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <vector>

#include "citor/cancellation.h"
#include "citor/cpos/parallel_reduce.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::CancellationToken;
using citor::cancelled_value_exception;
using citor::HintsDefaults;
using citor::ThreadPool;

// Cancellation produces a partial result via cancelled_value_exception<T>.
//
// Skipped on single-participant pools: the inline-fallback path runs the
// map/combine over the full range in one chunk, so the partial-value contract
// (some chunks ran, others did not) has no observable surface.
TEST(ParallelReduceCancellation,
     MidFlightStopReturnsCancelledValueExceptionCarryingPartialSum) {
  ThreadPool pool(4);
  if (pool.participants() < 2U) {
    GTEST_SKIP() << "single-participant pool collapses to inline path; "
                    "partial-value cancellation "
                    "contract has no observable surface";
  }
  constexpr std::size_t kN = 100000;
  std::vector<double> data(kN, 1.0);
  CancellationToken tok = CancellationToken::makeOwned();

  bool sawCancellation = false;
  try {
    (void)pool.parallelReduce<HintsDefaults>(
        0, kN, 0.0,
        [&](std::size_t lo, std::size_t hi) {
          // Stop the token as soon as the first chunk runs so subsequent chunks
          // are skipped at chunk boundaries by the dispatch engine.
          tok.request_stop();
          double s = 0.0;
          for (std::size_t i = lo; i < hi; ++i) {
            s += data[i];
          }
          return s;
        },
        [](double a, double b) { return a + b; }, tok);
  } catch (const cancelled_value_exception<double> &e) {
    sawCancellation = true;
    // The partial value must reflect the chunks that did run, with init folded
    // in. It should be strictly between 0 (no progress) and the full sum (full
    // execution).
    EXPECT_GT(e.partial_value, 0.0);
    EXPECT_LT(e.partial_value, static_cast<double>(kN));
  }
  EXPECT_TRUE(sawCancellation) << "expected cancelled_value_exception<double>";
}

// Pre-cancelled token on the inline-fallback path: the body must not run, and
// the producer must observe the partial-value cancellation contract with
// `partial_value == init`.
TEST(ParallelReduceCancellation,
     InlineFallbackPathReturnsInitWhenTokenAlreadyStopped) {
  ThreadPool pool(1);
  CancellationToken tok = CancellationToken::makeOwned();
  tok.request_stop();

  std::atomic<int> mapCalls{0};
  bool sawCancellation = false;
  try {
    (void)pool.parallelReduce<HintsDefaults>(
        0, 100, 7.0,
        [&](std::size_t /*lo*/, std::size_t /*hi*/) {
          mapCalls.fetch_add(1, std::memory_order_relaxed);
          return 1.0;
        },
        [](double a, double b) { return a + b; }, tok);
  } catch (const cancelled_value_exception<double> &e) {
    sawCancellation = true;
    EXPECT_EQ(e.partial_value, 7.0)
        << "partial value must be init unchanged when no chunk ran";
  }
  EXPECT_TRUE(sawCancellation);
  EXPECT_EQ(mapCalls.load(), 0);
}

// When every chunk gets cancelled before completing, the partial value returned
// via the cancellation exception must be `init` unchanged: the user `combine`
// is not assumed to be `combine(x, T{}) == x` so we must not combine against a
// default-constructed `T{}`.
TEST(ParallelReduceCancellation,
     EveryChunkCancelledBeforeBodyReturnsInitValue) {
  ThreadPool pool(4);
  if (pool.participants() < 2U) {
    GTEST_SKIP() << "single-participant pool tested in "
                    "InlinePathHonorsPreCancelledToken";
  }

  CancellationToken tok = CancellationToken::makeOwned();
  tok.request_stop();

  bool sawCancellation = false;
  try {
    (void)pool.parallelReduce<HintsDefaults>(
        0, 1024, 42.0,
        [&](std::size_t /*lo*/, std::size_t /*hi*/) {
          ADD_FAILURE() << "no chunk should run when token is pre-cancelled";
          return 1.0;
        },
        [](double a, double b) { return a + b; }, tok);
  } catch (const cancelled_value_exception<double> &e) {
    sawCancellation = true;
    EXPECT_EQ(e.partial_value, 42.0);
  }
  EXPECT_TRUE(sawCancellation);
}
