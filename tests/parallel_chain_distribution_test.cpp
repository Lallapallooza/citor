#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <unordered_set>

#include "citor/chain.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::ChainHintsDefaults;
using citor::staticStage;
using citor::ThreadPool;

// Chain's persistent-workers contract: across N stages, each slot s is
// served by the same OS thread for every stage. A regression that
// implemented chain as N back-to-back parallelFor calls with worker
// respawn would break the persistence assertion.
TEST(ParallelChainDistribution, EverySlotKeepsTheSameThreadAcrossStages) {
  constexpr std::size_t kRequestedParticipants = 4;
  constexpr std::size_t kStages = 3;
  constexpr std::size_t kN = 1u << 12;

  ThreadPool pool(kRequestedParticipants);
  const std::size_t participants = pool.participants();
  if (participants < 2U) {
    GTEST_SKIP() << "pool has " << participants << " participant(s)";
  }

  std::array<std::array<std::atomic<std::uintptr_t>, kRequestedParticipants>,
             kStages>
      observed{};
  for (std::size_t s = 0; s < kStages; ++s) {
    for (std::size_t p = 0; p < kRequestedParticipants; ++p) {
      observed[s][p].store(0u, std::memory_order_relaxed);
    }
  }
  const auto record = [&](std::size_t stageIdx, std::uint32_t slot,
                          std::size_t /*lo*/, std::size_t /*hi*/) {
    const auto hash = std::hash<std::thread::id>{}(std::this_thread::get_id());
    observed[stageIdx][slot].store(static_cast<std::uintptr_t>(hash),
                                   std::memory_order_relaxed);
  };

  pool.parallelChain<ChainHintsDefaults>(kN, staticStage("a", record),
                                         staticStage("b", record),
                                         staticStage("c", record));

  // Distinct-thread check: every participant slot ran at least one stage.
  std::unordered_set<std::uintptr_t> firstStageIds;
  for (std::size_t p = 0; p < participants; ++p) {
    firstStageIds.insert(observed[0][p].load(std::memory_order_relaxed));
  }
  EXPECT_EQ(firstStageIds.size(), participants);

  // Persistence check: slot s keeps the same thread for every stage.
  for (std::size_t p = 0; p < participants; ++p) {
    const auto first = observed[0][p].load(std::memory_order_relaxed);
    for (std::size_t s = 1; s < kStages; ++s) {
      EXPECT_EQ(observed[s][p].load(std::memory_order_relaxed), first)
          << "slot " << p << " stage " << s << " ran on a different thread";
    }
  }
}
