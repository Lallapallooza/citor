#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "citor/cancellation.h"
#include "citor/cpos/bulk_for_queries.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::Balance;
using citor::CancellationToken;
using citor::HintsDefaults;
using citor::ThreadPool;

// Hint preset at TU scope (not in an anonymous namespace) so clang-tidy treats
// every static-constexpr member as a public field of a named type rather than
// an unused constant.
struct BulkForQueriesTestHints : HintsDefaults {
  static constexpr Balance balance = Balance::DynamicChunked;
  static constexpr std::size_t chunk = 16;
};

namespace {

// Cancellation at chunk boundary: a token stopped mid-flight aborts subsequent
// queries. At least one chunk runs (to trigger the stop) and not every query is
// processed before the stop is observed.
//
// Skipped when the pool collapses to a single participant: the inline-fallback
// path runs the body once over the full range, so a token requested mid-body
// cannot abort any subsequent chunk (there are none). The chunk-boundary
// cancellation contract has no observable surface in that mode.
TEST(BulkForQueriesCancellation,
     MidFlightStopAbortsRemainingChunksAcrossQueryDimension) {
  ThreadPool pool(4);
  if (pool.participants() < 2U) {
    GTEST_SKIP() << "single-participant pool collapses to inline path; "
                    "cancellation at chunk "
                    "boundary has no observable surface";
  }
  constexpr std::size_t kQ = 4096;
  CancellationToken tok = CancellationToken::makeOwned();
  std::atomic<std::size_t> processed{0};

  pool.bulkForQueries<BulkForQueriesTestHints>(
      kQ,
      [&](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
          processed.fetch_add(1, std::memory_order_relaxed);
        }
        if (processed.load(std::memory_order_relaxed) >= 32) {
          tok.request_stop();
        }
      },
      tok);

  const std::size_t total = processed.load(std::memory_order_relaxed);
  EXPECT_GT(total, 0U);
  EXPECT_LE(total, kQ);
  EXPECT_LT(total, kQ) << "cancellation never observed";
}

} // namespace
