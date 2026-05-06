#include <gtest/gtest.h>

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

// Empty task pack: forkJoin returns immediately, no UB, no allocation visible.
TEST(ForkJoinEdge, EmptyTaskPackReturnsImmediately) {
  ThreadPool pool(4);
  pool.forkJoin<ForkJoinTestHints>();
  SUCCEED();
}
