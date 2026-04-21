#pragma once

#include <utility>

#include "citor/cancellation.h"

namespace citor {

/// Tag type identifying the `citor::submitDetached` customization point.
///
/// Passed as the first argument to every `tag_invoke` overload that implements `submitDetached` for
/// a given executor. Mirrors the `ForkJoinTag` pattern so the extension surface is symmetric
/// with the rest of the parallel CPO family.
struct SubmitDetachedTag {};

namespace detail {

/// Function-object backing the `citor::submitDetached` customization point.
///
/// Dispatches to an unqualified `tag_invoke` call so ADL can reach overloads in the executor's
/// namespace. The function object itself is passed as the first argument so overloads can key on
/// the CPO identity in addition to the tag.
///
/// The `HintsT` template parameter is a *type*, not a value: that lets the friend overload on
/// `ThreadPool` template on the same `HintsT` and monomorphize identically to the member-template
/// call. The hint type is reserved for future routing decisions (priority class, affinity), but is
/// unused on the current shape since detached submission has no partition / chunk schedule.
struct SubmitDetachedFn {
  /// Forward to the executor's `tag_invoke` overload, supplying a default `HintsT{}` value.
  ///
  /// The hint type is supplied as an explicit template parameter at the call site (mirroring the
  /// member-template surface) so the executor's overload can specialize on it via `if constexpr` or
  /// a regular template parameter.
  ///
  /// HintsT Hint type whose `static constexpr` members drive compile-time policy.
  /// Pool   Executor type.
  /// TaskFn Task callable, invocable as `void(void)`.
  /// pool   Executor instance.
  /// fn     Task body the executor runs without joining.
  /// tok    Cancellation token observed cooperatively by the body.
  template <class HintsT, class Pool, class TaskFn>
  void operator()(Pool &pool, TaskFn &&fn, CancellationToken tok = {}) const {
    tag_invoke(*this, pool, HintsT{}, std::forward<TaskFn>(fn), std::move(tok));
  }
};

} // namespace detail

/// Customization-point object for fire-and-forget detached submission.
///
/// Calling `submitDetached<HintsT>(pool, fn, tok)` dispatches through unqualified `tag_invoke`; the
/// executor's overload accepts ownership of the task, runs it asynchronously, and returns
/// immediately. The pool's destructor blocks until every detached task has completed; until then,
/// the pool's lifetime extends every in-flight body.
///
/// Cancellation: the supplied `CancellationToken` is observed cooperatively by the body (the
/// executor does not preempt). A pre-cancelled token short-circuits the body before any work runs.
///
/// Exception handling: an exception escaping a detached body is captured into a per-pool slot
/// (latched on first throw) and surfaced via `ThreadPool::lastDetachedException()`. The pool does
/// not call `std::terminate` on a detached throw, and subsequent throws drop on the floor.
inline constexpr detail::SubmitDetachedFn submitDetached{};

} // namespace citor
