#pragma once

#include <cstddef>
#include <utility>

#include "citor/cancellation.h"

namespace citor {

/// Tag type identifying the `citor::runPlex` customization point.
///
/// Passed as the first argument to every `tag_invoke` overload that implements
/// `runPlex` for a given executor. Mirrors the `ParallelForTag` pattern so the
/// extension surface is symmetric with the rest of the parallel CPO family.
struct RunPlexTag {};

namespace detail {

/// Function-object backing the `citor::runPlex` customization point.
///
/// Dispatches to an unqualified `tag_invoke` call so ADL can reach overloads in
/// the executor's namespace. The function object itself is passed as the first
/// argument so overloads can key on the CPO identity in addition to the tag.
///
/// The `HintsT` template parameter is a *type*, not a value: that lets the
/// friend overload on `ThreadPool` template on the same `HintsT` and
/// monomorphize identically to the member-template call. Both surfaces
/// ultimately route through the same engine; the CPO has zero runtime hint
/// dispatch cost.
struct RunPlexFn {
  /// Forward to the executor's `tag_invoke` overload, supplying a default
  /// `HintsT{}` value.
  ///
  /// The hint type is supplied as an explicit template parameter at the call
  /// site (mirroring the member-template surface) so the executor's overload
  /// can specialize on it via `if constexpr` or a regular template parameter.
  ///
  /// HintsT  Hint type whose `static constexpr` members drive compile-time
  /// policy. Pool    Executor type. Phase   Phase callable: `void(std::size_t
  /// phaseIdx, std::uint32_t slot,
  ///                                       std::size_t lo, std::size_t hi)`.
  /// pool     Executor instance.
  /// nPhases  Number of phases to run; `0` is a no-op.
  /// n        Row-range upper bound; partitioned across slots as
  ///                  `[n*slot/P, n*(slot+1)/P)`.
  /// phaseFn  Callable invoked once per `(phase, slot)` pair.
  /// tok      Cancellation token observed at phase boundaries.
  template <class HintsT, class Pool, class Phase>
  void operator()(Pool &pool, std::size_t nPhases, std::size_t n,
                  Phase &&phaseFn,
                  CancellationToken tok = CancellationToken{}) const {
    tag_invoke(*this, pool, nPhases, n, std::forward<Phase>(phaseFn), HintsT{},
               std::move(tok));
  }
};

} // namespace detail

/// Customization-point object for the persistent-worker plex pattern.
///
/// Calling `runPlex<HintsT>(pool, nPhases, n, phaseFn)` dispatches through
/// unqualified `tag_invoke`; the executor's overload runs `nPhases` phases in
/// order. Each phase invokes `phaseFn(phaseIdx, slot, lo, hi)` once per
/// participant slot, where `(lo, hi)` is the slot's contiguous range over `[0,
/// n)`. The producer participates as slot 0.
///
/// The plex stays in user-space spin-wait between phases (no futex round-trip
/// per phase), so inter-phase transition latency is dominated by
/// cache-coherency on the per-worker `done` flags. Use only when the user has a
/// known number of phases and inter-phase latency matters more than overall
/// work; for one-shot fan-outs `parallelFor` is cheaper.
inline constexpr detail::RunPlexFn runPlex{};

} // namespace citor
