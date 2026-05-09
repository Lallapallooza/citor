// Back-to-back `parallelFor` + `parallelForRuntime` calls on the same pool
// under CPU oversubscription must establish happens-before from each
// producer dispatch to every worker's read of the per-`WorkerState` fields
// it populated. Run as a TSan stress: concurrent producers each driving
// their own pool, with noise threads consuming the rest of the cores.
//
// On the unfixed cold-collapse path (producer self-stamps `doneSentinel`
// without waiting for the worker's release), TSan reports a `data race` on
// the producer's `JobDescriptor` stack address. Thread and iteration counts
// are tuned to surface the race under TSan.

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

#include "citor/hints.h"
#include "citor/thread_pool.h"

using namespace citor;

namespace {

constexpr std::size_t kPoolThreads = 4;
constexpr std::size_t kProducerThreads = 4;
constexpr std::size_t kIters = 512;
constexpr std::size_t kRange = 256;
constexpr std::size_t kNoiseThreads = 16;

void noiseLoop(std::atomic<bool> &stop) {
  volatile std::uint64_t acc = 0;
  while (!stop.load(std::memory_order_relaxed)) {
    for (int i = 0; i < 1024; ++i) {
      acc += static_cast<std::uint64_t>(i) * 2654435761U;
    }
  }
}

void backToBack() {
  ThreadPool pool(kPoolThreads);

  std::vector<int> compileBuf(kRange, 0);
  std::vector<int> runtimeBuf(kRange, 0);

  Hints runtimeHints;
  runtimeHints.balance = Balance::StaticUniform;
  runtimeHints.estimatedItemNs = 0.0;
  runtimeHints.minTaskUs = 0.0;
  runtimeHints.chunk = 0;

  for (std::size_t it = 0; it < kIters; ++it) {
    pool.parallelFor<HintsDefaults>(
        0, kRange, [&compileBuf](std::size_t lo, std::size_t hi) {
          for (std::size_t i = lo; i < hi; ++i) {
            compileBuf[i] = static_cast<int>(i);
          }
        });

    pool.parallelForRuntime(
        0, kRange,
        [&runtimeBuf](std::size_t lo, std::size_t hi) {
          for (std::size_t i = lo; i < hi; ++i) {
            runtimeBuf[i] = static_cast<int>(i);
          }
        },
        runtimeHints);

    ASSERT_EQ(compileBuf, runtimeBuf);
  }
}

} // namespace

TEST(RegressionBackToBackDispatch,
     AlternatingHintsKindsProduceMatchingOutputsUnderOversubscription) {
  std::atomic<bool> stopNoise{false};

  std::vector<std::thread> noise;
  noise.reserve(kNoiseThreads);
  for (std::size_t i = 0; i < kNoiseThreads; ++i) {
    noise.emplace_back([&stopNoise] { noiseLoop(stopNoise); });
  }

  std::vector<std::thread> producers;
  producers.reserve(kProducerThreads);
  for (std::size_t t = 0; t < kProducerThreads; ++t) {
    producers.emplace_back([] { backToBack(); });
  }

  for (auto &th : producers) {
    th.join();
  }

  stopNoise.store(true, std::memory_order_relaxed);
  for (auto &th : noise) {
    th.join();
  }
}
