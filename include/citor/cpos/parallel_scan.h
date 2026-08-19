#pragma once

#include <cstddef>
#include <utility>

#include "citor/cancellation.h"

namespace citor {

/// Tag type identifying the `citor::parallelScan` customization point.
///
/// Passed as the first argument to every `tag_invoke` overload that implements
/// `parallelScan` for a given executor. Mirrors the `ParallelForTag` pattern so
/// the extension surface is symmetric with the rest of the parallel CPO family.
struct ParallelScanTag {};

namespace detail {

/// Function-object backing the `citor::parallelScan` customization point.
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
struct ParallelScanFn {
  /// Forward to the executor's `tag_invoke` overload, supplying a default
  /// `HintsT{}` value.
  ///
  /// The hint type is supplied as an explicit template parameter at the call
  /// site (mirroring the member-template surface) so the executor's overload
  /// can specialize on it via `if constexpr` or a regular template parameter.
  ///
  /// Returns the final inclusive-prefix accumulator computed across every
  /// chunk: this matches the `op(prefix[last], partial[last])` value Blelloch's
  /// two-pass scan produces at the right edge, and gives callers a single value
  /// to consume without a follow-up reduction.
  ///
  /// |n| is the range length, partitioned across slots as
  /// `[n*slot/P, n*(slot+1)/P)`. |identity| seeds the first chunk's `initial`
  /// and is the result for `n == 0`. The engine invokes |body| as
  /// `T(chunkId, lo, hi, initial, out)`, twice per slot: Pass 1 with
  /// `initial = identity`, Pass 2 with `initial = exclusivePrefix[slot]`.
  /// |prefix| is the binary combiner that produces the cross-chunk
  /// exclusive-prefix sequence. Workers read |tok| at pass boundaries.
  template <class HintsT, class Pool, class T, class BodyFn, class PrefixFn>
  [[nodiscard]] T
  operator()(Pool &pool, std::size_t n, T identity, BodyFn &&body,
             PrefixFn &&prefix,
             CancellationToken tok = CancellationToken{}) const {
    return tag_invoke(*this, pool, n, std::move(identity),
                      std::forward<BodyFn>(body),
                      std::forward<PrefixFn>(prefix), HintsT{}, std::move(tok));
  }
};

} // namespace detail

/// Customization-point object for the two-pass parallel prefix scan.
///
/// Calling `parallelScan<HintsT>(pool, n, identity, body, prefix)` dispatches
/// through unqualified `tag_invoke`; the executor's overload runs the scan
/// synchronously and returns the inclusive accumulator at the right edge. The
/// hint type carries the compile-time policy (balance, chunk grain,
/// inline-fallback parameters) so every overload can specialize without runtime
/// branching.
///
/// The reduce-then-scan shape avoids the `n^2/p` sequential bottleneck of a
/// naive split-recombine:
///
///   1. Pass 1 computes per-chunk partial sums in parallel.
///   2. The producer computes the chunk-level exclusive prefixes serially in
///      `O(participants)`.
///   3. Pass 2 re-runs the body with each chunk's exclusive prefix as
///      `initial` to write the final scan output.
inline constexpr detail::ParallelScanFn parallelScan{};

} // namespace citor
