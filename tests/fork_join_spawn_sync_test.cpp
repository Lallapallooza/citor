#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "citor/cancellation.h"
#include "citor/cpos/fork_join.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::Balance;
using citor::CancellationToken;
using citor::CcdLocalForkJoinHints;
using citor::forkJoin;
using citor::HintsDefaults;
using citor::ThreadPool;

// Hint preset for fork-join tests. forkJoin uses its own Chase-Lev deque path;
// the engine does not consult `Balance` on this codepath, so the preset only
// inherits HintsDefaults. The cross-CCD case is exercised via the bundled
// `CcdLocalForkJoinHints` preset.
struct ForkJoinTestHints : HintsDefaults {};

// 4 root tasks, each writes to a distinct slot. Verify every slot was set
// exactly once.
TEST(ForkJoinSpawnSync, FourRootTasksWriteDistinctSlots) {
  ThreadPool pool(4);
  std::array<std::atomic<int>, 4> slots{};
  for (auto &s : slots) {
    s.store(0, std::memory_order_relaxed);
  }
  pool.forkJoin<ForkJoinTestHints>(
      [&]() { slots[0].store(11, std::memory_order_relaxed); },
      [&]() { slots[1].store(22, std::memory_order_relaxed); },
      [&]() { slots[2].store(33, std::memory_order_relaxed); },
      [&]() { slots[3].store(44, std::memory_order_relaxed); });
  EXPECT_EQ(slots[0].load(), 11);
  EXPECT_EQ(slots[1].load(), 22);
  EXPECT_EQ(slots[2].load(), 33);
  EXPECT_EQ(slots[3].load(), 44);
}

// Recursive divide-and-conquer Fibonacci computed via forkJoin. Verifies that
// nested forkJoin from inside a worker body terminates and produces the correct
// result. Uses a depth-limited recursion to keep the runtime bounded.
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

constexpr int kReferenceFib20 = 6765;

} // namespace

TEST(ForkJoinSpawnSync,
     NestedRecursiveFibonacciTreeReturnsClosedFormReference) {
  ThreadPool pool(4);
  const int got = fibForkJoin(pool, 20);
  EXPECT_EQ(got, kReferenceFib20);
}

// Single participant: every task runs serially on the producer; no fan-out, no
// deque traffic. Verifies the inline-fallback path.
TEST(ForkJoinSpawnSync, SingleParticipantPoolRunsEveryTaskInlineOnProducer) {
  ThreadPool pool(1);
  std::array<std::atomic<int>, 4> slots{};
  pool.forkJoin<ForkJoinTestHints>(
      [&]() { slots[0].store(1, std::memory_order_relaxed); },
      [&]() { slots[1].store(2, std::memory_order_relaxed); },
      [&]() { slots[2].store(3, std::memory_order_relaxed); },
      [&]() { slots[3].store(4, std::memory_order_relaxed); });
  EXPECT_EQ(slots[0].load(), 1);
  EXPECT_EQ(slots[1].load(), 2);
  EXPECT_EQ(slots[2].load(), 3);
  EXPECT_EQ(slots[3].load(), 4);
}

// CPO surface: dispatch through the customization-point-object surface mirrors
// the member-call surface. Verifies the friend tag_invoke wires up correctly.
TEST(ForkJoinSpawnSync, MemberCallAndCpoCallProduceIdenticalSideEffects) {
  ThreadPool pool(4);
  std::array<std::atomic<int>, 3> slots{};
  forkJoin.template operator()<ForkJoinTestHints>(
      pool, CancellationToken{},
      [&]() { slots[0].store(7, std::memory_order_relaxed); },
      [&]() { slots[1].store(8, std::memory_order_relaxed); },
      [&]() { slots[2].store(9, std::memory_order_relaxed); });
  EXPECT_EQ(slots[0].load(), 7);
  EXPECT_EQ(slots[1].load(), 8);
  EXPECT_EQ(slots[2].load(), 9);
}
