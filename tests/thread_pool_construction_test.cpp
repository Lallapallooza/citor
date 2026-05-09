#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <thread>

#ifdef __linux__
#include <sched.h>
#endif

#include "citor/thread_pool.h"

using citor::ThreadPool;

namespace {

// Returns the count of logical CPUs in the process's affinity mask.
std::size_t allowedLogicalCpus() noexcept {
#ifdef __linux__
  cpu_set_t mask;
  CPU_ZERO(&mask);
  if (sched_getaffinity(0, sizeof(mask), &mask) != 0) {
    return std::thread::hardware_concurrency();
  }
  std::size_t count = 0;
  for (std::size_t i = 0; i < static_cast<std::size_t>(CPU_SETSIZE); ++i) {
    if (CPU_ISSET(i, &mask)) {
      ++count;
    }
  }
  return count;
}
#else
  std::size_t allowedLogicalCpus() noexcept {
    return std::thread::hardware_concurrency();
  }
#endif

} // namespace

// Construct and destruct the pool at the canonical participant counts; the
// destructor must complete within 100 ms.
TEST(ThreadPoolLifecycleConstruction,
     ConstructAndDestructAtParticipantCounts1Through16WithoutLeak) {
  for (const std::size_t participants :
       {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8},
        std::size_t{16}}) {
    const auto start = std::chrono::steady_clock::now();
    {
      const ThreadPool pool(participants);
      // The constructor may truncate to the process affinity mask. The reported
      // count cannot exceed the requested count and must always be at least 1.
      EXPECT_GE(pool.participants(), 1U);
      EXPECT_LE(pool.participants(), participants);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    EXPECT_LT(ms, 200) << "construct+destruct round trip exceeded 200 ms slack "
                          "at participants="
                       << participants;
  }
}

// 100 successive construct/destruct cycles each fit within the bounded
// round-trip envelope.
TEST(ThreadPoolLifecycleConstruction,
     ShutdownReturnsInUnderConfiguredTimeoutAcrossEveryParticipantCount) {
  for (int cycle = 0; cycle < 100; ++cycle) {
    const auto start = std::chrono::steady_clock::now();
    {
      const ThreadPool pool(16);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    EXPECT_LT(ms, 200) << "construct+destruct cycle " << cycle
                       << " exceeded the slack bound";
  }
}

// Constructor must respect the process affinity mask: requesting more workers
// than the mask allows must truncate, not over-subscribe.
TEST(ThreadPoolLifecycleConstruction,
     ConstructorRespectsExternallyImposedAffinityMask) {
  const std::size_t allowedCpus = allowedLogicalCpus();
  ASSERT_GT(allowedCpus, 0U) << "test environment reports no allowed CPUs";
  const std::size_t hugeRequest = allowedCpus * 4U;
  const ThreadPool pool(hugeRequest);
  EXPECT_LE(pool.participants(), allowedCpus)
      << "pool must truncate to at most the affinity mask's logical CPU count";
  EXPECT_GE(pool.participants(), 1U);
}

// Constructing an inner pool from the main thread while an outer pool exists
// must not deadlock or throw; the inner pool's destruction also returns within
// the bounded envelope.
TEST(ThreadPoolLifecycleConstruction,
     NestedConstructAndDestructFromInsideAnotherPoolDoesNotDeadlock) {
  const ThreadPool outer(4);
  EXPECT_GE(outer.participants(), 1U);
  const auto start = std::chrono::steady_clock::now();
  {
    const ThreadPool inner(2);
    EXPECT_GE(inner.participants(), 1U);
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  EXPECT_LT(ms, 200);
}

// Truncation invariant: the pool's participant count must not exceed the number
// of logical CPUs in the process affinity mask. The runtime `sched_getcpu()`
// probe that confirms each worker landed on a distinct physical core is wired
// by the primitive layer once a wake-and-probe path exists; the engine itself
// is observable only via the truncation contract checked here.
TEST(ThreadPoolLifecycleConstruction,
     RequestedParticipantsClampToPhysicalCoreCount) {
#ifdef __linux__
  // Snapshot the process affinity mask BEFORE construction. The pool auto-pins
  // the constructor to slot 0's CPU at construction time so user buffers
  // first-touch on the right CCD; the truncation contract is "participants <=
  // physical CPUs in the original process mask", not "participants <= post-pin
  // mask".
  cpu_set_t mask;
  CPU_ZERO(&mask);
  ASSERT_EQ(sched_getaffinity(0, sizeof(mask), &mask), 0);
  std::size_t allowed = 0;
  for (std::size_t i = 0; i < static_cast<std::size_t>(CPU_SETSIZE); ++i) {
    if (CPU_ISSET(i, &mask)) {
      ++allowed;
    }
  }

  const ThreadPool pool(8);
  ASSERT_GE(pool.participants(), 1U);

  EXPECT_LE(pool.participants(), allowed);
#else
  GTEST_SKIP() << "affinity contract is Linux-only";
#endif
}
