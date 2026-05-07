#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>

#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::HintsDefaults;
using citor::ThreadPool;

struct LifecycleHotHints : HintsDefaults {
  static constexpr std::size_t chunk = 1;
  static constexpr bool cancellationChecks = false;
};

TEST(ThreadPoolLifecycleLowLatencyScope,
     LowLatencyScopeKeepsWorkersHotAcrossBackToBackDispatchBursts) {
  ThreadPool pool(4);
  const std::size_t participants = pool.participants();
  ASSERT_GE(participants, 1U);

  std::atomic<std::size_t> visited{0};
  {
    const auto producerGuard = pool.bindProducerSlot();
    const auto hotGuard = pool.lowLatencyScope();
    for (int iter = 0; iter < 256; ++iter) {
      pool.parallelFor<LifecycleHotHints>(
          0, participants, [&](std::size_t lo, std::size_t hi) {
            visited.fetch_add(hi - lo, std::memory_order_relaxed);
          });
    }
  }

  EXPECT_EQ(visited.load(std::memory_order_relaxed),
            participants * std::size_t{256});
}

// Two LowLatencyGuard constructors racing on the same pool must not deadlock.
// The ctor's wait must allow workers to acknowledge any equal-or-newer epoch,
// otherwise the loser's exact-equality wait blocks forever once the winner's
// epoch has been observed first.
TEST(ThreadPoolLifecycleLowLatencyScope,
     ConcurrentLowLatencyScopesFromMultipleProducersDoNotDeadlock) {
  ThreadPool pool(4);

  std::atomic<bool> go{false};
  auto buildGuard = [&] {
    while (!go.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    auto g = pool.lowLatencyScope();
  };

  std::thread t1(buildGuard);
  std::thread t2(buildGuard);
  go.store(true, std::memory_order_release);

  const auto start = std::chrono::steady_clock::now();
  std::atomic<bool> joined{false};
  std::thread joiner([&] {
    t1.join();
    t2.join();
    joined.store(true, std::memory_order_release);
  });
  while (!joined.load(std::memory_order_acquire)) {
    if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
      FAIL() << "concurrent LowLatencyGuard ctors deadlocked (>2s)";
      std::terminate();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  joiner.join();
}
