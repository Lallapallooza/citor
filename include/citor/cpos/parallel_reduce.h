#pragma once

#include <cstddef>
#include <utility>

#include "citor/cancellation.h"

namespace citor {

/// Tag type identifying the `citor::parallelReduce` customization point.
///
/// Passed as the first argument to every `tag_invoke` overload that implements
/// `parallelReduce` for a given executor. Mirrors the `ParallelForTag` pattern
/// so the extension surface is symmetric with the rest of the parallel CPO
/// family.
struct ParallelReduceTag {};

namespace detail {

/// Function-object backing the `citor::parallelReduce` customization point.
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
struct ParallelReduceFn {
  /// Forward to the executor's `tag_invoke` overload, supplying a default
  /// `HintsT{}` value.
  ///
  /// Returns the reduction result computed by the executor. The executor's
  /// overload is responsible for selecting `Determinism::FixedBlockOrder` vs
  /// `Determinism::KahanCompensated` reduction shapes from the static-constexpr
  /// members of |HintsT|.
  ///
  /// HintsT  Hint type whose `static constexpr` members drive compile-time
  /// policy. Pool    Executor type. T       Reduction value type. Map Per-block
  /// map callable: `T(std::size_t lo, std::size_t hi)`. Combine Binary combine
  /// callable: `T(T, T)`. pool    Executor instance. first   Inclusive lower
  /// bound of the iteration range. last    Exclusive upper bound of the
  /// iteration range. init    Identity value used when the range is empty AND
  /// seed for combiner. map     Callable that produces a partial value over a
  /// chunk range. combine Binary combiner over partial values. tok Cancellation
  /// token observed at chunk boundaries. The reduction result, identical across
  /// worker counts when the hint requests a
  ///         deterministic combine tree.
  template <class HintsT, class Pool, class T, class Map, class Combine>
  [[nodiscard]] T
  operator()(Pool &pool, std::size_t first, std::size_t last, T init, Map &&map,
             Combine &&combine,
             CancellationToken tok = CancellationToken{}) const {
    return tag_invoke(*this, pool, first, last, std::move(init),
                      std::forward<Map>(map), std::forward<Combine>(combine),
                      HintsT{}, std::move(tok));
  }
};

} // namespace detail

/// Customization-point object for parallel reductions over a `[first, last)`
/// range.
///
/// Calling `parallelReduce<HintsT>(pool, first, last, init, map, combine)`
/// dispatches through unqualified `tag_invoke`; the executor's overload runs
/// the reduction synchronously and returns the combined result. The hint type
/// carries the compile-time policy (balance, determinism, chunk grain,
/// inline-fallback parameters) so every overload can specialize without runtime
/// branching.
inline constexpr detail::ParallelReduceFn parallelReduce{};

} // namespace citor
