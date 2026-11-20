// Every public header must compile after Qt's keyword macros are defined. Qt
// defines `slots`, `signals`, `emit` and `foreach` as macros, so an identifier
// with one of those names breaks or silently changes meaning in any
// translation unit that includes a Qt header first. The definitions below
// match Qt's `qobjectdefs.h` and `qforeach.h`.

#define Q_SLOTS
#define Q_SIGNALS public
#define Q_EMIT
#define Q_FOREACH(variable, container) for (variable : container)
#define slots Q_SLOTS
#define signals Q_SIGNALS
#define emit Q_EMIT
#define foreach Q_FOREACH

#include "citor/always_assert.h"
#include "citor/cancellation.h"
#include "citor/chain.h"
#include "citor/coherence_cache.h"
#include "citor/coro.h"
#include "citor/cpos/bulk_for_queries.h"
#include "citor/cpos/fork_join.h"
#include "citor/cpos/inclusive_scan.h"
#include "citor/cpos/parallel_chain.h"
#include "citor/cpos/parallel_for.h"
#include "citor/cpos/parallel_reduce.h"
#include "citor/cpos/parallel_scan.h"
#include "citor/cpos/run_plex.h"
#include "citor/cpos/submit_detached.h"
#include "citor/function_ref.h"
#include "citor/hints.h"
#include "citor/pool_group.h"
#include "citor/thread_pool.h"
#include "citor/version.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>

// Instantiates the work-stealing deque through `forkJoin`, the path whose
// headers declared a `slots` member.
TEST(QtKeywordMacros, ForkJoinRunsWithQtMacrosDefined) {
  citor::ThreadPool pool(2);
  std::atomic<int> ran{0};
  pool.forkJoin<citor::HintsDefaults>(
      [&]() { ran.fetch_add(1, std::memory_order_relaxed); },
      [&]() { ran.fetch_add(1, std::memory_order_relaxed); });
  EXPECT_EQ(ran.load(std::memory_order_relaxed), 2);
}
