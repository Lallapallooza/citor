#pragma once

#include <utility>

#include "citor/cancellation.h"

namespace citor {

/// Tag type identifying the `citor::forkJoin` customization point.
///
/// Passed as the first argument to every `tag_invoke` overload that implements
/// `forkJoin` for a given executor. Mirrors the `ParallelForTag` pattern so the
/// extension surface is symmetric with the rest of the parallel CPO family.
struct ForkJoinTag {};

namespace detail {

/// Function-object backing the `citor::forkJoin` customization point.
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
///
/// Recursive tasks call back into the customization point from worker context;
/// each task receives a `ForkJoinScope` reference (defined by the executor) it
/// uses to spawn children. The scope abstraction keeps the CPO surface
/// decoupled from the engine's task-descriptor encoding.
struct ForkJoinFn {
  /// Forward to the executor's `tag_invoke` overload, supplying a default
  /// `HintsT{}` value.
  ///
  /// The hint type is supplied as an explicit template parameter at the call
  /// site (mirroring the member-template surface) so the executor's overload
  /// can specialize on it via `if constexpr` or a regular template parameter.
  ///
  /// HintsT  Hint type whose `static constexpr` members drive compile-time
  /// policy. Pool    Executor type. TaskFns Variadic pack of task callables,
  /// each invocable as `void(void)` or
  ///                 `void(ForkJoinScope&)` per the executor's contract.
  /// pool    Executor instance.
  /// tok     Cancellation token observed at task-boundary chunks.
  /// fns     Variadic pack of root tasks.
  template <class HintsT, class Pool, class... TaskFns>
  void operator()(Pool &pool, CancellationToken tok, TaskFns &&...fns) const {
    tag_invoke(*this, pool, std::move(tok), HintsT{},
               std::forward<TaskFns>(fns)...);
  }
};

} // namespace detail

/// Customization-point object for recursive fork-join with work-stealing.
///
/// Calling `forkJoin<HintsT>(pool, tok, fns...)` dispatches through unqualified
/// `tag_invoke`; the executor's overload submits each task to a per-worker
/// work-stealing deque and the call returns once every task (and every
/// recursive child spawned during their bodies) has retired. The producer
/// participates as slot 0 and steals from other workers' deques when its own
/// drains.
///
/// Cancellation: the supplied `CancellationToken` is observed at task-boundary
/// chunks. A stopped token causes participants to stop spawning fresh recursive
/// children and to drain the outstanding task count without admitting more
/// work; the join still rendezvous with every worker before returning.
///
/// Exception handling: if any task body throws, the first exception is captured
/// and rethrown from the producer after every outstanding task has retired.
/// Subsequent throws drop. The remaining tasks are cancelled (the cancellation
/// flag is set as part of the throw response) so the join does not block on
/// quiescence.
///
/// The `HintsT` policy carries the `Affinity` choice that the engine uses to
/// bias victim selection: `Affinity::CcdLocal` prefers same-CCD victims,
/// mirroring the topology's shared-L3 grouping. Other affinity values fall back
/// to a uniform xorshift random victim probe.
inline constexpr detail::ForkJoinFn forkJoin{};

} // namespace citor
