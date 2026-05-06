#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "citor/cancellation.h"
#include "citor/cpos/fork_join.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::Balance;
using citor::CancellationToken;
using citor::HintsDefaults;
using citor::ThreadPool;

// Hint preset for fork-join tests. forkJoin uses its own Chase-Lev deque path;
// the engine does not consult `Balance` on this codepath, so the preset only
// inherits HintsDefaults. The cross-CCD case is exercised via the bundled
// `CcdLocalForkJoinHints` preset.
struct ForkJoinTestHints : HintsDefaults {};

// One task throws; the producer's join must rethrow the captured exception, and
// the other tasks must complete (or have been cancelled cleanly).
TEST(ForkJoinErrors, ThrownExceptionRethrownAtJoin) {
  ThreadPool pool(4);
  std::array<std::atomic<int>, 4> slots{};
  std::atomic<int> ranCount{0};

  auto block = [&](int idx, int value) {
    return [&, idx, value]() {
      slots[static_cast<std::size_t>(idx)].store(value,
                                                 std::memory_order_relaxed);
      ranCount.fetch_add(1, std::memory_order_relaxed);
    };
  };

  bool caught = false;
  std::string what;
  try {
    pool.forkJoin<ForkJoinTestHints>(
        block(0, 1), block(1, 2),
        [&]() { throw std::runtime_error("forkjoin-fail"); }, block(3, 4));
  } catch (const std::runtime_error &e) {
    caught = true;
    what = e.what();
  }
  EXPECT_TRUE(caught);
  EXPECT_NE(what.find("forkjoin-fail"), std::string::npos);
  // At least one of the non-throwing tasks ran. Some may have been
  // short-circuited by the cancellation flag set on the throw path; the
  // contract is "no UB, captured exception rethrown", not "every task ran".
  EXPECT_GE(ranCount.load(), 1);
}
