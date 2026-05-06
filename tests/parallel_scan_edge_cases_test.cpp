#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "citor/cpos/parallel_scan.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::HintsDefaults;
using citor::ThreadPool;

// Empty range: parallelScan returns the identity without dispatching a body
// call.
TEST(ParallelScanEdgeCases, EmptyRangeProducesEmptyOutputAndDoesNotInvokeBody) {
  ThreadPool pool(4);
  std::atomic<int> bodyCalls{0};

  auto body = [&bodyCalls](std::size_t /*chunkId*/, std::size_t /*lo*/,
                           std::size_t /*hi*/, std::int64_t initial,
                           std::int64_t * /*unusedOut*/) -> std::int64_t {
    bodyCalls.fetch_add(1, std::memory_order_relaxed);
    return initial;
  };

  const std::int64_t total = pool.parallelScan<HintsDefaults>(
      0U, std::int64_t{0}, body,
      [](std::int64_t a, std::int64_t b) { return a + b; });

  EXPECT_EQ(total, 0);
  EXPECT_EQ(bodyCalls.load(std::memory_order_relaxed), 0)
      << "Empty scan must not invoke the body";
}

// Single-participant pool: the inline path runs the body exactly once on the
// producer.
TEST(ParallelScanEdgeCases,
     SingleParticipantPoolStillProducesCorrectInclusivePrefix) {
  ThreadPool pool(1);
  constexpr std::size_t kN = 1024;
  std::vector<std::int64_t> in(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    in[i] = static_cast<std::int64_t>(i + 1);
  }

  std::atomic<int> bodyCalls{0};
  auto body = [&in, &bodyCalls](std::size_t /*chunkId*/, std::size_t lo,
                                std::size_t hi, std::int64_t /*initial*/,
                                std::int64_t * /*unusedOut*/) -> std::int64_t {
    bodyCalls.fetch_add(1, std::memory_order_relaxed);
    std::int64_t s = 0;
    for (std::size_t i = lo; i < hi; ++i) {
      s += in[i];
    }
    return s;
  };

  const std::int64_t total = pool.parallelScan<HintsDefaults>(
      kN, std::int64_t{0}, body,
      [](std::int64_t a, std::int64_t b) { return a + b; });

  EXPECT_EQ(total, static_cast<std::int64_t>(kN) *
                       static_cast<std::int64_t>(kN + 1) / 2);
  EXPECT_EQ(bodyCalls.load(std::memory_order_relaxed), 1)
      << "Single-participant scan must invoke the body exactly once";
}
