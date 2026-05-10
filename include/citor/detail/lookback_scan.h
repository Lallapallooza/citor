#pragma once

#include <atomic>
#include <cstdint>
#include <utility>

#include "citor/detail/cpu_relax.h"
#include "citor/hints.h"

namespace citor::detail {

/// Per-tile state for Merrill-Garland 2016 decoupled-lookback scan.
///
/// Each tile lives on its own cache line so a worker's release-store on
/// `flag` for one tile does not invalidate a neighbour's flag line. The
/// state machine is:
///
///   `Initialized` -> `AggregateAvailable` -> `PrefixAvailable`
///
/// A tile's worker:
///   1. Reads `d.in[tile_lo..tile_hi]`, computes local total and writes
///      the chunk-local inclusive scan into `d.out[tile_lo..tile_hi]`.
///   2. Release-stores `aggregate` and lifts `flag` to
///      `AggregateAvailable`.
///   3. Walks predecessors, summing `aggregate`s, until it finds a
///      predecessor in `PrefixAvailable` (acquire). That predecessor's
///      `prefix` plus the accumulated aggregates is this tile's prefix.
///   4. Release-stores `prefix` and lifts `flag` to `PrefixAvailable`.
///   5. Adds `prefix` in place over `d.out[tile_lo..tile_hi]`.
///
/// Tile 0 is special: its prefix is `identity`, so it can skip the
/// lookback walk entirely and proceed straight from step 2 to step 4.
template <class T>
struct alignas(kCacheLine) LookbackTile {
  /// State-machine value stored in `flag`. The transitions are monotonic:
  /// `Initialized` -> `AggregateAvailable` -> `PrefixAvailable`.
  enum class Flag : std::uint64_t {
    /// The tile owns a slot but has not yet computed its local aggregate.
    Initialized = 0,
    /// `aggregate` is published and synchronises through an acquire-load
    /// of `flag`. The prefix is not yet known.
    AggregateAvailable = 1,
    /// `prefix` is published and synchronises through an acquire-load of
    /// `flag`. Successors may stop their lookback walk at this tile.
    PrefixAvailable = 2,
  };

  /// State-machine bit; release-stored after the corresponding payload
  /// (aggregate or prefix) has been written, so an acquire-load of
  /// `flag` synchronises with the payload.
  std::atomic<std::uint64_t> flag{0};

  /// Tile's local total. Valid (well-defined) when `flag >=
  /// AggregateAvailable`.
  T aggregate{};

  /// Tile's exclusive prefix. Valid when `flag >= PrefixAvailable`.
  T prefix{};
};

/// Walk back from `myTile - 1`, summing predecessor aggregates, until a
/// predecessor is observed in `PrefixAvailable` state. Returns the
/// computed prefix for `myTile`.
///
/// `prefix` is the user-supplied associative combiner; the walk
/// composes left-to-right (oldest predecessor first) to preserve
/// associativity even when the combiner is not commutative.
///
/// The walk avoids stalling on a slow predecessor by spinning with
/// `cpuRelax()`; on workloads where every tile's Pass-1 work is
/// roughly uniform, the chain typically terminates after one step (the
/// immediate predecessor's prefix is already published by the time the
/// successor finishes its own aggregate).
///
/// `noexcept` follows the user combiner: a throwing |prefix| propagates
/// to the caller, which the engine catches in `runInclusiveScanLookback`
/// and surfaces through `firstException`.
template <class T, class PrefixFn>
[[gnu::always_inline]] inline T
lookbackWalk(LookbackTile<T> *tiles, std::uint32_t myTile, T identity,
             PrefixFn &&prefix) noexcept(noexcept(prefix(std::declval<T>(),
                                                         std::declval<T>()))) {
  // `accum` carries the running total of every tile whose `aggregate`
  // we have folded in but whose `prefix` was not yet published. When
  // we hit a tile in `PrefixAvailable` state, that tile's prefix
  // covers everything to its left, so the result is
  // `prefix.left = prefix(prefix.left, peer.prefix, peer.aggregate, accum)`.
  // Compose left-to-right (peer is to the left of accum) so the user
  // monoid only needs to be associative, not commutative.
  T accum = identity;
  std::uint32_t cursor = myTile;
  while (cursor > 0U) {
    --cursor;
    auto &peer = tiles[cursor];
    while (true) {
      const auto state = peer.flag.load(std::memory_order_acquire);
      if (state >=
          static_cast<std::uint64_t>(LookbackTile<T>::Flag::PrefixAvailable)) {
        // peer.prefix is the exclusive total of [0, cursor_lo);
        // peer.aggregate is the total of [cursor_lo, cursor_hi);
        // accum is the total of [cursor_hi, myTile_lo). Stitch them
        // left-to-right.
        T leftCombined = prefix(peer.prefix, peer.aggregate);
        return prefix(std::move(leftCombined), std::move(accum));
      }
      if (state >= static_cast<std::uint64_t>(
                       LookbackTile<T>::Flag::AggregateAvailable)) {
        // peer's prefix not yet ready; fold its aggregate into `accum`
        // and keep walking back.
        accum = prefix(peer.aggregate, std::move(accum));
        break;
      }
      cpuRelax();
    }
  }
  return accum;
}

} // namespace citor::detail
