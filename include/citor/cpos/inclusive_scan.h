#pragma once

#include <cstddef>
#include <span>
#include <utility>

#include "citor/cancellation.h"

namespace citor {

/// Tag type identifying the `citor::inclusiveScan` customization point.
struct InclusiveScanTag {};

namespace detail {

/// Function-object backing the `citor::inclusiveScan` customization
/// point.
///
/// `inclusiveScan` is an opinionated buffer-to-buffer prefix-sum entry
/// distinct from `parallelScan`. The two differ in what the engine
/// observes:
///
///   * `parallelScan` takes a user body. The engine knows the chunk
///     ranges but not the addresses of the input or output buffers, so
///     it cannot prefetch, NT-store, or otherwise reorder the user's
///     memory accesses. It is fully general (any monoid, any
///     side-effecting body) at the cost of leaving micro-architectural
///     headroom on the table.
///   * `inclusiveScan` takes the input and output buffers directly. The
///     engine owns the inner loop and is free to use whatever memory
///     traffic shape minimises wall time on the host: decoupled-lookback
///     single-pass, `PREFETCHW` write-prefetch ahead of Pass 2, NT stores
///     on workloads where the output is larger than L3, AVX-512 in-register
///     scan, per-cluster lookback chains, etc.
///
/// The tradeoff: `inclusiveScan` is restricted to plain memory-to-memory
/// scans of trivially-relocatable types under a user-supplied associative
/// combiner. Bodies that need to inspect or mutate side state, allocate,
/// or otherwise reach beyond `[in, out)` should keep using
/// `parallelScan`.
struct InclusiveScanFn {
  /// Compute an inclusive prefix scan from `in` to `out` under `prefix`.
  ///
  /// `in` and `out` must have equal length and may alias only when the
  /// caller has made the alias safe (the engine reads `in[i]` before
  /// writing `out[i]` for every `i` so `in == out` is well-formed). The
  /// returned value is the inclusive total at the right edge --
  /// `prefix(prefix(... prefix(identity, in[0]) ...), in[n-1])` -- and
  /// matches the value Blelloch's two-pass scan produces.
  ///
  /// The hint type carries compile-time policy (per-tile size cap,
  /// affinity, priority); the engine consults `HintsT::stealPolicy` only
  /// for any nested fork/join the implementation may use internally
  /// (currently none).
  template <class HintsT, class Pool, class T, class PrefixFn>
  [[nodiscard]] T
  operator()(Pool &pool, std::span<const T> in, std::span<T> out, T identity,
             PrefixFn &&prefix,
             const CancellationToken &tok = CancellationToken{}) const {
    return tag_invoke(*this, pool, in, out, std::move(identity),
                      std::forward<PrefixFn>(prefix), HintsT{}, tok);
  }
};

} // namespace detail

/// Customization-point object for the buffer-to-buffer inclusive
/// prefix scan. See `detail::InclusiveScanFn` for the contract.
inline constexpr detail::InclusiveScanFn inclusiveScan{};

} // namespace citor
