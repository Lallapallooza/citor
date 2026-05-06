#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "citor/cpos/run_plex.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::HintsDefaults;
using citor::ThreadPool;

// Single-participant inline path: nPhases iterations all run on the producer
// thread, slot 0 only.
TEST(RunPlexInlineFallback,
     SingleParticipantPoolExecutesEveryPhaseInlineOnSlot0) {
  ThreadPool pool(1);
  constexpr std::size_t kPhases = 12;
  constexpr std::size_t kN = 8;

  std::atomic<std::size_t> calls{0};
  pool.runPlex<HintsDefaults>(kPhases, kN,
                              [&](std::size_t phaseIdx, std::uint32_t slot,
                                  std::size_t lo, std::size_t hi) {
                                EXPECT_EQ(slot, 0U);
                                EXPECT_EQ(lo, 0U);
                                EXPECT_EQ(hi, kN);
                                EXPECT_LT(phaseIdx, kPhases);
                                calls.fetch_add(1, std::memory_order_relaxed);
                              });
  EXPECT_EQ(calls.load(std::memory_order_relaxed), kPhases);
}
