#pragma once

#include <cstddef>
#include <utility>

#include "citor/cancellation.h"

namespace citor {

/// Tag type identifying the `citor::parallelChain` customization point.
///
/// Passed as the first argument to every `tag_invoke` overload that implements `parallelChain` for
/// a given executor. Mirrors the `ParallelForTag` pattern so the extension surface is symmetric
/// with the rest of the parallel CPO family.
struct ParallelChainTag {};

namespace detail {

/// Function-object backing the `citor::parallelChain` customization point.
///
/// Dispatches to an unqualified `tag_invoke` call so ADL can reach overloads in the executor's
/// namespace. The function object itself is passed as the first argument so overloads can key on
/// the CPO identity in addition to the tag.
///
/// The `ChainHintsT` template parameter is a *type*, not a value: that lets the friend overload on
/// `ThreadPool` template on the same `ChainHintsT` and monomorphize identically to the
/// member-template call. Both surfaces ultimately route through the same engine; the CPO has zero
/// runtime hint dispatch cost.
struct ParallelChainFn {
  /// Forward to the executor's `tag_invoke` overload, supplying a default `ChainHintsT{}`.
  ///
  /// The chain hint type is supplied as an explicit template parameter at the call site (mirroring
  /// the member-template surface) so the executor's overload can specialize on it via `if
  /// constexpr` or a regular template parameter. The variadic stage pack flows through perfect
  /// forwarding so each stage's compile-time `BarrierKind` is preserved.
  ///
  /// ChainHintsT Chain hint type whose `static constexpr` members drive compile-time
  ///                     policy.
  /// Pool        Executor type.
  /// Stages      Variadic pack of `Stage<F, BarrierKind>` value types.
  /// pool   Executor instance.
  /// n      Row-range upper bound; partitioned across slots as
  ///                `[n*slot/P, n*(slot+1)/P)`.
  /// stages Stage pack invoked in submission order with the declared barrier between
  ///                consecutive stages.
  template <class ChainHintsT, class Pool, class... Stages>
  void operator()(Pool &pool, std::size_t n, Stages &&...stages) const {
    tag_invoke(*this, pool, n, ChainHintsT{}, CancellationToken{}, std::forward<Stages>(stages)...);
  }

  /// Forward with an explicit cancellation token.
  ///
  /// Available for callers that need to wire a token in alongside a chain hint type.
  template <class ChainHintsT, class Pool, class... Stages>
  void operator()(Pool &pool, std::size_t n, CancellationToken tok, Stages &&...stages) const {
    tag_invoke(*this, pool, n, ChainHintsT{}, std::move(tok), std::forward<Stages>(stages)...);
  }
};

} // namespace detail

/// Customization-point object for the multi-stage chain primitive.
///
/// Calling `parallelChain<ChainHintsT>(pool, n, stages...)` dispatches through unqualified
/// `tag_invoke`; the executor's overload runs each stage in submission order with the declared
/// post-stage barrier. The producer participates as slot 0 across every stage.
///
/// The chain primitive amortises the cost of fanning out a multi-stage multi-stage compute-fan-out pipeline:
/// one descriptor publish drives the entire chain, with per-stage rendezvous handled in user-space
/// spin-wait. Use when the inter-stage transition latency is on the same order as a single
/// `parallelFor` dispatch and the chain has at least two stages.
inline constexpr detail::ParallelChainFn parallelChain{};

} // namespace citor
