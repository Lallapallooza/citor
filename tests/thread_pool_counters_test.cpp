#include <gtest/gtest.h>

#include <cstddef>

#include "citor/hints.h"
#include "citor/thread_pool.h"

// `snapshotCounters()` returns zero pool-level fields unless
// `CITOR_ENABLE_POOL_COUNTERS` was defined at build time. When defined,
// dispatches advance per fan-out and inlineFallbacks advances per `runInline`
// short-circuit. Worker-aggregated fields are always available.
TEST(ThreadPoolLifecycleCounters,
     DispatchAndInlineFallbackCountersAdvanceMonotonicallyPerCall) {
#ifdef CITOR_ENABLE_POOL_COUNTERS
  citor::ThreadPool pool(4);
  const auto before = pool.snapshotCounters();
  for (int i = 0; i < 16; ++i) {
    pool.parallelFor<citor::DynamicHints>(
        0U, 64U, [](std::size_t /*lo*/, std::size_t /*hi*/) noexcept {});
  }
  const auto after = pool.snapshotCounters();
  EXPECT_GE(after.dispatches, before.dispatches + 16U);

  citor::ThreadPool solo(1);
  const auto soloBefore = solo.snapshotCounters();
  for (int i = 0; i < 8; ++i) {
    solo.parallelFor<citor::DynamicHints>(
        0U, 64U, [](std::size_t /*lo*/, std::size_t /*hi*/) noexcept {});
  }
  const auto soloAfter = solo.snapshotCounters();
  EXPECT_GE(soloAfter.inlineFallbacks, soloBefore.inlineFallbacks + 8U);
  EXPECT_EQ(soloAfter.dispatches, soloBefore.dispatches);
#else
  GTEST_SKIP() << "pool-level counters disabled at build time "
                  "(CITOR_ENABLE_POOL_COUNTERS=OFF)";
#endif
}
