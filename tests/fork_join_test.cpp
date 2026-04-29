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

// Hint preset for fork-join tests. forkJoin uses its own Chase-Lev deque path; the engine does
// not consult `Balance` on this codepath, so the preset only inherits HintsDefaults. The
// cross-CCD case is exercised via the bundled `CcdLocalForkJoinHints` preset.
struct ForkJoinTestHints : HintsDefaults {};

// 4 root tasks, each writes to a distinct slot. Verify every slot was set exactly once.
TEST(ForkJoin, SimpleFourTasks) {
  ThreadPool pool(4);
  std::array<std::atomic<int>, 4> slots{};
  for (auto &s : slots) {
    s.store(0, std::memory_order_relaxed);
  }
  pool.forkJoin<ForkJoinTestHints>([&]() { slots[0].store(11, std::memory_order_relaxed); },
                                   [&]() { slots[1].store(22, std::memory_order_relaxed); },
                                   [&]() { slots[2].store(33, std::memory_order_relaxed); },
                                   [&]() { slots[3].store(44, std::memory_order_relaxed); });
  EXPECT_EQ(slots[0].load(), 11);
  EXPECT_EQ(slots[1].load(), 22);
  EXPECT_EQ(slots[2].load(), 33);
  EXPECT_EQ(slots[3].load(), 44);
}

