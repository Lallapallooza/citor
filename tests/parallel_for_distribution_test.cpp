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

// Each chunk announces its arrival on a shared gate and waits for every
// other chunk to do the same. This guarantees all K participant slots
// actually run their block instead of slot 0 cold-collapsing peers via
// the dispatch fast path.
DistributionResult distinctTidsInParallelFor(std::size_t requested,
                                             std::size_t n) {
  ThreadPool pool(requested);
  const std::size_t participants = pool.participants();
  std::mutex mu;
  std::unordered_set<std::thread::id> ids;
  std::atomic<std::uint32_t> gate{0};
  const auto target = static_cast<std::uint32_t>(participants);
  pool.parallelFor<HintsDefaults>(
      0, n, [&](std::size_t /*lo*/, std::size_t /*hi*/) {
        {
          const std::lock_guard<std::mutex> lk(mu);
          ids.insert(std::this_thread::get_id());
        }
        gate.fetch_add(1u, std::memory_order_acq_rel);
        while (gate.load(std::memory_order_acquire) < target) {
          std::this_thread::yield();
        }
      });
  return {.participants = participants, .distinctThreads = ids.size()};
}

} // namespace

// Codifies the parallel-dispatch contract: when every chunk synchronously
// rendezvouses inside its body, the engine MUST place each chunk on a
// distinct OS thread (one per participant slot). A regression that
// silently fell through to inline-on-producer would deadlock the gate
// for K > 1; a regression that ignored workers would deadlock too.
TEST(ParallelForDistribution, BodyRunsOnEveryParticipantSlotThread) {
  constexpr std::size_t kN = 1u << 14;
  const DistributionResult result = distinctTidsInParallelFor(4, kN);
  if (result.participants < 2U) {
    GTEST_SKIP() << "pool has " << result.participants << " participant(s)";
  }
  EXPECT_EQ(result.distinctThreads, result.participants);
}

TEST(ParallelForDistribution, SingleParticipantPoolRunsBodyOnCallerOnly) {
  ThreadPool pool(1);
  std::unordered_set<std::thread::id> ids;
  pool.parallelFor<HintsDefaults>(0, 1u << 12,
                                  [&](std::size_t /*lo*/, std::size_t /*hi*/) {
                                    ids.insert(std::this_thread::get_id());
                                  });
  EXPECT_EQ(ids.size(), 1u);
}
