#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "citor/cpos/run_plex.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::HintsDefaults;
using citor::ThreadPool;

// CPO equivalence: calling via the member surface and via the CPO produces
// identical visit counts.
TEST(RunPlexCpoEquivalence,
     MemberCallAndCpoCallProduceIdenticalPerPhaseVisits) {
  ThreadPool pool(4);
  constexpr std::size_t kPhases = 8;
  constexpr std::size_t kN = 64;

  std::atomic<std::size_t> memberCalls{0};
  std::atomic<std::size_t> cpoCalls{0};

  pool.runPlex<HintsDefaults>(
      kPhases, kN,
      [&](std::size_t /*phaseIdx*/, std::uint32_t /*slot*/, std::size_t /*lo*/,
          std::size_t /*hi*/) {
        memberCalls.fetch_add(1, std::memory_order_relaxed);
      });

  citor::runPlex.template operator()<HintsDefaults>(
      pool, kPhases, kN,
      [&](std::size_t /*phaseIdx*/, std::uint32_t /*slot*/, std::size_t /*lo*/,
          std::size_t /*hi*/) {
        cpoCalls.fetch_add(1, std::memory_order_relaxed);
      });

  EXPECT_EQ(memberCalls.load(std::memory_order_relaxed),
            cpoCalls.load(std::memory_order_relaxed));
  EXPECT_EQ(memberCalls.load(std::memory_order_relaxed),
            kPhases * pool.participants());
}
