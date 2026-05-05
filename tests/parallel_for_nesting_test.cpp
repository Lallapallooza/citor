#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::HintsDefaults;
using citor::ThreadPool;

// A nested `parallelFor` on the same standalone pool from inside an outer body
// must not deadlock on the dispatch mutex. The inner call may use a nested
// same-pool path, but it must still finish before the outer call returns.
TEST(ParallelForNesting, NestedSamePoolDispatchCompletesWithoutDeadlocking) {
  ThreadPool pool(4);
  if (pool.participants() < 2U) {
    GTEST_SKIP() << "single-participant pool collapses to inline path; the "
                    "outer parallelFor runs once and the inner-work counter "
                    "this test asserts does not apply";
  }
  constexpr std::size_t kOuter = 4;
  constexpr std::size_t kInner = 8;
  std::atomic<int> innerWork{0};

  pool.parallelFor<HintsDefaults>(
      0, kOuter, [&](std::size_t /*lo*/, std::size_t /*hi*/) {
        pool.parallelFor<HintsDefaults>(
            0, kInner, [&](std::size_t a, std::size_t b) {
              innerWork.fetch_add(static_cast<int>(b - a),
                                  std::memory_order_relaxed);
            });
      });

  EXPECT_EQ(innerWork.load(), static_cast<int>(kOuter * kInner));
}

TEST(ParallelForNesting, NestedInsideForkJoinTaskCoversChildRangeExactlyOnce) {
  ThreadPool pool(4);
  constexpr std::size_t kTasks = 4;
  constexpr std::size_t kInner = 128;
  std::vector<std::atomic<std::uint32_t>> counts(kTasks * kInner);
  for (auto &c : counts) {
    c.store(0, std::memory_order_relaxed);
  }

  auto runInner = [&](std::size_t task) {
    pool.parallelFor<HintsDefaults>(
        std::size_t{0}, kInner, [&](std::size_t lo, std::size_t hi) {
          for (std::size_t i = lo; i < hi; ++i) {
            counts[(task * kInner) + i].fetch_add(1, std::memory_order_relaxed);
          }
        });
  };

  pool.forkJoin<HintsDefaults>([&] { runInner(0); }, [&] { runInner(1); },
                               [&] { runInner(2); }, [&] { runInner(3); });

  for (std::size_t task = 0; task < kTasks; ++task) {
    for (std::size_t i = 0; i < kInner; ++i) {
      EXPECT_EQ(counts[(task * kInner) + i].load(std::memory_order_relaxed), 1U)
          << "task " << task << " index " << i;
    }
  }
}
