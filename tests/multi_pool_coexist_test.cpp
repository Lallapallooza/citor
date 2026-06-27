#include <gtest/gtest.h>

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>

#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::StaticHints;
using citor::ThreadPool;

namespace {

// Count the distinct worker slots that executed at least one block of a
// static-uniform parallelFor. A pool that fanned out runs blocks on more than
// one slot; a pool collapsed to a single participant runs everything on slot 0.
std::size_t distinctWorkerSlots(ThreadPool &pool) {
  std::atomic<std::uint64_t> seen{0};
  const std::size_t n = std::size_t{8} << 20;
  pool.parallelFor<StaticHints>(
      std::size_t{0}, n, [&](std::size_t lo, std::size_t hi) {
        const std::size_t slot = ThreadPool::workerIndex();
        if (slot < 64) {
          seen.fetch_or(std::uint64_t{1} << slot, std::memory_order_relaxed);
        }
        volatile double acc = 0;
        for (std::size_t i = lo; i < hi; ++i) {
          acc += static_cast<double>(i);
        }
      });
  return static_cast<std::size_t>(std::popcount(seen.load()));
}

} // namespace

// A second standalone pool, constructed while the first still holds its
// producer auto-pin, keeps its full worker count and fans out. Before the
// topology baseline fix, detectTopology read the producer's single-CPU pinned
// mask and the second pool collapsed to participants() == 1 and ran serial. The
// realized participant count is bounded by the host's physical-core count, so
// the second pool must match the first rather than the requested count.
TEST(MultiPoolCoexist, SecondStandalonePoolKeepsItsWorkers) {
  ThreadPool first(4);
  const std::size_t participants = first.participants();
  if (participants < 2U) {
    GTEST_SKIP() << "pool has " << participants << " participant(s)";
  }
  EXPECT_GT(distinctWorkerSlots(first), std::size_t{1});

  ThreadPool second(4);
  EXPECT_EQ(second.participants(), participants);
  EXPECT_GT(distinctWorkerSlots(second), std::size_t{1});

  // The first pool still fans out after the second is built.
  EXPECT_GT(distinctWorkerSlots(first), std::size_t{1});
}

// Independent algorithms with their own worker budgets: three pools coexist,
// each matching the host's per-pool participant count and fanning out on its
// own.
TEST(MultiPoolCoexist, ThreeBudgetsCoexist) {
  ThreadPool a(4);
  const std::size_t participants = a.participants();
  if (participants < 2U) {
    GTEST_SKIP() << "pool has " << participants << " participant(s)";
  }
  ThreadPool b(4);
  ThreadPool c(4);
  EXPECT_EQ(b.participants(), participants);
  EXPECT_EQ(c.participants(), participants);
  EXPECT_GT(distinctWorkerSlots(a), std::size_t{1});
  EXPECT_GT(distinctWorkerSlots(b), std::size_t{1});
  EXPECT_GT(distinctWorkerSlots(c), std::size_t{1});
}
