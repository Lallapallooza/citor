#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <unordered_set>

#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::HintsDefaults;
using citor::ThreadPool;

// `runPlex` keeps the same K-worker fleet live across every phase under one
// dispatch. The contract: for K participants and N phases, slot s ran on
// the same OS thread for every phase. A regression that lowered runPlex
// to N back-to-back parallelFor calls (or any path that lets workers park
// between phases) would break the persistence assertion.
TEST(RunPlexDistribution, EverySlotKeepsTheSameThreadAcrossPhases) {
  constexpr std::uint32_t kParticipants = 4U;
  constexpr std::size_t kPhases = 4;
  constexpr std::size_t kN = 1u << 12;

  ThreadPool pool(kParticipants);

  std::array<std::array<std::atomic<std::uintptr_t>, kParticipants>, kPhases>
      observed{};
  for (std::size_t ph = 0; ph < kPhases; ++ph) {
    for (std::size_t p = 0; p < kParticipants; ++p) {
      observed[ph][p].store(0u, std::memory_order_relaxed);
    }
  }

  pool.runPlex<HintsDefaults>(
      kPhases, kN,
      [&](std::size_t phaseIdx, std::uint32_t slot, std::size_t /*lo*/,
          std::size_t /*hi*/) {
        const auto hash =
            std::hash<std::thread::id>{}(std::this_thread::get_id());
        observed[phaseIdx][slot].store(static_cast<std::uintptr_t>(hash),
                                       std::memory_order_relaxed);
      });

  // Distinct-thread check: every participant slot ran at least one phase.
  std::unordered_set<std::uintptr_t> firstPhaseIds;
  for (std::size_t p = 0; p < kParticipants; ++p) {
    firstPhaseIds.insert(observed[0][p].load(std::memory_order_relaxed));
  }
  EXPECT_EQ(firstPhaseIds.size(), kParticipants);

  // Persistence check: slot s keeps the same thread for every phase.
  for (std::size_t p = 0; p < kParticipants; ++p) {
    const auto first = observed[0][p].load(std::memory_order_relaxed);
    for (std::size_t ph = 1; ph < kPhases; ++ph) {
      EXPECT_EQ(observed[ph][p].load(std::memory_order_relaxed), first)
          << "slot " << p << " phase " << ph << " ran on a different thread";
    }
  }
}