// Recursive divide-and-conquer Fibonacci computed via forkJoin. Verifies that nested forkJoin
// from inside a worker body terminates and produces the correct result. Uses a depth-limited
// recursion to keep the runtime bounded.
namespace {

int fibForkJoin(ThreadPool &pool, int n) {
  if (n < 2) {
    return n;
  }
  if (n < 12) {
    // Cutoff: below the threshold, run serially to keep the test runtime bounded.
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

TEST(ForkJoin, NestedFibonacci) {
  ThreadPool pool(4);
  const int got = fibForkJoin(pool, 20);
  EXPECT_EQ(got, kReferenceFib20);
}

// One task throws; the producer's join must rethrow the captured exception, and the other tasks
// must complete (or have been cancelled cleanly).
TEST(ForkJoin, ExceptionRethrownAndOthersComplete) {
  ThreadPool pool(4);
  std::array<std::atomic<int>, 4> slots{};
  std::atomic<int> ranCount{0};

  auto block = [&](int idx, int value) {
    return [&, idx, value]() {
      slots[static_cast<std::size_t>(idx)].store(value, std::memory_order_relaxed);
      ranCount.fetch_add(1, std::memory_order_relaxed);
    };
  };

  bool caught = false;
  std::string what;
  try {
    pool.forkJoin<ForkJoinTestHints>(
        block(0, 1), block(1, 2), [&]() { throw std::runtime_error("forkjoin-fail"); },
        block(3, 4));
  } catch (const std::runtime_error &e) {
    caught = true;
    what = e.what();
  }
  EXPECT_TRUE(caught);
  EXPECT_NE(what.find("forkjoin-fail"), std::string::npos);
  // At least one of the non-throwing tasks ran. Some may have been short-circuited by the
  // cancellation flag set on the throw path; the contract is "no UB, captured exception
  // rethrown", not "every task ran".
  EXPECT_GE(ranCount.load(), 1);
}

// Cancellation token stopped before forkJoin call: every task observes cancellation at entry,
// the join still rendezvous, no exception.
TEST(ForkJoin, CancellationBeforeStart) {
  ThreadPool pool(4);
  CancellationToken tok = CancellationToken::makeOwned();
  tok.request_stop();

  std::atomic<int> ranCount{0};
  pool.forkJoin<ForkJoinTestHints>(
      tok, [&]() { ranCount.fetch_add(1, std::memory_order_relaxed); },
      [&]() { ranCount.fetch_add(1, std::memory_order_relaxed); },
      [&]() { ranCount.fetch_add(1, std::memory_order_relaxed); },
      [&]() { ranCount.fetch_add(1, std::memory_order_relaxed); });
  // No body ran (cancellation flag is set before any task admit), and no UB.
  EXPECT_EQ(ranCount.load(), 0);
}

// Cancellation requested mid-flight: partial completion is acceptable, but no UB.
TEST(ForkJoin, CancellationMidFlight) {
  ThreadPool pool(4);
  CancellationToken tok = CancellationToken::makeOwned();
  std::atomic<int> ranCount{0};
  std::atomic<bool> firstObserved{false};

  std::atomic<int> spinSink{0};
  auto longishTask = [&]() {
    ranCount.fetch_add(1, std::memory_order_relaxed);
    firstObserved.store(true, std::memory_order_release);
    // Spin briefly to give the producer time to request stop while other tasks may still be
    // queued. The duration is bounded to keep test runtime small.
    for (int i = 0; i < 200000; ++i) {
      spinSink.fetch_add(1, std::memory_order_relaxed);
    }
  };

  // Spawn a controller thread that requests stop once the first task has begun executing.
  std::thread controller([&]() {
    while (!firstObserved.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    tok.request_stop();
  });

  pool.forkJoin<ForkJoinTestHints>(tok, longishTask, longishTask, longishTask, longishTask,
                                   longishTask, longishTask, longishTask, longishTask);
  controller.join();
  // No assertion on the exact count; cancellation is best-effort. The test passes if
  // forkJoin returns at all (no deadlock) and no UB was triggered.
  SUCCEED();
}

// Determinism at the value level: with the same input the result is the same regardless of
// `nJobs`. The forkJoin-based reduction is associative + commutative over `int`, so the value is
// deterministic even though task order is racy.
TEST(ForkJoin, ResultDeterministicAcrossNJobs) {
  constexpr int kRoot = 24;
  std::vector<int> results;
  results.reserve(5);
  for (const std::size_t nJobs : {1U, 2U, 4U, 8U, 16U}) {
    ThreadPool pool(nJobs);
    const int got = fibForkJoin(pool, kRoot);
    results.push_back(got);
  }
  for (std::size_t i = 1; i < results.size(); ++i) {
    EXPECT_EQ(results[i], results[0]) << "fork-join diverged at nJobs index " << i;
  }
}

// CCD-local affinity hint reaches the forkJoin entry without crashing or stalling. The probe
// order biases stealing toward same-CCD victims; the test verifies end-to-end correctness on
// the affinity-aware path.
TEST(ForkJoin, CcdLocalAffinity) {
  ThreadPool pool(4);
  std::atomic<int> sum{0};
  pool.forkJoin<CcdLocalForkJoinHints>([&]() { sum.fetch_add(1, std::memory_order_relaxed); },
                                       [&]() { sum.fetch_add(2, std::memory_order_relaxed); },
                                       [&]() { sum.fetch_add(4, std::memory_order_relaxed); },
                                       [&]() { sum.fetch_add(8, std::memory_order_relaxed); });
  EXPECT_EQ(sum.load(), 15);
}

// Single participant: every task runs serially on the producer; no fan-out, no deque traffic.
// Verifies the inline-fallback path.
TEST(ForkJoin, SingleParticipantInlineFallback) {
  ThreadPool pool(1);
  std::array<std::atomic<int>, 4> slots{};
  pool.forkJoin<ForkJoinTestHints>([&]() { slots[0].store(1, std::memory_order_relaxed); },
                                   [&]() { slots[1].store(2, std::memory_order_relaxed); },
                                   [&]() { slots[2].store(3, std::memory_order_relaxed); },
                                   [&]() { slots[3].store(4, std::memory_order_relaxed); });
  EXPECT_EQ(slots[0].load(), 1);
  EXPECT_EQ(slots[1].load(), 2);
  EXPECT_EQ(slots[2].load(), 3);
  EXPECT_EQ(slots[3].load(), 4);
}

// Empty task pack: forkJoin returns immediately, no UB, no allocation visible.
TEST(ForkJoin, EmptyTaskPack) {
  ThreadPool pool(4);
  pool.forkJoin<ForkJoinTestHints>();
  SUCCEED();
}

// CPO surface: dispatch through the customization-point-object surface mirrors the member-call
// surface. Verifies the friend tag_invoke wires up correctly.
TEST(ForkJoin, CpoSurface) {
  ThreadPool pool(4);
  std::array<std::atomic<int>, 3> slots{};
  forkJoin.template operator()<ForkJoinTestHints>(
      pool, CancellationToken{}, [&]() { slots[0].store(7, std::memory_order_relaxed); },
      [&]() { slots[1].store(8, std::memory_order_relaxed); },
      [&]() { slots[2].store(9, std::memory_order_relaxed); });
  EXPECT_EQ(slots[0].load(), 7);
  EXPECT_EQ(slots[1].load(), 8);
  EXPECT_EQ(slots[2].load(), 9);
}
