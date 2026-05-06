#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "citor/cpos/bulk_for_queries.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::Balance;
using citor::Hints;
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

// Member-and-CPO equivalence: writing per-query slots via either surface
// produces byte-equal output. The output is keyed on the query index, so
// dispatch order does not matter for the equality check.
TEST(BulkForQueriesCpo, MemberCallAndCpoCallProduceIdenticalSideEffects) {
  ThreadPool pool(4);
  constexpr std::size_t kQ = 512;
  std::vector<std::int32_t> outMember(kQ, 0);
  std::vector<std::int32_t> outCpo(kQ, 0);

  pool.bulkForQueries<BulkForQueriesTestHints>(
      kQ, [&](std::size_t lo, std::size_t hi) {
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

// Runtime-hint sibling: `bulkForQueriesRuntime` routes through the same engine
// and observably matches the compile-time member template's output.
TEST(BulkForQueriesCpo,
     RuntimeHintsSiblingProducesSameOutputAsCompileTimeHints) {
  ThreadPool pool(4);
  constexpr std::size_t kQ = 256;
  std::vector<std::int32_t> outCompile(kQ, 0);
  std::vector<std::int32_t> outRuntime(kQ, 0);

  pool.bulkForQueries<BulkForQueriesTestHints>(
      kQ, [&](std::size_t lo, std::size_t hi) {
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

} // namespace
