#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <random>
#include <thread>
#include <unordered_set>
#include <vector>

#include "citor/detail/chase_lev_deque.h"

using citor::detail::ChaseLevDeque;

// steal from another thread observes the value the owner pushed earlier.
TEST(ChaseLevDequeConcurrent,
     StealerThreadObservesEveryPushedValueExactlyOnce) {
  ChaseLevDeque<int> dq;
  for (int i = 0; i < 100; ++i) {
    dq.push(i);
  }
  std::atomic<int> stolenCount{0};
  std::vector<int> stolenValues(100, -1);
  std::thread stealer([&]() {
    while (true) {
      auto v = dq.steal();
      if (!v) {
        if (dq.empty()) {
          break;
        }
        std::this_thread::yield();
        continue;
      }
      const int idx = stolenCount.fetch_add(1, std::memory_order_relaxed);
      stolenValues[static_cast<std::size_t>(idx)] = *v;
    }
  });
  stealer.join();

  EXPECT_EQ(stolenCount.load(), 100);
  // Bag semantics: every value 0..99 appears exactly once.
  std::unordered_set<int> seen;
  for (const int v : stolenValues) {
    seen.insert(v);
  }
  EXPECT_EQ(seen.size(), 100U);
  for (int i = 0; i < 100; ++i) {
    EXPECT_NE(seen.find(i), seen.end());
  }
}

// Concurrent push (owner) + steal (multiple stealers): every pushed value is
// consumed exactly once between pop and the stealers' tally. TSan-clean by
// construction; the test fails if the deque drops or duplicates values under
// contention.
TEST(ChaseLevDequeConcurrent,
     ConsumesEveryValueExactlyOnceUnderOwnerPushAndMultipleStealers) {
  ChaseLevDeque<int> dq(/*initialCapacity=*/64);
  constexpr int kPerThread = 5000;
  constexpr int kStealers = 4;

  std::atomic<int> consumed{0};
  std::atomic<bool> producerDone{false};
  std::vector<std::vector<int>> stolenByThread(kStealers);

  std::vector<std::thread> stealers;
  stealers.reserve(kStealers);
  for (int t = 0; t < kStealers; ++t) {
    stealers.emplace_back([&, t]() {
      auto &out = stolenByThread[static_cast<std::size_t>(t)];
      while (!producerDone.load(std::memory_order_acquire) || !dq.empty()) {
        auto v = dq.steal();
        if (!v) {
          std::this_thread::yield();
          continue;
        }
        out.push_back(*v);
        consumed.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  std::vector<int> popped;
  std::mt19937_64 rng(0xC0FFEEULL);
  for (int i = 0; i < kPerThread; ++i) {
    dq.push(i);
    // Occasional owner pop to mix with the steal stream and exercise the
    // last-item race.
    if ((rng() & 0x7U) == 0U) {
      auto v = dq.pop();
      if (v) {
        popped.push_back(*v);
        consumed.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
  // Drain the remainder from the owner side until empty (race against
  // stealers).
  while (true) {
    auto v = dq.pop();
    if (!v) {
      break;
    }
    popped.push_back(*v);
    consumed.fetch_add(1, std::memory_order_relaxed);
  }
  producerDone.store(true, std::memory_order_release);
  for (auto &th : stealers) {
    th.join();
  }

  EXPECT_EQ(consumed.load(), kPerThread);

  // Every value 0..kPerThread-1 was consumed exactly once.
  std::vector<int> all = popped;
  for (auto &v : stolenByThread) {
    all.insert(all.end(), v.begin(), v.end());
  }
  ASSERT_EQ(all.size(), static_cast<std::size_t>(kPerThread));
  std::unordered_set<int> seen;
  for (const int v : all) {
    EXPECT_TRUE(seen.insert(v).second) << "Value " << v << " consumed twice";
  }
  EXPECT_EQ(seen.size(), static_cast<std::size_t>(kPerThread));
  for (int i = 0; i < kPerThread; ++i) {
    EXPECT_NE(seen.find(i), seen.end()) << "Value " << i << " missing";
  }
}
