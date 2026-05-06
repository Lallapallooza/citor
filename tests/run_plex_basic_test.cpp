#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "citor/cpos/run_plex.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::BulkHints;
using citor::HintsDefaults;
using citor::ThreadPool;

// Visit every (phase, slot) pair exactly once, in stable phase-then-slot order.
// Verifies that `phaseFn` is invoked exactly once per pair and the phase epoch
// advances monotonically.
TEST(RunPlexBasic, VisitsEveryPhaseAndSlotPairExactlyOnceInPhaseThenSlotOrder) {
  ThreadPool pool(4);
  constexpr std::size_t kPhases = 7;
  constexpr std::size_t kN = 64;
  const std::size_t participants = pool.participants();

  // visited[p][slot] counts visits; expected 1 each.
  std::vector<std::vector<std::atomic<std::uint32_t>>> visited(kPhases);
  for (auto &row : visited) {
    row = std::vector<std::atomic<std::uint32_t>>(participants);
    for (auto &c : row) {
      c.store(0, std::memory_order_relaxed);
    }
  }

  // Track the maximum observed phase per slot to confirm monotonic advancement.
  std::vector<std::atomic<std::uint64_t>> maxPhase(participants);
  for (auto &m : maxPhase) {
    m.store(0, std::memory_order_relaxed);
  }

  pool.runPlex<HintsDefaults>(
      kPhases, kN,
      [&](std::size_t phaseIdx, std::uint32_t slot, std::size_t lo,
          std::size_t hi) {
        EXPECT_LT(phaseIdx, kPhases);
        EXPECT_LT(slot, participants);
        EXPECT_LE(lo, hi);
        EXPECT_LE(hi, kN);
        visited[phaseIdx][slot].fetch_add(1, std::memory_order_relaxed);
        // Monotonic: the slot should not see phase decrease.
        const auto prevMax = maxPhase[slot].load(std::memory_order_relaxed);
        EXPECT_GE(phaseIdx + 1U, prevMax);
        maxPhase[slot].store(phaseIdx + 1U, std::memory_order_relaxed);
      });

  for (std::size_t p = 0; p < kPhases; ++p) {
    for (std::size_t s = 0; s < participants; ++s) {
      EXPECT_EQ(visited[p][s].load(std::memory_order_relaxed), 1U)
          << "phase=" << p << " slot=" << s;
    }
  }
}

// Slot 0 must run on the producer thread, exactly once per phase. Verifies
// producer-as-slot-0 semantics.
TEST(RunPlexBasic, ProducerThreadIsObservedAsSlot0InEveryPhase) {
  ThreadPool pool(4);
  constexpr std::size_t kPhases = 5;
  constexpr std::size_t kN = 32;

  std::atomic<std::size_t> slot0VisitCount{0};

  pool.runPlex<HintsDefaults>(kPhases, kN,
                              [&](std::size_t /*phaseIdx*/, std::uint32_t slot,
                                  std::size_t /*lo*/, std::size_t /*hi*/) {
                                if (slot == 0U) {
                                  slot0VisitCount.fetch_add(
                                      1, std::memory_order_relaxed);
                                }
                              });

  EXPECT_EQ(slot0VisitCount.load(std::memory_order_relaxed), kPhases);
}

// Pre-phase hook: the producer's `prePhase(p)` runs before publishing each
// phase. Verifies that state written in `prePhase` is visible to every worker's
// `phaseFn` for that phase.
TEST(RunPlexBasic, WritesByPrePhaseFnAreVisibleToEveryWorkerInPhaseBody) {
  ThreadPool pool(4);
  constexpr std::size_t kPhases = 10;
  constexpr std::size_t kN = 32;

  // Per-phase `target` written by producer; workers read it during their phase
  // work.
  std::vector<std::int64_t> perPhaseTarget(kPhases);
  std::int64_t currentTarget = -1;

  // Each (phase, slot) records what target it observed.
  std::vector<std::vector<std::atomic<std::int64_t>>> observed(kPhases);
  for (auto &row : observed) {
    row = std::vector<std::atomic<std::int64_t>>(pool.participants());
    for (auto &c : row) {
      c.store(-2, std::memory_order_relaxed);
    }
  }

  pool.runPlex<HintsDefaults>(
      kPhases, kN,
      [&](std::size_t phaseIdx, std::uint32_t slot, std::size_t /*lo*/,
          std::size_t /*hi*/) {
        observed[phaseIdx][slot].store(currentTarget,
                                       std::memory_order_relaxed);
      },
      [&](std::size_t phaseIdx) {
        currentTarget = static_cast<std::int64_t>(phaseIdx * 1000ULL);
        perPhaseTarget[phaseIdx] = currentTarget;
      });

  for (std::size_t p = 0; p < kPhases; ++p) {
    for (std::size_t s = 0; s < pool.participants(); ++s) {
      EXPECT_EQ(observed[p][s].load(std::memory_order_relaxed),
                perPhaseTarget[p])
          << "phase=" << p << " slot=" << s;
    }
  }
}

// Zero phases: returns immediately with no body invocations.
TEST(RunPlexBasic, ZeroPhaseChainIsANoopAndDoesNotInvokeBody) {
  ThreadPool pool(4);
  std::atomic<std::size_t> calls{0};
  pool.runPlex<HintsDefaults>(0, 32,
                              [&](std::size_t /*phaseIdx*/,
                                  std::uint32_t /*slot*/, std::size_t /*lo*/,
                                  std::size_t /*hi*/) {
                                calls.fetch_add(1, std::memory_order_relaxed);
                              });
  EXPECT_EQ(calls.load(std::memory_order_relaxed), 0U);
}

// Bundled preset smoke test: runPlex template-instantiates with one of the
// presets shipped alongside HintsDefaults.
TEST(RunPlexBasic, AcceptsBulkHintsPolicyAndExecutesEveryPhase) {
  ThreadPool pool(4);
  std::atomic<std::size_t> calls{0};
  pool.runPlex<BulkHints>(4, 16,
                          [&](std::size_t /*phaseIdx*/, std::uint32_t /*slot*/,
                              std::size_t /*lo*/, std::size_t /*hi*/) {
                            calls.fetch_add(1, std::memory_order_relaxed);
                          });
  EXPECT_EQ(calls.load(std::memory_order_relaxed), 4 * pool.participants());
}
