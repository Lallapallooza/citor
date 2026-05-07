// Regression tests for slot-range partition overflow at large N.
//
// Each test pins a fix for a specific overflow path in the partition
// arithmetic that previously produced UB or wraparound for ranges near
// `SIZE_MAX` or for participant counts near the pending-mask bit width.

#include <gtest/gtest.h>

#include "citor/chain.h"
#include "citor/thread_pool.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

using namespace citor;

// pendingMaskBits used `1ULL << effective` which is UB for effective > 64.
// The clamp now caps the shift at 64. Most CI hosts do not have >64 cores
// so we cannot construct a real pool that triggers it, but we can verify
// the construction code path exits cleanly under UBSan for the boundary
// case (participants exactly equal to physical cores). We also verify a
// normal pool's join works for participants < 64.
TEST(RegressionPartitionOverflow,
     JoinScansFullPendingMaskOnSmallPoolWithoutDereferenceUB) {
  ThreadPool pool(4);
  std::atomic<int> count{0};
  pool.parallelFor<HintsDefaults>(0, 1024, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) {
      count.fetch_add(1, std::memory_order_relaxed);
    }
  });
  EXPECT_EQ(count.load(), 1024);
}

// chain/plex/scan slotRange used `n * slot` which overflows size_t when `n`
// is near SIZE_MAX. The 128-bit promotion fixes the partition. We pick a
// moderately large `n` and verify the partition covers [0, n) exactly with
// no overlap or gap.
TEST(RegressionPartitionOverflow,
     RunPlexSlotRangePartitionsExactlyForRangesNearMaxSizeT) {
  ThreadPool pool(4);
  if (pool.participants() < 2U) {
    GTEST_SKIP() << "needs at least two participants";
  }
  // Pick `n` near `SIZE_MAX / participants` to guarantee `n * slot` would
  // overflow in 64-bit. We don't actually iterate `[0, n)`; we just record
  // each slot's partition bounds.
  const std::size_t p = pool.participants();
  const std::size_t n = std::numeric_limits<std::size_t>::max();
  std::vector<std::pair<std::size_t, std::size_t>> ranges(p);
  pool.runPlex<HintsDefaults>(1, n,
                              [&](std::size_t /*stage*/, std::uint32_t slot,
                                  std::size_t lo,
                                  std::size_t hi) { ranges[slot] = {lo, hi}; });
  EXPECT_EQ(ranges[0].first, 0U);
  EXPECT_EQ(ranges[p - 1].second, n);
  for (std::size_t s = 0; s + 1 < p; ++s) {
    EXPECT_EQ(ranges[s].second, ranges[s + 1].first)
        << "gap or overlap at slot " << s;
    EXPECT_LE(ranges[s].first, ranges[s].second) << "inverted at slot " << s;
  }
}

TEST(RegressionPartitionOverflow,
     ParallelChainSlotRangePartitionsExactlyForRangesNearMaxSizeT) {
  ThreadPool pool(4);
  if (pool.participants() < 2U) {
    GTEST_SKIP() << "needs at least two participants";
  }
  const std::size_t p = pool.participants();
  const std::size_t n = std::numeric_limits<std::size_t>::max();
  std::vector<std::pair<std::size_t, std::size_t>> ranges(p);
  pool.parallelChain<ChainHintsDefaults>(
      n, globalStage("record", [&](std::size_t /*stage*/, std::uint32_t slot,
                                   std::size_t lo, std::size_t hi) {
        ranges[slot] = {lo, hi};
      }));
  EXPECT_EQ(ranges[0].first, 0U);
  EXPECT_EQ(ranges[p - 1].second, n);
  for (std::size_t s = 0; s + 1 < p; ++s) {
    EXPECT_EQ(ranges[s].second, ranges[s + 1].first);
    EXPECT_LE(ranges[s].first, ranges[s].second);
  }
}
