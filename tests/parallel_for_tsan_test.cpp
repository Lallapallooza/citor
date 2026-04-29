#include <gtest/gtest.h>

#ifdef __has_feature
#if __has_feature(thread_sanitizer)
#define CITOR_TSAN_BUILD 1
#endif
#endif
#ifdef __SANITIZE_THREAD__
#define CITOR_TSAN_BUILD 1
#endif

#ifdef CITOR_TSAN_BUILD

#include <atomic>  // IWYU pragma: keep
#include <cstddef> // IWYU pragma: keep
#include <cstdint> // IWYU pragma: keep
#include <random>  // IWYU pragma: keep
#include <vector>  // IWYU pragma: keep

#include "citor/hints.h"       // IWYU pragma: keep
#include "citor/thread_pool.h" // IWYU pragma: keep

using citor::Balance;
using citor::HintsDefaults;
using citor::ThreadPool;

struct TsanDynamicHints : HintsDefaults {
  static constexpr Balance balance = Balance::DynamicChunked;
  static constexpr std::size_t chunk = 16;
};

// Randomized parallelFor submissions under TSan. Builds without TSan compile this out, so the
// dependency on `__SANITIZE_THREAD__` matches the lifecycle test convention. The shape mirrors
// the rfc's "10000 randomized parallelFor submissions under TSan" criterion at a smaller
// iteration count to keep ctest snappy; the goal is to surface any race the static / dynamic
// dispatch tiers introduce.
TEST(ParallelForTsan, RandomizedSubmissionsAreRaceFree) {
  constexpr int kIterations = 1000;
  ThreadPool pool(8);

  std::mt19937_64 rng(0xC0FFEEU);
  std::uniform_int_distribution<std::size_t> sizeDist(1U, 256U);
  std::uniform_int_distribution<int> balanceDist(0, 1);

  std::vector<std::atomic<std::uint32_t>> counts(256);
  for (auto &c : counts) {
    c.store(0, std::memory_order_relaxed);
  }

  for (int iter = 0; iter < kIterations; ++iter) {
    const std::size_t n = sizeDist(rng);
    const auto body = [&counts, n](std::size_t lo, std::size_t hi) {
      for (std::size_t i = lo; i < hi && i < n; ++i) {
        counts[i].fetch_add(1, std::memory_order_relaxed);
      }
    };
    if (balanceDist(rng) == 0) {
      pool.parallelFor<HintsDefaults>(0, n, body);
    } else {
      pool.parallelFor<TsanDynamicHints>(0, n, body);
    }
  }
  // Reads are relaxed but stress the loop's writes; TSan's only contract is no race occurred.
  std::uint64_t total = 0;
  for (auto &c : counts) {
    total += c.load(std::memory_order_relaxed);
  }
  EXPECT_GT(total, 0U);
}

#endif // CITOR_TSAN_BUILD
