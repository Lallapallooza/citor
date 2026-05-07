// Regression test for the inline-fallback path's chunk-size honoring.

#include <gtest/gtest.h>

#include "citor/hints.h"
#include "citor/thread_pool.h"

#include <algorithm>
#include <cstddef>

using namespace citor;

namespace {

// Hints variant used by the inline-fallback chunk-honoring test. Declared at
// TU scope so clang-tidy treats every static-constexpr member as a public
// field of a named type rather than an unused constant.
struct ChunkedHints : HintsDefaults {
  static constexpr std::size_t chunk = 10;
};

} // namespace

// An inline fallback (single-participant pool, or when the inline gate
// fires) must respect the user's chunk size when one is set explicitly.
// Before the fix the fallback called fn([first, last)) with the whole
// range regardless of HintsT::chunk.
TEST(RegressionInlineFallback,
     InlineFallbackPathHonorsExplicitChunkSizeForRangeBoundaries) {
  ThreadPool pool(1); // forces inline fallback
  std::size_t maxChunkSeen = 0;
  pool.parallelFor<ChunkedHints>(0, 1000, [&](std::size_t lo, std::size_t hi) {
    const std::size_t span = hi - lo;
    maxChunkSeen = std::max(maxChunkSeen, span);
  });
  EXPECT_LE(maxChunkSeen, 10U);
}
