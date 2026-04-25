#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "citor/cancellation.h"
#include "citor/cpos/run_plex.h"
#include "citor/example_hints.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

using citor::Balance;
using citor::CancellationToken;
using citor::FrontierPlexHints;
using citor::Priority;
using citor::ThreadPool;

// Hint preset at TU scope (not in an anonymous namespace) so clang-tidy treats every
// static-constexpr member as a public field of a named type rather than an unused constant.
struct PlexTestHints {
  static constexpr Balance balance = Balance::StaticUniform;
  static constexpr Priority priority = Priority::Throughput;
  static constexpr double estimatedItemNs = 0.0;
  static constexpr double minTaskUs = 0.0;
  static constexpr std::size_t chunk = 0;
};

namespace {

// Visit every (phase, slot) pair exactly once, in stable phase-then-slot order. Verifies that
// `phaseFn` is invoked exactly once per pair and the phase epoch advances monotonically.
TEST(RunPlex, BasicPhaseOrdering) {
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

  pool.runPlex<PlexTestHints>(kPhases, kN,
                              [&](std::size_t phaseIdx, std::uint32_t slot, std::size_t lo,
                                  std::size_t hi, void * /*tlsArena*/) {
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

// Slot 0 must run on the producer thread, exactly once per phase. Verifies producer-as-slot-0
// semantics.
TEST(RunPlex, ProducerParticipatesAsSlot0) {
  ThreadPool pool(4);
  constexpr std::size_t kPhases = 5;
  constexpr std::size_t kN = 32;

  std::atomic<std::size_t> slot0VisitCount{0};

  pool.runPlex<PlexTestHints>(kPhases, kN,
                              [&](std::size_t /*phaseIdx*/, std::uint32_t slot, std::size_t /*lo*/,
                                  std::size_t /*hi*/, void * /*tlsArena*/) {
                                if (slot == 0U) {
                                  slot0VisitCount.fetch_add(1, std::memory_order_relaxed);
                                }
                              });

  EXPECT_EQ(slot0VisitCount.load(std::memory_order_relaxed), kPhases);
}

// Cancellation: when the token is stopped before the plex starts, no phases run after the stop is
// observed. The plex still rendezvous with every worker so no body is in flight on return.
TEST(RunPlex, CancellationMidPhase) {
  ThreadPool pool(4);
  constexpr std::size_t kPhases = 100;
  constexpr std::size_t kN = 32;

  CancellationToken tok = CancellationToken::makeOwned();
  std::atomic<std::size_t> phasesObserved{0};
  // Stop after the first phase completes (slot-0 hook fires inside phase 0 for slot 0).
  pool.runPlex<PlexTestHints>(
      kPhases, kN,
      [&](std::size_t /*phaseIdx*/, std::uint32_t slot, std::size_t /*lo*/, std::size_t /*hi*/,
          void * /*tlsArena*/) {
        if (slot == 0U) {
          phasesObserved.fetch_add(1, std::memory_order_relaxed);
          tok.request_stop();
        }
      },
      tok);

  // We expect at least one phase to run (the one that triggered stop). Subsequent phases are
  // cut short because the producer observes the stop at the next phase boundary.
  const std::size_t observed = phasesObserved.load(std::memory_order_relaxed);
  EXPECT_GE(observed, std::size_t{1});
  EXPECT_LT(observed, kPhases);
}

// Exception in phaseFn: the first thrown exception is rethrown by `runPlex` after rendezvous.
TEST(RunPlex, ExceptionInPhaseFnPropagates) {
  ThreadPool pool(4);
  constexpr std::size_t kPhases = 10;
  constexpr std::size_t kN = 32;

  EXPECT_THROW(
      pool.runPlex<PlexTestHints>(kPhases, kN,
                                  [](std::size_t phaseIdx, std::uint32_t slot, std::size_t /*lo*/,
                                     std::size_t /*hi*/, void * /*tlsArena*/) {
                                    if (phaseIdx == 3U && slot == 0U) {
                                      throw std::runtime_error("plex slot-0 fault");
                                    }
                                  }),
      std::runtime_error);
}

// CPO equivalence: calling via the member surface and via the CPO produces identical visit
// counts.
TEST(RunPlex, MemberCpoEquivalence) {
  ThreadPool pool(4);
  constexpr std::size_t kPhases = 8;
  constexpr std::size_t kN = 64;

  std::atomic<std::size_t> memberCalls{0};
  std::atomic<std::size_t> cpoCalls{0};

  pool.runPlex<PlexTestHints>(
      kPhases, kN,
      [&](std::size_t /*phaseIdx*/, std::uint32_t /*slot*/, std::size_t /*lo*/, std::size_t /*hi*/,
          void * /*tlsArena*/) { memberCalls.fetch_add(1, std::memory_order_relaxed); });

  citor::runPlex.template operator()<PlexTestHints>(
      pool, kPhases, kN,
      [&](std::size_t /*phaseIdx*/, std::uint32_t /*slot*/, std::size_t /*lo*/, std::size_t /*hi*/,
          void * /*tlsArena*/) { cpoCalls.fetch_add(1, std::memory_order_relaxed); });

  EXPECT_EQ(memberCalls.load(std::memory_order_relaxed), cpoCalls.load(std::memory_order_relaxed));
  EXPECT_EQ(memberCalls.load(std::memory_order_relaxed), kPhases * pool.participants());
}

// Pre-phase hook: the producer's `prePhase(p)` runs before publishing each phase. Verifies that
// state written in `prePhase` is visible to every worker's `phaseFn` for that phase.
TEST(RunPlex, PrePhaseStateVisibleToWorkers) {
  ThreadPool pool(4);
  constexpr std::size_t kPhases = 10;
  constexpr std::size_t kN = 32;

  // Per-phase `target` written by producer; workers read it during their phase work.
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

  pool.runPlex<PlexTestHints>(
      kPhases, kN,
      [&](std::size_t phaseIdx, std::uint32_t slot, std::size_t /*lo*/, std::size_t /*hi*/,
          void * /*tlsArena*/) {
        observed[phaseIdx][slot].store(currentTarget, std::memory_order_relaxed);
      },
      [&](std::size_t phaseIdx) {
        currentTarget = static_cast<std::int64_t>(phaseIdx * 1000ULL);
        perPhaseTarget[phaseIdx] = currentTarget;
      });

  for (std::size_t p = 0; p < kPhases; ++p) {
    for (std::size_t s = 0; s < pool.participants(); ++s) {
      EXPECT_EQ(observed[p][s].load(std::memory_order_relaxed), perPhaseTarget[p])
          << "phase=" << p << " slot=" << s;
    }
  }
}

// Worker exception during phase 0 must short-circuit the producer's phase loop: no later
// `prePhaseFn` and no later slot-0 `phaseFn` should run, even though the captured exception is
// rethrown only after every worker has rendezvoused.
TEST(RunPlex, WorkerExceptionStopsLaterPhases) {
  ThreadPool pool(4);
  const std::size_t participants = pool.participants();
  if (participants <= 1U) {
    GTEST_SKIP() << "single-participant pool runs inline; this test exercises the parallel path";
  }

  std::atomic<int> prePhaseCalls{0};
  std::atomic<int> slot0LatePhaseCalls{0};

  EXPECT_THROW(
      pool.runPlex<PlexTestHints>(
          /*nPhases=*/3, /*n=*/64,
          [&](std::size_t phase, std::uint32_t slot, std::size_t /*lo*/, std::size_t /*hi*/,
              void * /*tls*/) {
            if (slot == 1U && phase == 0U) {
              throw std::runtime_error("worker-throw");
            }
            if (slot == 0U && phase > 0U) {
              slot0LatePhaseCalls.fetch_add(1, std::memory_order_relaxed);
            }
          },
          [&](std::size_t /*phaseIdx*/) { prePhaseCalls.fetch_add(1, std::memory_order_relaxed); }),
      std::runtime_error);

  EXPECT_LE(prePhaseCalls.load(), 1) << "prePhaseFn ran for phases beyond the throwing one";
  EXPECT_EQ(slot0LatePhaseCalls.load(), 0)
      << "slot-0 phaseFn ran for phases > 0 after a worker threw";
}

// Single-participant inline path: nPhases iterations all run on the producer thread, slot 0 only.
TEST(RunPlex, InlineFallbackForSingleParticipant) {
  ThreadPool pool(1);
  constexpr std::size_t kPhases = 12;
  constexpr std::size_t kN = 8;

  std::atomic<std::size_t> calls{0};
  pool.runPlex<PlexTestHints>(kPhases, kN,
                              [&](std::size_t phaseIdx, std::uint32_t slot, std::size_t lo,
                                  std::size_t hi, void * /*tlsArena*/) {
                                EXPECT_EQ(slot, 0U);
                                EXPECT_EQ(lo, 0U);
                                EXPECT_EQ(hi, kN);
                                EXPECT_LT(phaseIdx, kPhases);
                                calls.fetch_add(1, std::memory_order_relaxed);
                              });
  EXPECT_EQ(calls.load(std::memory_order_relaxed), kPhases);
}

// Zero phases: returns immediately with no body invocations.
TEST(RunPlex, ZeroPhasesIsNoop) {
  ThreadPool pool(4);
  std::atomic<std::size_t> calls{0};
  pool.runPlex<PlexTestHints>(
      0, 32,
      [&](std::size_t /*phaseIdx*/, std::uint32_t /*slot*/, std::size_t /*lo*/, std::size_t /*hi*/,
          void * /*tlsArena*/) { calls.fetch_add(1, std::memory_order_relaxed); });
  EXPECT_EQ(calls.load(std::memory_order_relaxed), 0U);
}

// Slot range partition: the union of every slot's `[lo, hi)` covers `[0, n)` exactly once for
// each phase. Sums must be `n * kPhases` over all visited indices.
TEST(RunPlex, SlotRangesPartitionExactly) {
  ThreadPool pool(4);
  constexpr std::size_t kPhases = 3;
  constexpr std::size_t kN = 100;
  std::vector<std::atomic<std::uint32_t>> coverage(kN);
  for (auto &c : coverage) {
    c.store(0, std::memory_order_relaxed);
  }

  pool.runPlex<PlexTestHints>(kPhases, kN,
                              [&](std::size_t /*phaseIdx*/, std::uint32_t /*slot*/, std::size_t lo,
                                  std::size_t hi, void * /*tlsArena*/) {
                                for (std::size_t i = lo; i < hi; ++i) {
                                  coverage[i].fetch_add(1, std::memory_order_relaxed);
                                }
                              });

  for (std::size_t i = 0; i < kN; ++i) {
    EXPECT_EQ(coverage[i].load(std::memory_order_relaxed), kPhases) << "i=" << i;
  }
}

// FrontierPlexHints integration smoke test: runPlex template-instantiates with the named site hint.
TEST(RunPlex, PrimRelaxHintsInstantiates) {
  ThreadPool pool(4);
  std::atomic<std::size_t> calls{0};
  pool.runPlex<FrontierPlexHints>(
      4, 16,
      [&](std::size_t /*phaseIdx*/, std::uint32_t /*slot*/, std::size_t /*lo*/, std::size_t /*hi*/,
          void * /*tlsArena*/) { calls.fetch_add(1, std::memory_order_relaxed); });
  EXPECT_EQ(calls.load(std::memory_order_relaxed), 4 * pool.participants());
}

} // namespace
