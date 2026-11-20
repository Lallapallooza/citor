// Regression tests for the lifetime of the producer's `parallelFor`
// descriptor under cold collapse. A worker the producer cold-stamped may read
// the descriptor after the dispatch returns; the descriptor must stay intact
// until that worker acknowledges, both across pools on one thread and across
// the producer thread's exit. Under ThreadSanitizer either violation reports
// a data race on the descriptor.

#include <gtest/gtest.h>

#include "citor/hints.h"
#include "citor/thread_pool.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>

#ifdef __linux__
#include <unistd.h>

#include "test_support_subprocess.h"
#endif

using citor::HintsDefaults;
using citor::ThreadPool;

namespace {

constexpr std::size_t kRange = 1024;

// Runs one `parallelFor` over `[0, kRange)` and returns how many indices the
// body visited.
std::size_t visitRange(ThreadPool &pool) {
  std::atomic<std::size_t> visited{0};
  pool.parallelFor<HintsDefaults>(
      0, kRange, [&](std::size_t lo, std::size_t hi) {
        visited.fetch_add(hi - lo, std::memory_order_relaxed);
      });
  return visited.load(std::memory_order_relaxed);
}

} // namespace

// Each dispatch comes from a new thread that exits right after it. A worker
// cold-stamped by one thread must not read that thread's descriptor after the
// thread is gone: a fresh thread's descriptor can occupy the same storage, the
// worker then misreads the dispatch and never acknowledges it, and the next
// producer waits for that acknowledgement forever. Runs in a child process so
// a hang reads as a timeout.
TEST(RegressionColdCollapseDescLifetime,
     FreshProducerThreadPerDispatchCompletes) {
#ifndef __linux__
  GTEST_SKIP() << "uses fork() to bound a hang";
#else
  const auto outcome = citor_test_support::runInChildWithTimeout(
      [] {
        ThreadPool pool(4);
        constexpr int kThreads = 2000;
        for (int i = 0; i < kThreads; ++i) {
          std::size_t visited = 0;
          std::thread producer([&] { visited = visitRange(pool); });
          producer.join();
          if (visited != kRange) {
            _exit(2);
          }
        }
      },
      std::chrono::seconds(60));
  EXPECT_EQ(outcome, citor_test_support::ChildOutcome::Exited0)
      << "a dispatch from a fresh producer thread did not complete";
#endif
}

// One thread dispatches on two pools back to back. The second dispatch must
// not rewrite the shared descriptor while a worker the first pool cold-stamped
// is still reading it.
TEST(RegressionColdCollapseDescLifetime,
     ProducerAlternatingPoolsVisitsEveryIndexOnce) {
  constexpr int kRounds = 50;
  for (int round = 0; round < kRounds; ++round) {
    // Fresh pools make cold collapse likely: the first dispatch reaches
    // workers that have not started spinning yet.
    ThreadPool first(2);
    ThreadPool second(2);
    ASSERT_EQ(visitRange(first), kRange);
    ASSERT_EQ(visitRange(second), kRange);
    ASSERT_EQ(visitRange(first), kRange);
  }
}
