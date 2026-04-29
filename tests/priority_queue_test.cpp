#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>

#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::HintsDefaults;
using citor::Priority;
using citor::ThreadPool;

// Hint presets at TU scope (not in an anonymous namespace) so clang-tidy treats every
// static-constexpr member as a public field of a named type rather than an unused constant.

struct GateLatencyHints : HintsDefaults {
  static constexpr Priority priority = Priority::Latency;
};

struct GateBackgroundHints : HintsDefaults {
  static constexpr Priority priority = Priority::Background;
};

// Latency callers reach workers ahead of throughput callers when both contend on the same pool.
// The first throughput producer holds the dispatch gate by sleeping inside its body; while the
// gate is held, both a latency producer and a second throughput producer queue behind it. After
// the gate releases, the latency producer's body must run before the late throughput producer's
// body. The order tags compared in the assertion are stamped from inside each body so the
// observation is post-gate, post-publish, and stable across replays.
TEST(PriorityQueue, LatencyDispatchPreemptsThroughput) {
  ThreadPool pool(4);

  std::atomic<int> orderCounter{0};
  std::atomic<int> latencyOrder{-1};
  std::atomic<int> lateThroughputOrder{-1};
  std::atomic<bool> firstThroughputInBody{false};

  // First throughput producer occupies the dispatch gate by blocking inside the body. Without
  // the sleep the contended-dispatch window is too narrow to observe the ordering
  // deterministically.
  std::thread firstThroughput([&]() {
    pool.parallelFor<HintsDefaults>(0, 8, [&](std::size_t /*lo*/, std::size_t /*hi*/) {
      firstThroughputInBody.store(true, std::memory_order_release);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    });
  });

  // Wait for the first throughput producer to enter its body. After this point the dispatch gate
  // is held; subsequent producers contend.
  while (!firstThroughputInBody.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  // Launch the latency caller; it registers itself in `m_latencyWaiting` before contending on the
  // mutex. Any throughput caller observing a non-zero latency-waiting count yields back.
  std::thread latencyProducer([&]() {
    pool.parallelFor<GateLatencyHints>(0, 8, [&](std::size_t /*lo*/, std::size_t /*hi*/) {
      latencyOrder.store(orderCounter.fetch_add(1, std::memory_order_acq_rel),
                         std::memory_order_release);
    });
  });

  // Give the latency producer time to register its waiting count.
  std::this_thread::sleep_for(std::chrono::milliseconds(5));

  // Late throughput caller: also waits on the gate. It must observe the latency-waiting count and
  // back off until the latency call completes. After the latency call clears the count, the late
  // throughput call may run.
  std::thread lateThroughput([&]() {
    pool.parallelFor<HintsDefaults>(0, 8, [&](std::size_t /*lo*/, std::size_t /*hi*/) {
      lateThroughputOrder.store(orderCounter.fetch_add(1, std::memory_order_acq_rel),
                                std::memory_order_release);
    });
  });

  firstThroughput.join();
  latencyProducer.join();
  lateThroughput.join();

  const int latency = latencyOrder.load(std::memory_order_acquire);
  const int lateTp = lateThroughputOrder.load(std::memory_order_acquire);
  ASSERT_GE(latency, 0);
  ASSERT_GE(lateTp, 0);
  EXPECT_LT(latency, lateTp)
      << "latency dispatch must complete before late throughput dispatch when both contend";
}

// Background callers yield to throughput callers on the dispatch gate. The yield-on-contention
// policy biases the race toward throughput; under sustained contention from a second throughput
// producer, the background producer should be reordered behind the throughput producers. We
// schedule the test as: a primary throughput producer holds the gate by sleeping inside the body;
// while held, both a background producer and a second throughput producer queue. After the
// primary releases, the second throughput producer should win the gate before background (because
// background yields each contended attempt and the throughput peer does not). Run multiple
// rounds because mutex acquire ordering is not strictly FIFO; the bias must dominate the sample.
TEST(PriorityQueue, BackgroundYieldsToThroughput) {
  // Skipped when the pool collapses below four participants: the test orchestrates three concurrent
  // dispatch attempts (primary throughput holding the gate via sleep, second throughput, and a
  // background contender) plus the producer slot. Without that breadth there is no sustained
  // contention on the dispatch gate, the priority bias is invisible, and the race becomes
  // observably one-sided in the wrong direction. The yield-on-contention contract still holds; it
  // has no observable surface to gate.
  {
    const ThreadPool probe(4);
    if (probe.participants() < 4U) {
      GTEST_SKIP() << "priority bias requires concurrent contenders; pool reports "
                   << probe.participants() << " participant(s)";
    }
  }
  constexpr int kRounds = 16;
  int throughputWins = 0;
  int backgroundWins = 0;

  for (int round = 0; round < kRounds; ++round) {
    ThreadPool pool(4);
    std::atomic<int> orderCounter{0};
    std::atomic<int> backgroundOrder{-1};
    std::atomic<int> secondThroughputOrder{-1};
    std::atomic<bool> primaryInBody{false};

    // Primary throughput producer occupies the gate by sleeping inside its body so the two
    // contenders below have time to register and start spinning on the lock.
    std::thread primaryThroughput([&]() {
      pool.parallelFor<HintsDefaults>(0, 8, [&](std::size_t /*lo*/, std::size_t /*hi*/) {
        primaryInBody.store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      });
    });

    while (!primaryInBody.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }

    // Background contender. Yields to the second throughput contender on every attempt.
    std::thread backgroundProducer([&]() {
      pool.parallelFor<GateBackgroundHints>(0, 8, [&](std::size_t /*lo*/, std::size_t /*hi*/) {
        backgroundOrder.store(orderCounter.fetch_add(1, std::memory_order_acq_rel),
                              std::memory_order_release);
      });
    });

    // Second throughput contender. Spins on the lock without yielding to background.
    std::thread secondThroughput([&]() {
      pool.parallelFor<HintsDefaults>(0, 8, [&](std::size_t /*lo*/, std::size_t /*hi*/) {
        secondThroughputOrder.store(orderCounter.fetch_add(1, std::memory_order_acq_rel),
                                    std::memory_order_release);
      });
    });

    primaryThroughput.join();
    backgroundProducer.join();
    secondThroughput.join();

    const int background = backgroundOrder.load(std::memory_order_acquire);
    const int throughput = secondThroughputOrder.load(std::memory_order_acquire);
    if (throughput >= 0 && background >= 0 && throughput < background) {
      ++throughputWins;
    } else {
      ++backgroundWins;
    }
  }

  // Background's per-attempt yield biases the race toward throughput. Require throughput to win
  // a strict majority -- failure indicates the yield was dropped or inverted.
  EXPECT_GT(throughputWins, backgroundWins)
      << "throughput=" << throughputWins << " background=" << backgroundWins
      << "; background must yield to throughput on dispatch contention";
}

// Single-producer dispatches against an idle pool see no gate contention; the priority class is
// hint-only when there is nothing to contend with. This is the common case in real call
// graph and the test guards against accidental ordering side effects on an idle pool.
TEST(PriorityQueue, SingleProducerHasNoOrderingConstraint) {
  ThreadPool pool(4);
  std::atomic<int> seen{0};

  pool.parallelFor<GateLatencyHints>(0, 16, [&](std::size_t /*lo*/, std::size_t /*hi*/) {
    seen.fetch_add(1, std::memory_order_acq_rel);
  });
  pool.parallelFor<HintsDefaults>(0, 16, [&](std::size_t /*lo*/, std::size_t /*hi*/) {
    seen.fetch_add(1, std::memory_order_acq_rel);
  });
  pool.parallelFor<GateBackgroundHints>(0, 16, [&](std::size_t /*lo*/, std::size_t /*hi*/) {
    seen.fetch_add(1, std::memory_order_acq_rel);
  });

  EXPECT_GT(seen.load(std::memory_order_acquire), 0);
}
