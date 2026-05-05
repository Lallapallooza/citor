#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::HintsDefaults;
using citor::ThreadPool;

// Blocking semantics: parallelFor returns only after every chunk completes. We
// stage a side-effect (worker writes) that the producer must observe
// synchronously without an additional fence.
TEST(ParallelForBlocking, BlocksUntilEveryChunkHasReturned) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 256;
  std::vector<int> data(kN, 0);

  pool.parallelFor<HintsDefaults>(0, kN, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) {
      data[i] = static_cast<int>(i + 1);
    }
  });

  for (std::size_t i = 0; i < kN; ++i) {
    EXPECT_EQ(data[i], static_cast<int>(i + 1));
  }
}

// FunctionRef lifetime: the wrapping lambda goes out of scope after parallelFor
// returns; the test stays inside one TU and asserts no use-after-free is
// observed (validated under sanitizers).
TEST(ParallelForBlocking, KeepsBoundCallableAliveAcrossEveryWorkerInvocation) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 64;
  std::vector<int> data(kN, 0);
  {
    // `local` is captured by reference so the FunctionRef thunk reads through a
    // real address; the read survives only because parallelFor blocks until
    // every worker returns from the closure, exercising the descriptor's "body
    // alive for the call" contract end-to-end.
    std::atomic<int> local{5};
    pool.parallelFor<HintsDefaults>(
        0, kN, [&data, &local](std::size_t lo, std::size_t hi) {
          const int value = local.load(std::memory_order_relaxed);
          for (std::size_t i = lo; i < hi; ++i) {
            data[i] = value;
          }
        });
  }
  for (std::size_t i = 0; i < kN; ++i) {
    EXPECT_EQ(data[i], 5);
  }
}

// Repeated dispatches reuse the persistent worker pool: 100 back-to-back calls
// all complete and the cumulative coverage is correct.
TEST(ParallelForBlocking,
     OneHundredBackToBackDispatchesAllCompleteUnderRepeatedReuse) {
  ThreadPool pool(4);
  constexpr std::size_t kN = 64;
  std::vector<std::atomic<std::uint32_t>> counts(kN);
  for (auto &c : counts) {
    c.store(0, std::memory_order_relaxed);
  }

  for (int round = 0; round < 100; ++round) {
    pool.parallelFor<HintsDefaults>(0, kN, [&](std::size_t lo, std::size_t hi) {
      for (std::size_t i = lo; i < hi; ++i) {
        counts[i].fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  for (std::size_t i = 0; i < kN; ++i) {
    EXPECT_EQ(counts[i].load(std::memory_order_relaxed), 100U) << "index " << i;
  }
}
