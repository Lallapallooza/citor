#include <gtest/gtest.h>

#include <cstddef>
#include <mutex>
#include <thread>
#include <unordered_set>

#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::HintsDefaults;
using citor::ThreadPool;

namespace {

// Captures the set of OS thread ids that observed at least one `map`
// invocation across a single `parallelReduce` call. Returns its size.
std::size_t distinctTidsInParallelReduce(std::size_t participants,
                                         std::size_t n) {
  ThreadPool pool(participants);
  std::mutex mu;
  std::unordered_set<std::thread::id> ids;
  (void)pool.parallelReduce<HintsDefaults>(
      0, n, std::size_t{0},
      [&](std::size_t lo, std::size_t hi) {
        const std::thread::id me = std::this_thread::get_id();
        {
          const std::lock_guard<std::mutex> lk(mu);
          ids.insert(me);
        }
        return hi - lo;
      },
      [](std::size_t a, std::size_t b) { return a + b; });
  return ids.size();
}

} // namespace

// Each participant slot owns a disjoint contiguous block range under the
// `Static-Contiguous` partition; every slot's worker (incl. producer at slot
// 0) must execute at least one map invocation, so the distinct-thread count
// equals the participant count.
TEST(ParallelReduceDistribution, MapRunsOnEveryParticipantSlotThread) {
  constexpr std::size_t kN = 1u << 14;
  EXPECT_EQ(distinctTidsInParallelReduce(2, kN), 2u);
  EXPECT_EQ(distinctTidsInParallelReduce(4, kN), 4u);
}

// Single-participant pool collapses to inline on the caller; map runs on the
// calling thread only.
TEST(ParallelReduceDistribution, SingleParticipantPoolRunsMapOnCallerOnly) {
  EXPECT_EQ(distinctTidsInParallelReduce(1, 1u << 12), 1u);
}
