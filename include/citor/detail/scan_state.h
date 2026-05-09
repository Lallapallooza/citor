#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <utility>

#include "citor/hints.h"

namespace citor::detail {

/// Per-chunk completion slot used by the Blelloch two-pass `parallelScan`
/// rendezvous.
///
/// Each slot lives on its own `citor::kCacheLine` -sized line so a worker's
/// release-store on `done` for one chunk cannot invalidate a neighbouring slot
/// the producer or downstream worker is acquiring. The owning worker writes
/// `done = 1` once after its pass-1 partial-sum write and `done = 2` once after
/// its pass-2 final-output write; downstream readers use acquire-loads to
/// confirm the upstream worker's pass-1 partials are visible before the
/// sequential reduce, and the producer's pass-2 join uses the same per-slot
/// epoch ladder.
///
/// Padding-suppression note: the layout keeps the atomic on its own
/// `kCacheLine`-sized line, so the analyser's "excessive padding" warning is
/// the design trade-off we want -- false-sharing avoidance over byte-tight
/// packing.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct alignas(kCacheLine) ScanDoneSlot {
  /// Pass epoch the worker last completed for its chunk. `0` means "not yet
  /// started", `1` means "pass-1 partial written and visible", `2` means
  /// "pass-2 output written".
  std::atomic<std::uint64_t> done{0};
};

/// Stack-resident state shared by the producer and background workers across
/// both passes of a
///        single `parallelScan` call.
///
/// Layout invariants:
/// - `scanCancelled` lives on its own line so cancellation broadcast does not
/// interfere with the
///   producer's hot path.
/// - `firstException` lives on its own line so exception capture does not
/// invalidate the per-chunk
///   done slots on the worker write path.
/// - `done[c]` lives on a dedicated line via `ScanDoneSlot` so adjacent chunks'
/// release-stores
///   do not contend for the same cache line.
///
/// The state itself lives on the producer's stack across a `parallelScan` call.
/// The trailing per-chunk `done` slots are NOT owned by `ScanState`: the pool
/// pre-allocates a contiguous `ChainDoneSlot` block once at construction time
/// (sized `participants()`), and every scan call borrows that block via
/// `doneSlots`, reinterpreting each `ChainDoneSlot::done` as a
/// `ScanDoneSlot::done` epoch ladder. The producer zero-resets each slot at
/// entry to honour the scan's "fresh epoch per call" contract; this avoids
/// `operator new` / `operator delete` on the dispatch hot path.
///
/// The rendezvous between Pass 1 and the sequential reduce is fully
/// decentralized: every slot stamps `done[slot] = 1` (release) after writing
/// its partial, and the producer (slot 0) waits until every slot has reached
/// `done >= 1` before computing exclusive prefixes. The rendezvous between the
/// sequential reduce and Pass 2 is implicit: the producer's release-store on
/// `prefixesPublished` synchronizes with each worker's acquire-load before that
/// worker reads `partials[slot]` as the seed for its Pass-2 body invocation.
///
/// The cancellation handshake is encoded by stamping `done = 2` on the
/// cancelled slot's line, which satisfies any active pass's wait condition
/// unconditionally and lets the spin loop drop a per-iteration `scanCancelled`
/// poll, mirroring the chain's `nStages` stamping idiom in `chain_state.h`.
///
/// Padding-suppression note: the layout keeps every contended atomic on its own
/// `kCacheLine`-sized line, so the analyser's "excessive padding" warning is
/// the design trade-off we want -- false-sharing avoidance over byte-tight
/// packing.
///
/// T Reduction value type the scan operates on.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
template <class T>
struct ScanState {
  /// Number of participants (= number of chunks) collaborating in the scan.
  std::uint32_t participants = 0;

  /// Row-range upper bound passed in by the caller; used for chunk
  /// partitioning.
  std::size_t n = 0;

