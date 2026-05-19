#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <mutex>
#include <thread>
#include <unordered_set>

#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::HintsDefaults;
using citor::ThreadPool;

namespace {

// Per-task body: spin briefly on an atomic counter so siblings have time to
// wake before the producer's slot 0 finishes. Without the spin a fast K=4
// pool can drain slot 0's task and then satisfy 1, 2, 3 from the same
// thread before workers arrive.
void barrierSpin(std::atomic<std::uint32_t> &gate, std::uint32_t target) {
  gate.fetch_add(1u, std::memory_order_acq_rel);
  while (gate.load(std::memory_order_acquire) < target) {
    std::this_thread::yield();
  }
}

} // namespace

// Four-task forkJoin on a four-participant pool: every task must run on a
// distinct OS thread. A regression that inlines forkJoin onto the producer
// would yield one id and fail here.
TEST(ForkJoinDistribution, FourTasksRunOnFourDistinctThreads) {
  ThreadPool pool(4);
  std::mutex mu;
  std::unordered_set<std::thread::id> ids;
  std::atomic<std::uint32_t> gate{0};
  const auto recordAndSync = [&]() {
    const std::thread::id me = std::this_thread::get_id();
    {
      const std::lock_guard<std::mutex> lk(mu);
      ids.insert(me);
    }
    barrierSpin(gate, 4u);
  };
  pool.forkJoin<HintsDefaults>(recordAndSync, recordAndSync, recordAndSync,
                               recordAndSync);
  EXPECT_EQ(ids.size(), 4u);
}

// Single-participant forkJoin runs every task inline on the caller; the
// `participants <= 1` path in `runForkJoinOuter` does not spawn deque pushes.
TEST(ForkJoinDistribution, SingleParticipantPoolRunsTasksOnCallerOnly) {
  ThreadPool pool(1);
  std::mutex mu;
  std::unordered_set<std::thread::id> ids;
  const auto record = [&]() {
    const std::thread::id me = std::this_thread::get_id();
    const std::lock_guard<std::mutex> lk(mu);
    ids.insert(me);
  };
  pool.forkJoin<HintsDefaults>(record, record, record, record);
  EXPECT_EQ(ids.size(), 1u);
}
