#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "citor/cpos/bulk_for_queries.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::Balance;
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

// Range coverage: every query index in [0, q) must be processed exactly once
// across the chunked dispatch. Mirrors the parallel_for `RangeCoverage` test
// but key on bulk-query semantics.
TEST(BulkForQueriesPartition,
     InvokesQueryBodyAcrossEveryItemAndQueryPairExactlyOnce) {
  ThreadPool pool(4);
  constexpr std::size_t kQ = 1024;
  std::vector<std::atomic<std::uint32_t>> counts(kQ);
  for (auto &c : counts) {
    c.store(0, std::memory_order_relaxed);
  }

  pool.bulkForQueries<BulkForQueriesTestHints>(
      kQ, [&](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
          counts[i].fetch_add(1, std::memory_order_relaxed);
        }
      });

  for (std::size_t i = 0; i < kQ; ++i) {
    EXPECT_EQ(counts[i].load(std::memory_order_relaxed), 1U) << "query " << i;
  }
}

// Named hint instantiation: a HintsDefaults-derived bulk hint is a valid policy
// type and routes through `bulkForQueries` without compile errors. This is the
// "realistic call site" smoke test.
TEST(BulkForQueriesPartition, AcceptsBulkQueryHintsPolicyAndCoversFullRange) {
  ThreadPool pool(4);
  constexpr std::size_t kQ = 1024;
  std::vector<std::atomic<std::uint32_t>> counts(kQ);
  for (auto &c : counts) {
    c.store(0, std::memory_order_relaxed);
  }

  pool.bulkForQueries<BulkForQueriesTestHints>(
      kQ, [&](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
          counts[i].fetch_add(1, std::memory_order_relaxed);
        }
      });

  for (std::size_t i = 0; i < kQ; ++i) {
    EXPECT_EQ(counts[i].load(std::memory_order_relaxed), 1U) << "query " << i;
  }
}

} // namespace