  /// Cancellation flag flipped by the producer's cancellation observer.
  ///
  /// Worker bodies check this between passes and exit cleanly when set;
  /// cancelled slots stamp `done = 2` so peers waiting on `done >= target` for
  /// any active pass proceed without needing to poll this flag inside the spin
  /// loop. The flag is release-written by the slot that observed the
  /// cancellation, and acquire-read by other slots at pass boundaries.
  alignas(kCacheLine) std::atomic<std::uint32_t> scanCancelled{0};

  /// First-exception capture slot shared across all participants.
  ///
  /// Workers `compare_exchange` this from null to a heap-allocated
  /// `std::exception_ptr` to record the first failure deterministically;
  /// subsequent throws drop. The producer reads the slot after joining and
  /// rethrows if non-null. Allocation only happens on the cold throw path.
  alignas(kCacheLine) std::atomic<std::exception_ptr *> firstException{nullptr};

  /// Producer-side flag flipped after the sequential reduce computes every
  /// chunk's exclusive
  ///        prefix.
  ///
  /// Workers acquire-spin on this between passes; the release-store from the
  /// producer publishes the `partials` array (re-purposed to hold exclusive
  /// prefixes after the reduce) so every worker's pass-2 body sees the seed for
  /// its chunk.
  alignas(kCacheLine) std::atomic<std::uint32_t> prefixesPublished{0};

  /// Per-chunk partial / exclusive-prefix slot.
  ///
  /// After Pass 1, `partials[c]` holds chunk `c`'s partial sum. After the
  /// sequential reduce, `partials[c]` holds chunk `c`'s exclusive prefix (the
  /// seed each Pass-2 invocation feeds back into the body's `initial`
  /// argument). Sized `participants` valid elements.
  T *partials = nullptr;

  /// Borrowed pointer to the pool's pre-allocated per-worker completion slots.
  ///
  /// The pool owns the storage; the scan call reserves a fresh interval
  /// `[epochBase, epochBase + 2]` in the pool's monotonically-advancing epoch
  /// counter so successive calls observe disjoint targets without
  /// zero-resetting the slots. Sized `participants` valid elements; reading
  /// past that index is undefined.
  ChainDoneSlot *doneSlots = nullptr;

  /// Per-call base of the pool's monotonic done-epoch counter.
  ///
  /// Stamps are absolute: `done = epochBase + 1` after Pass 1, `done =
  /// epochBase + 2` after Pass 2. Waits compare against `epochBase + 1` or
  /// `epochBase + 2`. The producer reserves the interval under the dispatch
  /// gate before publishing, so prior-dispatch values cannot satisfy a current
  /// wait. Cancellation stamps `epochBase + 2` so peers waiting on either pass
  /// advance.
  std::uint64_t epochBase = 0;

  /// Subscript a slot by index.
  ///
  /// The slot itself owns mutable atomics; the accessor is `const` because
  /// reading from the borrowed pointer does not modify the owning state, and
  /// the returned slot's atomics carry their own internal mutability.
  ///
  /// idx Slot index in `[0, participants)`.
  /// Reference to the slot.
  [[nodiscard]] ChainDoneSlot &doneSlot(std::size_t idx) const noexcept {
    return doneSlots[idx];
  }

  /// Compute a slot's contiguous row range over `[0, n)` using static
  /// partitioning.
  ///
  /// The partition is `lo = (n * slot) / participants`, `hi = (n * (slot + 1))
  /// / participants`, matching the chain / plex convention so the scan's chunk
  /// identity is bit-stable across worker counts at fixed `n`.
  ///
  /// slot Worker slot index in `[0, participants)`.
  /// `(lo, hi)` pair denoting the slot's contiguous range over `[0, n)`.
  [[nodiscard]] std::pair<std::size_t, std::size_t>
  slotRange(std::uint32_t slot) const noexcept {
    using u128 = unsigned __int128;
    const std::size_t lo =
        static_cast<std::size_t>((static_cast<u128>(n) * slot) / participants);
    const std::size_t hi = static_cast<std::size_t>(
        (static_cast<u128>(n) * (slot + 1U)) / participants);
    return {lo, hi};
  }
};

} // namespace citor::detail
