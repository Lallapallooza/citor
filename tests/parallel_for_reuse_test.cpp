#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>

#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::HintsDefaults;
using citor::ThreadPool;

// Reuse-bit regression tests need the StaticUniform path with
// `cancellationChecks = false` and `chunk = 1` to land on the
// same-command-reuse branch the producer's TLS descriptor exposes.
struct ReuseRegressionHints : HintsDefaults {
  static constexpr std::size_t chunk = 1;
  static constexpr bool cancellationChecks = false;
};

// Reuse-bit must be invalidated when the SAME thread dispatches the same
// `parallelFor<H,F>` instantiation onto a DIFFERENT pool. The TLS descriptor
// matches but the new pool's worker mailboxes have never been primed;
// publishing with kReuseBit (which skips the mailboxDesc store) leaves
// background workers running with a null descriptor and skipping the body.
TEST(ParallelForReuse,
     ReuseBitInvalidatedWhenSameProducerSwitchesPoolsBetweenCalls) {
  ThreadPool p1(4);
  ThreadPool p2(4);

  std::atomic<int> count{0};
  auto body = [&count](std::size_t lo, std::size_t hi) noexcept {
    count.fetch_add(static_cast<int>(hi - lo), std::memory_order_relaxed);
  };

  constexpr std::size_t kN = 256;

  // Warm the TLS descriptor on p1 first.
  p1.parallelFor<ReuseRegressionHints>(0, kN, body);
  ASSERT_EQ(count.load(), static_cast<int>(kN));

  count.store(0, std::memory_order_relaxed);

  // First call into p2 with the same instantiation. Current code can publish
  // kReuseBit because the producer's TLS desc still matches p1's prior call,
  // but p2's workers have never been primed.
  p2.parallelFor<ReuseRegressionHints>(0, kN, body);
  EXPECT_EQ(count.load(), static_cast<int>(kN));
}

// Reuse-bit must be invalidated when a DIFFERENT producer thread dispatches the
// same `parallelFor<H,F>` instantiation onto the SAME pool between two of
// producer A's calls. The workers' last full publish carried producer B's
// lambda; reusing on A's second call would run B's body over A's range.
TEST(ParallelForReuse,
     ReuseBitInvalidatedWhenAnotherProducerInterleavesCallsOnSamePool) {
  ThreadPool pool(4);
  std::atomic<int> aCount{0};
  std::atomic<int> bCount{0};
  auto a = [&aCount](std::size_t lo, std::size_t hi) noexcept {
    aCount.fetch_add(static_cast<int>(hi - lo), std::memory_order_relaxed);
  };
  auto b = [&bCount](std::size_t lo, std::size_t hi) noexcept {
    bCount.fetch_add(static_cast<int>(hi - lo), std::memory_order_relaxed);
  };

  constexpr std::size_t kN = 512;

  std::mutex mu;
  std::condition_variable cv;
  bool aFirstDone = false;
  bool bDone = false;

  std::thread producerA([&] {
    pool.parallelFor<ReuseRegressionHints>(0, kN, a);
    {
      const std::scoped_lock lk(mu);
      aFirstDone = true;
    }
    cv.notify_all();
    {
      std::unique_lock<std::mutex> lk(mu);
      cv.wait(lk, [&] { return bDone; });
    }
    aCount.store(0, std::memory_order_relaxed);
    bCount.store(0, std::memory_order_relaxed);
    pool.parallelFor<ReuseRegressionHints>(0, kN, a);
  });
  std::thread producerB([&] {
    {
      std::unique_lock<std::mutex> lk(mu);
      cv.wait(lk, [&] { return aFirstDone; });
    }
    pool.parallelFor<ReuseRegressionHints>(0, kN, b);
    {
      const std::scoped_lock lk(mu);
      bDone = true;
    }
    cv.notify_all();
  });

  producerA.join();
  producerB.join();

  EXPECT_EQ(aCount.load(), static_cast<int>(kN));
  EXPECT_EQ(bCount.load(), 0);
}
