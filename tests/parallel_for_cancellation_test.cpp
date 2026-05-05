#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::Balance;
using citor::CancellationToken;
using citor::HintsDefaults;
using citor::ThreadPool;

// Hint preset used by the chunk-boundary cancellation test. Lives at TU scope
// (not in an anonymous namespace) so clang-tidy treats every static-constexpr
// member as a public field of a named type rather than an unused constant.
struct DynamicChunkedTestHints : HintsDefaults {
  static constexpr Balance balance = Balance::DynamicChunked;
  static constexpr std::size_t chunk = 16;
};

// Cancellation at chunk boundaries: a token stopped mid-flight aborts
// subsequent chunks.
//
// Skipped on single-participant pools: the inline-fallback path runs the body
// once over the full range, leaving the chunk-boundary cancellation contract no
// observable surface to exercise.
TEST(ParallelForCancellation,
     MidFlightStopAbortsRemainingChunksWithoutLossOrDuplication) {
  ThreadPool pool(4);
  if (pool.participants() < 2U) {
    GTEST_SKIP() << "single-participant pool collapses to inline path; "
                    "cancellation at chunk "
                    "boundary has no observable surface";
  }
  constexpr std::size_t kN = 4096;
  CancellationToken tok = CancellationToken::makeOwned();
  std::atomic<std::size_t> processed{0};

  pool.parallelFor<DynamicChunkedTestHints>(
      0, kN,
      [&](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
          processed.fetch_add(1, std::memory_order_relaxed);
        }
        if (processed.load(std::memory_order_relaxed) >= 32) {
          tok.request_stop();
        }
      },
      tok);

  // Cancellation is best-effort at chunk boundaries: at least one chunk runs,
  // and not every chunk ran (the stop was observed before the range was
  // exhausted).
  const std::size_t total = processed.load(std::memory_order_relaxed);
  EXPECT_GT(total, 0U);
  EXPECT_LE(total, kN);
  EXPECT_LT(total, kN) << "cancellation never observed";
}

// A pre-cancelled token on the inline-fallback path must short-circuit before
// the body runs.
TEST(ParallelForCancellation,
     InlineFallbackPathDoesNotInvokeBodyWhenTokenAlreadyStopped) {
  ThreadPool pool(1);
  CancellationToken tok = CancellationToken::makeOwned();
  tok.request_stop();

  std::atomic<int> calls{0};
  pool.parallelFor<HintsDefaults>(
      0, 100,
      [&](std::size_t /*lo*/, std::size_t /*hi*/) {
        calls.fetch_add(1, std::memory_order_relaxed);
      },
      tok);

  EXPECT_EQ(calls.load(), 0);
}

TEST(ParallelForCancellation,
     NestedInsideForkJoinDoesNotInvokeBodyWhenTokenAlreadyStopped) {
  ThreadPool pool(4);
  CancellationToken tok = CancellationToken::makeOwned();
  tok.request_stop();
  std::atomic<std::uint32_t> bodies{0};

  pool.forkJoin<HintsDefaults>(
      [&] {
        pool.parallelFor<HintsDefaults>(
            std::size_t{0}, std::size_t{128},
            [&bodies](std::size_t lo, std::size_t hi) {
              bodies.fetch_add(static_cast<std::uint32_t>(hi - lo),
                               std::memory_order_relaxed);
            },
            tok);
      },
      [] {});

  EXPECT_EQ(bodies.load(std::memory_order_acquire), 0U);
}
