#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "citor/cpos/fork_join.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::HintsDefaults;
using citor::ThreadPool;

// Hint preset for fork-join tests. forkJoin uses its own Chase-Lev deque path;
// the engine does not consult `Balance` on this codepath, so the preset only
// inherits HintsDefaults. The cross-CCD case is exercised via the bundled
// `CcdLocalForkJoinHints` preset.
struct ForkJoinTestHints : HintsDefaults {};

namespace {

int fibForkJoin(ThreadPool &pool, int n) {
  if (n < 2) {
    return n;
  }
  if (n < 12) {
    // Cutoff: below the threshold, run serially to keep the test runtime
    // bounded.
    return fibForkJoin(pool, n - 1) + fibForkJoin(pool, n - 2);
  }
  int a = 0;
  int b = 0;
  pool.forkJoin<ForkJoinTestHints>([&]() { a = fibForkJoin(pool, n - 1); },
                                   [&]() { b = fibForkJoin(pool, n - 2); });
  return a + b;
}

} // namespace

// Determinism at the value level: with the same input the result is the same
// regardless of `nJobs`. The forkJoin-based reduction is associative +
// commutative over `int`, so the value is deterministic even though task order
// is racy.
TEST(ForkJoinDeterminism,
     ParallelTreeReductionMatchesSerialAcrossEveryParticipantCount) {
  constexpr int kRoot = 24;
  std::vector<int> results;
  results.reserve(5);
  for (const std::size_t nJobs : {1U, 2U, 4U, 8U, 16U}) {
    ThreadPool pool(nJobs);
    const int got = fibForkJoin(pool, kRoot);
    results.push_back(got);
  }
  for (std::size_t i = 1; i < results.size(); ++i) {
    EXPECT_EQ(results[i], results[0])
        << "fork-join diverged at nJobs index " << i;
  }
}
