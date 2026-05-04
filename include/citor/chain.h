#pragma once

#include <utility>

#include "citor/hints.h"

namespace citor {

/// Construct a `Stage` with the `BarrierKind::None` post-stage barrier.
///
/// The "static" naming reflects that a worker proceeds to the next stage without synchronizing on
/// any shared state -- each worker increments its local stage epoch and runs the next body. Use
/// for stages whose downstream consumer reads strictly per-worker state (no cross-worker reads).
///
/// F Deduced callable type.
/// name Diagnostic identifier surfaced through trace tooling; kept on the stage value for
///              future plumbing (the pool does not currently consume it).
/// fn   Stage body invoked as `fn(stageIdx, slot, lo, hi)`.
/// A `Stage<decay_t<F>, BarrierKind::None>` carrying the callable.
template <class F>
[[nodiscard]] constexpr auto staticStage([[maybe_unused]] const char *name, F &&fn) noexcept(
    noexcept(Stage<std::decay_t<F>, BarrierKind::None>{std::forward<F>(fn)}))
    -> Stage<std::decay_t<F>, BarrierKind::None> {
  return Stage<std::decay_t<F>, BarrierKind::None>{std::forward<F>(fn)};
}

/// Construct a `Stage` with the `BarrierKind::Global` post-stage barrier.
///
/// Every worker rendezvouses on the producer-driven stage epoch before any worker may begin the
/// next stage. Use when the next stage reads state that any upstream worker may have written.
///
/// F Deduced callable type.
/// name Diagnostic identifier surfaced through trace tooling.
/// fn   Stage body invoked as `fn(stageIdx, slot, lo, hi)`.
/// A `Stage<decay_t<F>, BarrierKind::Global>` carrying the callable.
template <class F>
[[nodiscard]] constexpr auto globalStage([[maybe_unused]] const char *name, F &&fn) noexcept(
    noexcept(Stage<std::decay_t<F>, BarrierKind::Global>{std::forward<F>(fn)}))
    -> Stage<std::decay_t<F>, BarrierKind::Global> {
  return Stage<std::decay_t<F>, BarrierKind::Global>{std::forward<F>(fn)};
}

/// Construct a `Stage` with the `BarrierKind::DeterministicReduce` post-stage
///        barrier.
///
/// Behaves as a global rendezvous in v1; the deterministic chunk-id pairwise tree reduction is
/// the user's own concern inside the stage body (callers run a `parallelReduce` with the same
/// fixed-block shape from inside the stage). The chain primitive guarantees the global sync; the
/// reduction is the call site's responsibility.
///
/// F Deduced callable type.
/// name Diagnostic identifier surfaced through trace tooling.
/// fn   Stage body invoked as `fn(stageIdx, slot, lo, hi)`.
/// A `Stage<decay_t<F>, BarrierKind::DeterministicReduce>` carrying the callable.
template <class F>
[[nodiscard]] constexpr auto reduceStage([[maybe_unused]] const char *name, F &&fn) noexcept(
    noexcept(Stage<std::decay_t<F>, BarrierKind::DeterministicReduce>{std::forward<F>(fn)}))
    -> Stage<std::decay_t<F>, BarrierKind::DeterministicReduce> {
  return Stage<std::decay_t<F>, BarrierKind::DeterministicReduce>{std::forward<F>(fn)};
}

/// Construct a `Stage` with the `BarrierKind::ProducerSerial` post-stage barrier.
///
/// The producer (slot 0) runs the stage body alone; non-producer workers spin on the
/// producer-done flag. Use for stages whose body is inherently serial (centroid update divide,
/// summary stats publish) that should not be replicated across slots.
///
/// F Deduced callable type.
/// name Diagnostic identifier surfaced through trace tooling.
/// fn   Stage body invoked as `fn(stageIdx, slot, lo, hi)`.
/// A `Stage<decay_t<F>, BarrierKind::ProducerSerial>` carrying the callable.
template <class F>
[[nodiscard]] constexpr auto serialStage([[maybe_unused]] const char *name, F &&fn) noexcept(
    noexcept(Stage<std::decay_t<F>, BarrierKind::ProducerSerial>{std::forward<F>(fn)}))
    -> Stage<std::decay_t<F>, BarrierKind::ProducerSerial> {
  return Stage<std::decay_t<F>, BarrierKind::ProducerSerial>{std::forward<F>(fn)};
}

} // namespace citor
