#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_set>

#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::HintsDefaults;
using citor::ThreadPool;

namespace {

struct DistributionResult {
  std::size_t participants;
  std::size_t distinctThreads;
};

// Captures the set of OS thread ids that observed at least one `map`
// invocation across a single `parallelReduce` call. Returns its size.
DistributionResult distinctTidsInParallelReduce(std::size_t requested,
                                                std::size_t n) {
  ThreadPool pool(requested);
  const std::size_t participants = pool.participants();
  std::mutex mu;
  std::unordered_set<std::thread::id> ids;
  std::atomic<std::uint32_t> gate{0};
  const auto target = static_cast<std::uint32_t>(participants);
  (void)pool.parallelReduce<HintsDefaults>(
      0, n, std::size_t{0},
      [&](std::size_t lo, std::size_t hi) {
        const std::thread::id me = std::this_thread::get_id();
        {
          const std::lock_guard<std::mutex> lk(mu);
          ids.insert(me);
        }
        gate.fetch_add(1u, std::memory_order_acq_rel);
        while (gate.load(std::memory_order_acquire) < target) {
          std::this_thread::yield();
        }
        return hi - lo;
      },
      [](std::size_t a, std::size_t b) { return a + b; });
  return {.participants = participants, .distinctThreads = ids.size()};
}

} // namespace

// Each participant slot owns a disjoint contiguous block range under the
// `Static-Contiguous` partition; every slot's worker (incl. producer at slot
// 0) must execute at least one map invocation, so the distinct-thread count
// equals the participant count.
TEST(ParallelReduceDistribution, MapRunsOnEveryParticipantSlotThread) {
  constexpr std::size_t kN = 1u << 14;
  const DistributionResult result = distinctTidsInParallelReduce(4, kN);
  if (result.participants < 2U) {
    GTEST_SKIP() << "pool has " << result.participants << " participant(s)";
  }
  EXPECT_EQ(result.distinctThreads, result.participants);
}

// Single-participant pool collapses to inline on the caller; map runs on the
// calling thread only.
TEST(ParallelReduceDistribution, SingleParticipantPoolRunsMapOnCallerOnly) {
  EXPECT_EQ(distinctTidsInParallelReduce(1, 1u << 12).distinctThreads, 1u);
}
