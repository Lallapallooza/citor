#pragma once

#include <cstddef>
#include <utility>

#include "citor/cancellation.h"

namespace citor {

/// Tag type identifying the `citor::bulkForQueries` customization point.
///
/// Passed as the first argument to every `tag_invoke` overload that implements `bulkForQueries`
/// for a given executor. Mirrors the `ParallelForTag` pattern so the extension surface is
/// symmetric with the rest of the parallel CPO family.
struct BulkForQueriesTag {};

namespace detail {

/// Function-object backing the `citor::bulkForQueries` customization point.
///
/// Dispatches to an unqualified `tag_invoke` call so ADL can reach overloads in the executor's
/// namespace. The function object itself is passed as the first argument so overloads can key on
/// the CPO identity in addition to the tag.
///
/// The `HintsT` template parameter is a *type*, not a value: that lets the friend overload on
/// `ThreadPool` template on the same `HintsT` and monomorphize identically to the member-template
/// call. Both surfaces ultimately route through the same engine; the CPO has zero runtime hint
/// dispatch cost.
struct BulkForQueriesFn {
  /// Forward to the executor's `tag_invoke` overload, supplying a default `HintsT{}` value.
  ///
  /// The hint type is supplied as an explicit template parameter at the call site (mirroring the
  /// member-template surface) so the executor's overload can specialize on it via `if constexpr`
  /// or a regular template parameter.
  ///
  /// HintsT  Hint type whose `static constexpr` members drive compile-time policy.
  /// Pool    Executor type.
  /// QueryFn Callable invoked once per chunk as
  ///                 `QueryFn(std::size_t qFirst, std::size_t qLast)`; the body must process every
  ///                 query index in `[qFirst, qLast)`.
  /// pool    Executor instance.
  /// q       Total query count; the engine fans `[0, q)` across workers.
  /// fn      Callable invoked over each chunk of the query range.
  /// tok     Cancellation token observed at chunk boundaries.
  template <class HintsT, class Pool, class QueryFn>
  void operator()(Pool &pool, std::size_t q, QueryFn &&fn, CancellationToken tok = CancellationToken{}) const {
    tag_invoke(*this, pool, q, std::forward<QueryFn>(fn), HintsT{}, std::move(tok));
  }
};

} // namespace detail

/// Customization-point object for fanning many independent queries across a pool.
///
/// Calling `bulkForQueries<HintsT>(pool, q, fn)` dispatches through unqualified `tag_invoke`; the
/// executor's overload partitions `[0, q)` and invokes `fn(qFirst, qLast)` on each chunk so the
/// body can run every query index in the range. The default `Balance::DynamicChunked` carried by
/// `citor::BulkQueryHints` and friends amortizes per-query traversal-depth
/// skew across workers; sites whose per-query cost is uniform can override the policy with a
/// static-uniform hint.
///
/// Output stability: per-query results MUST be written to a per-query slot (`out[q]` keyed on the
/// query index passed into `fn`). The chunk dispatch order varies across worker counts; only
/// indexing by query index gives a bit-identical output regardless of policy.
inline constexpr detail::BulkForQueriesFn bulkForQueries{};

} // namespace citor
