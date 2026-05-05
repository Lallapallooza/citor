#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::Balance;
using citor::BulkHints;
using citor::Hints;
using citor::HintsDefaults;
using citor::ThreadPool;

// Hint preset used by the tests. Lives at TU scope (not in an anonymous
// namespace) so clang-tidy treats every static-constexpr member as a public
// field of a named type rather than an unused constant.
struct DynamicChunkedTestHints : HintsDefaults {
  static constexpr Balance balance = Balance::DynamicChunked;
  static constexpr std::size_t chunk = 16;
};

// Range coverage: every index in [0, n) must be visited exactly once across the
// per-block ranges.
TEST(ParallelForPartition, InvokesBodyForEveryIndexInRangeExactlyOnce) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 1024;
  std::vector<std::atomic<std::uint32_t>> counts(kN);
  for (auto &c : counts) {
    c.store(0, std::memory_order_relaxed);
  }

  pool.parallelFor<HintsDefaults>(0, kN, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) {
      counts[i].fetch_add(1, std::memory_order_relaxed);
    }
  });

  for (std::size_t i = 0; i < kN; ++i) {
    EXPECT_EQ(counts[i].load(std::memory_order_relaxed), 1U) << "index " << i;
  }
}

// Default chunk derivation: with `chunk == 0` and `participants == 4`, the
// dispatcher oversubscribes to `2 * participants` blocks so dynamic balance has
// a tail to absorb a straggling rank's share. Verify the block count and range
// coverage.
TEST(ParallelForPartition, StaticUniformPartitionEmitsTwoBlocksPerParticipant) {
  ThreadPool pool(4);
  if (pool.participants() < 2U) {
    GTEST_SKIP() << "single-participant pool collapses to inline path; the "
                    "block-strided count this test asserts does not apply";
  }
  constexpr std::size_t kN = 4096;
  std::atomic<std::size_t> blockCount{0};

  pool.parallelFor<HintsDefaults>(0, kN, [&](std::size_t lo, std::size_t hi) {
    EXPECT_LT(lo, hi);
    EXPECT_LE(hi, kN);
    blockCount.fetch_add(1, std::memory_order_relaxed);
  });

  EXPECT_EQ(blockCount.load(std::memory_order_relaxed),
            pool.participants() * 2U);
}

// Dynamic-chunked balances a skewed workload across workers so total wall time
// is dominated by the slowest chunk. This is a smoke test that the
// dynamic-counter tier completes the full range.
TEST(ParallelForPartition, DynamicChunkedBalanceCoversFullRangeUnderSkew) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 1024;
  std::vector<std::atomic<std::uint32_t>> counts(kN);
  for (auto &c : counts) {
    c.store(0, std::memory_order_relaxed);
  }

  pool.parallelFor<DynamicChunkedTestHints>(
      0, kN, [&](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
          counts[i].fetch_add(1, std::memory_order_relaxed);
        }
      });

  for (std::size_t i = 0; i < kN; ++i) {
    EXPECT_EQ(counts[i].load(std::memory_order_relaxed), 1U) << "index " << i;
  }
}

// BulkHints (a HintsDefaults-derived preset) is a valid policy type and routes
// through parallelFor without compile errors. This is the "realistic call site"
// smoke test.
TEST(ParallelForPartition, AcceptsBulkHintsPolicyAndCoversFullRange) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 1024;
  std::vector<std::atomic<std::uint32_t>> counts(kN);
  for (auto &c : counts) {
    c.store(0, std::memory_order_relaxed);
  }

  pool.parallelFor<BulkHints>(0, kN, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) {
      counts[i].fetch_add(1, std::memory_order_relaxed);
    }
  });

  for (std::size_t i = 0; i < kN; ++i) {
    EXPECT_EQ(counts[i].load(std::memory_order_relaxed), 1U) << "index " << i;
  }
}
