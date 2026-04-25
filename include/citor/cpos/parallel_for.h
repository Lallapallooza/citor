#pragma once

#include <cstddef>
#include <utility>

#include "citor/cancellation.h"

namespace citor {

/// Tag type identifying the `citor::parallelFor` customization point.
///
/// Passed as the first argument to every `tag_invoke` overload that implements `parallelFor` for a
/// given executor. Mirrors the tag-based dispatch pattern so the
/// extension surface is symmetric with the rest of the codebase.
struct ParallelForTag {};

namespace detail {

/// Function-object backing the `citor::parallelFor` customization point.
///
/// Dispatches to an unqualified `tag_invoke` call so ADL can reach overloads in the executor's
/// namespace. The function object itself is passed as the first argument so overloads can key on
/// the CPO identity in addition to the tag.
///
/// The `HintsT` template parameter is a *type*, not a value: that lets the friend overload on
/// `ThreadPool` template on the same `HintsT` and monomorphize identically to the member-template
/// call. Both surfaces ultimately route through the same engine; the CPO has zero runtime hint
/// dispatch cost.
struct ParallelForFn {
  /// Forward to the executor's `tag_invoke` overload, supplying a default `HintsT{}` value.
  ///
  /// The hint type is supplied as an explicit template parameter at the call site (mirroring the
  /// member-template surface) so the executor's overload can specialize on it via `if constexpr`
  /// or a regular template parameter.
  ///
  /// HintsT Hint type whose `static constexpr` members drive compile-time policy.
  /// Pool   Executor type.
  /// F      Callable type invoked once per block as `F(std::size_t lo, std::size_t hi)`.
  /// pool   Executor instance.
  /// first  Inclusive lower bound of the iteration range.
  /// last   Exclusive upper bound of the iteration range.
  /// fn     Callable invoked over each block.
  /// tok    Cancellation token observed at chunk boundaries.
  template <class HintsT, class Pool, class F>
  void operator()(Pool &pool, std::size_t first, std::size_t last, F &&fn,
                  CancellationToken tok = CancellationToken{}) const {
    tag_invoke(*this, pool, first, last, std::forward<F>(fn), HintsT{}, std::move(tok));
  }
};

} // namespace detail

/// Customization-point object for parallel iteration over a `[first, last)` range.
///
/// Calling `parallelFor<HintsT>(pool, first, last, fn)` dispatches through unqualified
/// `tag_invoke`; the executor's overload runs the iteration synchronously and returns once every
/// block has completed. The hint type carries the compile-time policy (balance, partition, chunk
/// grain, inline-fallback parameters) so every overload can specialize without runtime branching.
inline constexpr detail::ParallelForFn parallelFor{};

} // namespace citor
