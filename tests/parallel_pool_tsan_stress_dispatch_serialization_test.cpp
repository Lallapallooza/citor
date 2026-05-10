#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

#include "citor/chain.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::ChainHintsDefaults;
using citor::HintsDefaults;
using citor::staticStage;
using citor::ThreadPool;

struct StressForkJoinHints : HintsDefaults {
  static constexpr citor::StealPolicy stealPolicy =
      citor::StealPolicy::ClusterLocal;
};

// Two producer threads dispatch synchronous primitives to the same pool
// concurrently. The dispatch gate must serialize them around shared pool
// scratch (`m_chainDoneSlots`, `m_workerDeques`, per-task descriptor pointers).
// Without the gate held during pre-publish scratch mutation, a TSan run flags a
// data race in `runOneTask` / Chase-Lev push.
TEST(ParallelPoolTsanStressDispatchSerialization,
     ConcurrentForkJoinProducersAreSerializedByDispatchGate) {
  ThreadPool pool(4);

  constexpr int kIters = 1000;
  std::atomic<int> sumA{0};
  std::atomic<int> sumB{0};

  auto runForkJoin = [&pool](std::atomic<int> &accum) {
    for (int i = 0; i < kIters; ++i) {
      pool.forkJoin<StressForkJoinHints>(
          [&] { accum.fetch_add(1, std::memory_order_relaxed); },
          [&] { accum.fetch_add(1, std::memory_order_relaxed); },
          [&] { accum.fetch_add(1, std::memory_order_relaxed); },
          [&] { accum.fetch_add(1, std::memory_order_relaxed); });
    }
  };

  std::thread t1([&] { runForkJoin(sumA); });
  std::thread t2([&] { runForkJoin(sumB); });
  t1.join();
  t2.join();

  EXPECT_EQ(sumA.load(), 4 * kIters);
  EXPECT_EQ(sumB.load(), 4 * kIters);
}

// Same shape as the forkJoin variant but exercises `parallelChain` so the
// `m_chainDoneSlots` reset is covered by the dispatch lease.
TEST(ParallelPoolTsanStressDispatchSerialization,
     ConcurrentParallelChainProducersAreSerializedByDispatchGate) {
  ThreadPool pool(4);

  constexpr int kIters = 200;
  std::atomic<std::uint64_t> sumA{0};
  std::atomic<std::uint64_t> sumB{0};

  auto run = [&pool](std::atomic<std::uint64_t> &accum) {
    for (int i = 0; i < kIters; ++i) {
      pool.parallelChain<ChainHintsDefaults>(
          64U,
          staticStage("a",
                      [&](std::size_t /*idx*/, std::uint32_t /*slot*/,
                          std::size_t lo, std::size_t hi) {
                        accum.fetch_add(hi - lo, std::memory_order_relaxed);
                      }),
          staticStage("b", [&](std::size_t /*idx*/, std::uint32_t /*slot*/,
                               std::size_t lo, std::size_t hi) {
            accum.fetch_add(hi - lo, std::memory_order_relaxed);
          }));
    }
  };

  std::thread t1([&] { run(sumA); });
  std::thread t2([&] { run(sumB); });
  t1.join();
  t2.join();

  const std::uint64_t expected =
      2ULL * 64ULL * static_cast<std::uint64_t>(kIters);
  EXPECT_EQ(sumA.load(), expected);
  EXPECT_EQ(sumB.load(), expected);
}
