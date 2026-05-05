#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>

#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::HintsDefaults;
using citor::ThreadPool;

// Hint preset used by the inline-fallback test. Lives at TU scope (not in an
// anonymous namespace) so clang-tidy treats every static-constexpr member as a
// public field of a named type rather than an unused constant.
struct InlineFallbackHints : HintsDefaults {
  // Gate is n*estimatedItemNs < minTaskUs*participants; producer runs inline
  // for any n < 1000*participants.
  static constexpr double estimatedItemNs = 1.0;
  static constexpr double minTaskUs = 1000.0;
};

// Inline fallback: small n with a high estimated cost runs inline; the body
// executes once with the full range on the producer's thread without waking
// workers.
TEST(ParallelForInlineFallback,
     RunsBodyInlineOnceWhenEstimatedCostFallsBelowThreshold) {
  ThreadPool pool(4);

  std::atomic<std::size_t> processed{0};
  std::atomic<std::size_t> blockCount{0};
  pool.parallelFor<InlineFallbackHints>(
      0, 1, [&](std::size_t lo, std::size_t hi) {
        EXPECT_EQ(lo, 0U);
        EXPECT_EQ(hi, 1U);
        processed.fetch_add(1, std::memory_order_relaxed);
        blockCount.fetch_add(1, std::memory_order_relaxed);
      });

  EXPECT_EQ(processed.load(std::memory_order_relaxed), 1U);
  // Exactly one block ran: the producer ran the full range inline rather than
  // partitioning it across workers.
  EXPECT_EQ(blockCount.load(std::memory_order_relaxed), 1U);
}
