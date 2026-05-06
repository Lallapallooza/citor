#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "citor/hints.h"
#include "citor/pool_group.h"
#include "citor/thread_pool.h"

using citor::HintsDefaults;
using citor::PoolGroup;
using citor::ThreadPool;

TEST(PoolGroupReentrancy,
     NestedDispatchOnSameArenaCoversFullRangeWithoutDeadlocking) {
  PoolGroup &group = PoolGroup::global();
  // Same-pool reentrancy: an arena participant calls `parallelFor` on its own
  // arena while the outer dispatch is still active. The inner call must take a
  // nested same-pool path or a safe inline fallback, never publish a colliding
  // dispatch generation.
  //
  // Skipped when arena 0 has only one participant: there is no background
  // worker for the inner dispatch to land on, so `insidePoolWorker()` is false
  // on every body invocation and `workerInvocations` stays zero. The reentrancy
  // guard is exercised on hosts where arena 0 spawns at least one background
  // worker.
  if (group.arena(0).participants() < 2U) {
    GTEST_SKIP() << "arena 0 has " << group.arena(0).participants()
                 << " participant(s); reentrancy probe needs a background "
                    "worker to exercise the "
                    "guard";
  }
  std::atomic<std::uint64_t> nestedBodyExecutions{0};
  std::atomic<std::uint64_t> workerInvocations{0};
  group.arena(0).parallelFor<HintsDefaults>(
      std::size_t{0}, std::size_t{16},
      [&nestedBodyExecutions, &workerInvocations, &group](std::size_t /*lo*/,
                                                          std::size_t /*hi*/) {
        if (!ThreadPool::insidePoolWorker()) {
          return; // Producer side; dispatching here would publish a colliding
                  // generation.
        }
        workerInvocations.fetch_add(1, std::memory_order_relaxed);
        group.arena(0).parallelFor<HintsDefaults>(
            std::size_t{0}, std::size_t{32},
            [&nestedBodyExecutions](std::size_t lo, std::size_t hi) {
              nestedBodyExecutions.fetch_add(hi - lo,
                                             std::memory_order_relaxed);
            });
      });
  EXPECT_GT(workerInvocations.load(std::memory_order_acquire), 0U);
  // Each participant that invoked the inner call must have observed all 32
  // elements; total executions = workerInvocations * 32.
  EXPECT_EQ(nestedBodyExecutions.load(std::memory_order_acquire),
            workerInvocations.load(std::memory_order_acquire) * 32U);
}
