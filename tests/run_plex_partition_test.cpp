#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "citor/cpos/run_plex.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::HintsDefaults;
using citor::ThreadPool;

// Slot range partition: the union of every slot's `[lo, hi)` covers `[0, n)`
// exactly once for each phase. Sums must be `n * kPhases` over all visited
// indices.
TEST(RunPlexPartition, UnionOfSlotRangesCoversFullRangeExactlyOncePerPhase) {
  ThreadPool pool(4);
  constexpr std::size_t kPhases = 3;
  constexpr std::size_t kN = 100;
  std::vector<std::atomic<std::uint32_t>> coverage(kN);
  for (auto &c : coverage) {
    c.store(0, std::memory_order_relaxed);
  }

  pool.runPlex<HintsDefaults>(
      kPhases, kN,
      [&](std::size_t /*phaseIdx*/, std::uint32_t /*slot*/, std::size_t lo,
          std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
          coverage[i].fetch_add(1, std::memory_order_relaxed);
        }
      });

  for (std::size_t i = 0; i < kN; ++i) {
    EXPECT_EQ(coverage[i].load(std::memory_order_relaxed), kPhases)
        << "i=" << i;
  }
}
