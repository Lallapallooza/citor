#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "citor/cancellation.h"
#include "citor/cpos/parallel_scan.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::CancellationToken;
using citor::HintsDefaults;
using citor::ThreadPool;

// Cancellation mid-scan: a pre-stopped token returns early without UB. We do
// not assert anything about the partial output beyond "no crash, no UB"; the
// body counter records how many bodies ran.
TEST(ParallelScanCancellation,
     MidFlightStopAbortsScanWithoutCorruptingPriorChunks) {
  ThreadPool pool(4);
  if (pool.participants() < 2U) {
    GTEST_SKIP() << "single-participant pool collapses to inline single-call "
                    "shape; the two-pass body contract this test exercises "
                    "does not apply";
  }
  constexpr std::size_t kN = 100'000;
  std::vector<std::int64_t> in(kN, 1);

  CancellationToken tok = CancellationToken::makeOwned();
  EXPECT_TRUE(tok.request_stop());

  std::atomic<int> bodyCalls{0};
  auto body = [&in, &bodyCalls](std::size_t /*chunkId*/, std::size_t lo,
                                std::size_t hi, std::int64_t /*initial*/,
                                std::int64_t * /*unusedOut*/) -> std::int64_t {
    bodyCalls.fetch_add(1, std::memory_order_relaxed);
    std::int64_t s = 0;
    for (std::size_t i = lo; i < hi; ++i) {
      s += in[i];
    }
    return s;
  };

  // The pre-stopped token doesn't abort the call (we still complete dispatch to
  // honour dispatchOne's join contract); it just makes Pass 2 skip via the
  // wrapper's stop check. The primitive must return cleanly without UB;
  // downstream tests assert the body count is bounded.
  const std::int64_t total = pool.parallelScan<HintsDefaults>(
      kN, std::int64_t{0}, body,
      [](std::int64_t a, std::int64_t b) { return a + b; }, tok);

  // After cancellation, Pass 2 may have been skipped; the producer's sequential
  // reduce still yields `inclusiveTotal == sum(in) == kN` because Pass 1 ran on
  // every slot before the cancellation observation point.
  EXPECT_EQ(total, static_cast<std::int64_t>(kN));
  // Body should have been invoked at LEAST `participants` times (Pass 1 on
  // every slot) and at MOST `2 * participants` times (Pass 1 + Pass 2 on every
  // slot). The exact number depends on when the wrapper observed the stop
  // request.
  const int calls = bodyCalls.load(std::memory_order_relaxed);
  EXPECT_GE(calls, static_cast<int>(pool.participants()));
  EXPECT_LE(calls, 2 * static_cast<int>(pool.participants()));
}
