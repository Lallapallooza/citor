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

  /// Borrowed pointer to the pool's per-slot CCD index array.
  ///
  /// Set when the call enables CCD-aware asymmetric chunk partitioning;
  /// nullptr otherwise. Used by `slotRange` to give larger chunks to slots on
  /// the producer's CCD and smaller chunks to slots on a different CCD,
  /// reducing the cross-CCD coherence volume on Pass-2 writes.
  const std::uint32_t *ccdOfSlot = nullptr;

  /// CCD identity of the producer (slot 0). When asymmetric partitioning is
  /// enabled, slots whose `ccdOfSlot[s] == producerCcd` are the
  /// "producer-CCD" slots and receive a larger share of `n`.
  std::uint32_t producerCcd = UINT32_MAX;

  /// Numerator (out of 16) of `n` allocated to the producer-CCD slot group as
  /// a whole. The cross-CCD slot group receives `(16 - asymmetricNum) / 16`.
  /// Used only when `ccdOfSlot != nullptr`. Shared evenly within each group.
  /// The declarator default matches the pool's initial value so the field
  /// stays in a consistent neutral state until the per-call setup overwrites
  /// it.
  std::uint32_t asymmetricNum = 8;

  /// Number of slots on the producer's CCD. Precomputed by the caller to
  /// avoid a per-`slotRange()` walk over `ccdOfSlot`.
  std::uint32_t slotsOnProducerCcd = 0;

  /// Probe-derived per-slot cluster id; `clusterIdOfSlot[s]` indexes a
  /// `[0, numClusters)` cluster. Set when the pool's coherence probe
  /// returns `numClusters >= 2` and the scan call opts into the
  /// hierarchical algorithm. Nullptr when the call uses the
  /// single-cluster reduce path.
  const std::uint32_t *clusterIdOfSlot = nullptr;

  /// Number of distinct clusters in this scan call. Zero when the
  /// hierarchical path is disabled.
  std::uint32_t numClusters = 0;

  /// Per-cluster slot ranges precomputed by the caller. `clusterFirstSlot[c]`
  /// is the lowest slot index in cluster `c`; `clusterSlotCount[c]` is the
  /// count of slots in cluster `c`. The caller arranges that each cluster's
  /// slots are contiguous in slot-index order, so cluster k's slots are
  /// `[clusterFirstSlot[k], clusterFirstSlot[k] + clusterSlotCount[k])`.
  /// Nullptr when the hierarchical path is disabled.
  const std::uint32_t *clusterFirstSlot = nullptr;
  /// Companion to `clusterFirstSlot`. `clusterSlotCount[c]` is the number
  /// of slots in cluster `c`. Nullptr when the hierarchical path is
  /// disabled.
  const std::uint32_t *clusterSlotCount = nullptr;

  /// Per-cluster total / exclusive prefix slots. Each lives on its own
  /// cache line. `clusterTotals[k]` is written by cluster k's leader
  /// after its local reduce; `clusterPrefixes[k]` is the cross-cluster
  /// exclusive prefix the producer writes back. Both are sized
  /// `numClusters`. Nullptr when the hierarchical path is disabled.
  T *clusterTotals = nullptr;
  /// Per-cluster cross-cluster exclusive prefixes the producer writes back
  /// after the cluster reduce. Sized `numClusters`. Nullptr when the
  /// hierarchical path is disabled.
  T *clusterPrefixes = nullptr;

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

  /// Compute a slot's contiguous row range over `[0, n)`.
  ///
  /// Uniform mode (`ccdOfSlot == nullptr`): `lo = (n * slot) / participants`,
  /// `hi = (n * (slot + 1)) / participants`, matching the chain / plex
  /// convention so the scan's chunk identity is bit-stable across worker
  /// counts at fixed `n`.
  ///
  /// Asymmetric mode (`ccdOfSlot != nullptr`): producer-CCD slots receive a
  /// larger contiguous prefix of `n` (`asymmetricNum / 16`), cross-CCD slots
  /// receive the trailing remainder. Within each group the chunks are
  /// uniform. The producer-CCD slots take the prefix `[0, producerVolume)` so
  /// chunk-id-order over slots is still left-to-right, preserving the
  /// `slot=0` -> `chunk-id=0` invariant the seq-reduce relies on. Whether
  /// this is enabled is decided per-call by `runScanParallel` based on
  /// detected cross-CCD presence; it is opt-in to avoid regressing balanced
  /// compute-bound bodies on single-CCD or homogeneous-CCD topologies.
  ///
  /// slot Worker slot index in `[0, participants)`.
  /// `(lo, hi)` pair denoting the slot's contiguous range over `[0, n)`.
  [[nodiscard]] std::pair<std::size_t, std::size_t>
  slotRange(std::uint32_t slot) const noexcept {
    __extension__ using u128 = unsigned __int128;
    if (ccdOfSlot == nullptr) {
      const auto lo = static_cast<std::size_t>((static_cast<u128>(n) * slot) /
                                               participants);
      const auto hi = static_cast<std::size_t>(
          (static_cast<u128>(n) * (slot + 1U)) / participants);
      return {lo, hi};
    }
    // Producer-CCD slot group covers the prefix `[0, producerVolume)`;
    // cross-CCD group covers `[producerVolume, n)`.
    const auto producerVolume =
        static_cast<std::size_t>((static_cast<u128>(n) * asymmetricNum) / 16U);
    const std::uint32_t numProducer = slotsOnProducerCcd;
    const std::uint32_t numCross = participants - slotsOnProducerCcd;
    // Index of `slot` within its CCD group (0-based, in slot-index order).
    std::uint32_t indexInGroup = 0;
    const bool isProducerCcd = (ccdOfSlot[slot] == producerCcd);
    for (std::uint32_t s = 0; s < slot; ++s) {
      if ((ccdOfSlot[s] == producerCcd) == isProducerCcd) {
        ++indexInGroup;
      }
    }
    if (isProducerCcd && numProducer > 0U) {
      const auto lo = static_cast<std::size_t>(
          (static_cast<u128>(producerVolume) * indexInGroup) / numProducer);
      const auto hi = static_cast<std::size_t>(
          (static_cast<u128>(producerVolume) * (indexInGroup + 1U)) /
          numProducer);
      return {lo, hi};
    }
    if (numCross > 0U) {
      const std::size_t crossVolume = n - producerVolume;
      const std::size_t lo =
          producerVolume +
          static_cast<std::size_t>(
              (static_cast<u128>(crossVolume) * indexInGroup) / numCross);
      const std::size_t hi =
          producerVolume +
          static_cast<std::size_t>(
              (static_cast<u128>(crossVolume) * (indexInGroup + 1U)) /
              numCross);
      return {lo, hi};
    }
    return {0U, 0U};
  }
};

} // namespace citor::detail
