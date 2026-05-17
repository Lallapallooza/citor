// Regression test for the inline `parallelReduce` chunk loop: the
// single-participant path must poll the cancellation token between
// chunks so that a mid-flight stop throws `cancelled_value_exception<T>`
// like the multi-worker path does.

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>

#include "citor/cancellation.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::CancellationToken;
using citor::cancelled_value_exception;
using citor::HintsDefaults;
using citor::ThreadPool;

TEST(RegressionInlineReduceMidflightCancel,
     SingleParticipantInlineReduceObservesMidflightCancellationBetweenChunks) {
  ThreadPool pool(1);
  ASSERT_EQ(pool.participants(), 1U)
      << "test requires a 1-participant pool to force the inline-reduce path";

  // `n` chosen to produce nChunks > 1 in `runReduceInlineImpl`:
  // `reduceChunkSize(n, 0)` returns `ceilDiv(n, 64)` when n > 64, yielding
  // exactly 64 chunks for n = 100000.
  constexpr std::size_t kN = 100000;

  CancellationToken tok = CancellationToken::makeOwned();
  std::atomic<int> chunkCount{0};

  bool sawCancellation = false;
  try {
    (void)pool.parallelReduce<HintsDefaults>(
        0, kN, 0.0,
        [&](std::size_t lo, std::size_t hi) {
          const int c = chunkCount.fetch_add(1, std::memory_order_acq_rel);
          if (c == 0) {
            // Stop the token from inside the first chunk's body -- the same
            // realistic pattern the parallel path's existing
            // `MidFlightStopReturnsCancelledValueExceptionCarryingPartialSum`
            // test exercises. The inline path is supposed to honor the same
            // contract.
            tok.request_stop();
          }
          double s = 0.0;
          for (std::size_t i = lo; i < hi; ++i) {
            s += 1.0;
          }
          return s;
        },
        [](double a, double b) { return a + b; }, tok);
  } catch (const cancelled_value_exception<double> &e) {
    sawCancellation = true;
    // Partial value must be strictly between init (0.0) and the full sum.
    EXPECT_GT(e.partial_value, 0.0);
    EXPECT_LT(e.partial_value, static_cast<double>(kN));
  }
  EXPECT_TRUE(sawCancellation)
      << "Inline parallelReduce path on a 1-participant pool ignored "
         "mid-flight cancellation. The documented contract says "
         "parallelReduce throws cancelled_value_exception<T>; the inline "
         "path's chunk loop must poll the token between chunks to match the "
         "parallel path's behavior.";
  // The first chunk runs and stops the token; subsequent chunks should be
  // skipped. On buggy code chunkCount == nChunks (= 64 for kN=100000).
  const int observed = chunkCount.load();
  EXPECT_GE(observed, 1);
  EXPECT_LT(observed, 64)
      << "All 64 chunks ran -- inline loop did not observe the stop request.";
}
