#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include "citor/cpos/run_plex.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::HintsDefaults;
using citor::ThreadPool;

// Exception in phaseFn: the first thrown exception is rethrown by `runPlex`
// after rendezvous.
TEST(RunPlexExceptions, PhaseFnExceptionRethrownAtJoin) {
  ThreadPool pool(4);
  constexpr std::size_t kPhases = 10;
  constexpr std::size_t kN = 32;

  EXPECT_THROW(pool.runPlex<HintsDefaults>(
                   kPhases, kN,
                   [](std::size_t phaseIdx, std::uint32_t slot,
                      std::size_t /*lo*/, std::size_t /*hi*/) {
                     if (phaseIdx == 3U && slot == 0U) {
                       throw std::runtime_error("plex slot-0 fault");
                     }
                   }),
               std::runtime_error);
}

// Worker exception during phase 0 must short-circuit the producer's phase loop:
// no later `prePhaseFn` and no later slot-0 `phaseFn` should run, even though
// the captured exception is rethrown only after every worker has rendezvoused.
TEST(RunPlexExceptions, WorkerBodyExceptionPreventsLaterPhasesFromExecuting) {
  ThreadPool pool(4);
  const std::size_t participants = pool.participants();
  if (participants <= 1U) {
    GTEST_SKIP() << "single-participant pool runs inline; this test exercises "
                    "the parallel path";
  }

  std::atomic<int> prePhaseCalls{0};
  std::atomic<int> slot0LatePhaseCalls{0};

  EXPECT_THROW(pool.runPlex<HintsDefaults>(
                   /*nPhases=*/3, /*n=*/64,
                   [&](std::size_t phase, std::uint32_t slot,
                       std::size_t /*lo*/, std::size_t /*hi*/) {
                     if (slot == 1U && phase == 0U) {
                       throw std::runtime_error("worker-throw");
                     }
                     if (slot == 0U && phase > 0U) {
                       slot0LatePhaseCalls.fetch_add(1,
                                                     std::memory_order_relaxed);
                     }
                   },
                   [&](std::size_t /*phaseIdx*/) {
                     prePhaseCalls.fetch_add(1, std::memory_order_relaxed);
                   }),
               std::runtime_error);

  EXPECT_LE(prePhaseCalls.load(), 1)
      << "prePhaseFn ran for phases beyond the throwing one";
  EXPECT_EQ(slot0LatePhaseCalls.load(), 0)
      << "slot-0 phaseFn ran for phases > 0 after a worker threw";
}
