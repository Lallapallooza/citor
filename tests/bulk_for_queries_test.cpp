#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "citor/cancellation.h"
#include "citor/cpos/bulk_for_queries.h"
#include "citor/hints.h"
#include "citor/example_hints.h"
#include "citor/thread_pool.h"

using citor::Balance;
using citor::CancellationToken;
using citor::Hints;
using citor::BulkQueryHints;
using citor::ThreadPool;

// Hint preset at TU scope (not in an anonymous namespace) so clang-tidy treats every
// static-constexpr member as a public field of a named type rather than an unused constant.
struct BulkForQueriesTestHints {
  static constexpr Balance balance = Balance::DynamicChunked;
  static constexpr citor::Priority priority =
      citor::Priority::Throughput;
  static constexpr double estimatedItemNs = 0.0;
  static constexpr double minTaskUs = 0.0;
  static constexpr std::size_t chunk = 16;
};

namespace {

// Range coverage: every query index in [0, q) must be processed exactly once across the chunked
// dispatch. Mirrors the parallel_for `RangeCoverage` test but key on bulk-query semantics.
TEST(BulkForQueries, RangeCoverage) {
  ThreadPool pool(4);
  constexpr std::size_t kQ = 1024;
  std::vector<std::atomic<std::uint32_t>> counts(kQ);
  for (auto &c : counts) {
    c.store(0, std::memory_order_relaxed);
  }

  pool.bulkForQueries<BulkForQueriesTestHints>(kQ, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) {
      counts[i].fetch_add(1, std::memory_order_relaxed);
    }
  });

  for (std::size_t i = 0; i < kQ; ++i) {
    EXPECT_EQ(counts[i].load(std::memory_order_relaxed), 1U) << "query " << i;
  }
}

// Member-and-CPO equivalence: writing per-query slots via either surface produces byte-equal
// output. The output is keyed on the query index, so dispatch order does not matter for the
// equality check.
TEST(BulkForQueries, MemberCpoEquivalence) {
  ThreadPool pool(4);
  constexpr std::size_t kQ = 512;
  std::vector<std::int32_t> outMember(kQ, 0);
  std::vector<std::int32_t> outCpo(kQ, 0);

  pool.bulkForQueries<BulkForQueriesTestHints>(kQ, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) {
      outMember[i] = static_cast<std::int32_t>((i * 3) + 1);
    }
  });

  citor::bulkForQueries.template operator()<BulkForQueriesTestHints>(
      pool, kQ, [&](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
          outCpo[i] = static_cast<std::int32_t>((i * 3) + 1);
        }
      });

  EXPECT_EQ(outMember, outCpo);
}

// Runtime-hint sibling: `bulkForQueriesRuntime` routes through the same engine and observably
// matches the compile-time member template's output.
TEST(BulkForQueries, RuntimeHintsMatchCompileTime) {
  ThreadPool pool(4);
  constexpr std::size_t kQ = 256;
  std::vector<std::int32_t> outCompile(kQ, 0);
  std::vector<std::int32_t> outRuntime(kQ, 0);

  pool.bulkForQueries<BulkForQueriesTestHints>(kQ, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) {
      outCompile[i] = static_cast<std::int32_t>(i + 7);
    }
  });

  Hints runtimeHints;
  runtimeHints.balance = Balance::DynamicChunked;
  runtimeHints.estimatedItemNs = 0.0;
  runtimeHints.minTaskUs = 0.0;
  runtimeHints.chunk = 16;
  pool.bulkForQueriesRuntime(
      kQ,
      [&](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
          outRuntime[i] = static_cast<std::int32_t>(i + 7);
        }
      },
      runtimeHints);

  EXPECT_EQ(outCompile, outRuntime);
}

// Cancellation at chunk boundary: a token stopped mid-flight aborts subsequent queries. At least
// one chunk runs (to trigger the stop) and not every query is processed before the stop is
// observed.
TEST(BulkForQueries, CancellationAtChunkBoundary) {
  ThreadPool pool(4);
  constexpr std::size_t kQ = 4096;
  CancellationToken tok;
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

// Named hint instantiation: the `BulkQueryHints` site hint is a valid policy type and routes
// through `bulkForQueries` without compile errors. This is the "realistic call site" smoke test.
TEST(BulkForQueries, BulkQueryHintInstantiates) {
  ThreadPool pool(4);
  constexpr std::size_t kQ = 1024;
  std::vector<std::atomic<std::uint32_t>> counts(kQ);
  for (auto &c : counts) {
    c.store(0, std::memory_order_relaxed);
  }

  pool.bulkForQueries<BulkQueryHints>(kQ, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) {
      counts[i].fetch_add(1, std::memory_order_relaxed);
    }
  });

  for (std::size_t i = 0; i < kQ; ++i) {
    EXPECT_EQ(counts[i].load(std::memory_order_relaxed), 1U) << "query " << i;
  }
}

} // namespace
