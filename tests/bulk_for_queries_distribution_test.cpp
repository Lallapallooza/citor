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

// `bulkForQueries` forwards to `parallelFor(0, q, fn)`. The distinct-thread
// contract is the same: when every chunk synchronously rendezvouses, the
// engine must place each chunk on a distinct OS thread. The barrier-spin
// guards against the cold-collapse fast path absorbing peer slots.
TEST(BulkForQueriesDistribution, BodyRunsOnEveryParticipantSlotThread) {
  constexpr std::size_t kQ = 1u << 14;
  ThreadPool pool(4);
  const auto participants = static_cast<std::uint32_t>(pool.participants());
  if (participants < 2U) {
    GTEST_SKIP() << "pool has " << participants << " participant(s)";
  }
  std::mutex mu;
  std::unordered_set<std::thread::id> ids;
  std::atomic<std::uint32_t> gate{0};
  pool.bulkForQueries<HintsDefaults>(
      kQ, [&](std::size_t /*lo*/, std::size_t /*hi*/) {
        {
          const std::lock_guard<std::mutex> lk(mu);
          ids.insert(std::this_thread::get_id());
        }
        gate.fetch_add(1u, std::memory_order_acq_rel);
        while (gate.load(std::memory_order_acquire) < participants) {
          std::this_thread::yield();
        }
      });
  EXPECT_EQ(ids.size(), participants);
}

TEST(BulkForQueriesDistribution, SingleParticipantPoolRunsBodyOnCallerOnly) {
  ThreadPool pool(1);
  std::unordered_set<std::thread::id> ids;
  pool.bulkForQueries<HintsDefaults>(
      1u << 12, [&](std::size_t /*lo*/, std::size_t /*hi*/) {
        ids.insert(std::this_thread::get_id());
      });
  EXPECT_EQ(ids.size(), 1u);
}
