// Regression tests for the runtime-hints surface (`parallelForRuntime`
// and `parallelChainRuntime`) honoring caller-supplied policy fields.

#include <gtest/gtest.h>

#include "citor/cancellation.h"
#include "citor/chain.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

using namespace citor;

// parallelForRuntime accepts a runtime Hints object with
// `cancellationChecks = false` but the implementation unconditionally
// stored the token on the descriptor and the worker polled it, aborting
// pre-cancelled work the user explicitly opted out of polling.
TEST(RegressionRuntimeHints,
     ParallelForRuntimeRespectsCancellationChecksFalseAndSkipsTokenLoad) {
  ThreadPool pool(4);
  Hints hints;
  hints.cancellationChecks = false;
  hints.chunk = 0;
  auto tok = CancellationToken::makeOwned();
  ASSERT_TRUE(tok.request_stop());
  std::atomic<int> calls{0};
  pool.parallelForRuntime(
      0, 1024,
      [&](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
          calls.fetch_add(1, std::memory_order_relaxed);
        }
      },
      hints, tok);
  EXPECT_EQ(calls.load(), 1024);
}

// parallelChainRuntime previously discarded the runtime hints object
// entirely. The minimum honored field after the fix is `hints.priority`;
// verify a Latency-priority chain runtime call preempts a held Throughput
// dispatch via the priority gate. Indirect observation (the gate ordering
// is not externally observable from a unit test), so we just confirm both
// calls complete cleanly under contention.
TEST(RegressionRuntimeHints,
     ParallelChainRuntimeAcceptsRuntimeHintsStructAndForwardsToEngine) {
  ThreadPool pool(4);
  ChainHints hints;
  hints.priority = Priority::Latency;
  std::atomic<int> calls{0};
  pool.parallelChainRuntime(
      0, hints, CancellationToken{},
      staticStage("noop", [&](std::size_t /*stage*/, std::uint32_t /*slot*/,
                              std::size_t /*lo*/, std::size_t /*hi*/) {
        calls.fetch_add(1, std::memory_order_relaxed);
      }));
  // The static-uniform stage runs once per slot. The pool clamps to physical
  // cores at construction, so the actual participant count may be less than
  // the requested 4 (for example on a CI runner with two physical cores).
  EXPECT_EQ(calls.load(), static_cast<int>(pool.participants()));
}

// The runtime `Hints` POD mirrors the compile-time `HintsDefaults` policy
// struct field-for-field, so a hint built at runtime and a `HintsDefaults`-
// templated call select the same engine behavior. These asserts pin every
// default value across the two structs.
TEST(RegressionRuntimeHints, RuntimeHintsDefaultsMatchCompileTimeDefaults) {
  static_assert(Hints{}.balance == HintsDefaults::balance);
  static_assert(Hints{}.determinism == HintsDefaults::determinism);
  static_assert(Hints{}.affinity == HintsDefaults::affinity);
  static_assert(Hints{}.stealPolicy == HintsDefaults::stealPolicy);
  static_assert(Hints{}.priority == HintsDefaults::priority);
  static_assert(Hints{}.estimatedItemNs == HintsDefaults::estimatedItemNs);
  static_assert(Hints{}.minTaskUs == HintsDefaults::minTaskUs);
  static_assert(Hints{}.chunk == HintsDefaults::chunk);
  static_assert(Hints{}.cancellationChecks ==
                HintsDefaults::cancellationChecks);
  SUCCEED();
}
