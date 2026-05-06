#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "citor/cancellation.h"
#include "citor/cpos/run_plex.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::CancellationToken;
using citor::HintsDefaults;
using citor::ThreadPool;

// Cancellation: when the token is stopped before the plex starts, no phases run
// after the stop is observed. The plex still rendezvous with every worker so no
// body is in flight on return.
TEST(RunPlexCancellation, MidPhaseStopAbortsLaterPhasesWithoutInvokingBody) {
  ThreadPool pool(4);
  constexpr std::size_t kPhases = 100;
  constexpr std::size_t kN = 32;

  CancellationToken tok = CancellationToken::makeOwned();
  std::atomic<std::size_t> phasesObserved{0};
  // Stop after the first phase completes (slot-0 hook fires inside phase 0 for
  // slot 0).
  pool.runPlex<HintsDefaults>(
      kPhases, kN,
      [&](std::size_t /*phaseIdx*/, std::uint32_t slot, std::size_t /*lo*/,
          std::size_t /*hi*/) {
        if (slot == 0U) {
          phasesObserved.fetch_add(1, std::memory_order_relaxed);
          tok.request_stop();
        }
      },
      tok);

  // We expect at least one phase to run (the one that triggered stop).
  // Subsequent phases are cut short because the producer observes the stop at
  // the next phase boundary.
  const std::size_t observed = phasesObserved.load(std::memory_order_relaxed);
  EXPECT_GE(observed, std::size_t{1});
  EXPECT_LT(observed, kPhases);
}
