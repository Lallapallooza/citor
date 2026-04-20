#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <utility>

#include "citor/hints.h"

namespace citor::detail {

/// Per-worker phase-completion slot used by the persistent-worker plex protocol.
///
/// Each slot lives on its own `citor::kCacheLine` -sized line so the producer's
/// acquire-loads on `done` during phase rendezvous never collide with a neighbouring worker's
/// release-store. The owning worker writes `done` exactly once per phase via release; the producer
/// reads it via acquire to confirm the worker's slice for that phase has retired.
struct alignas(kCacheLine) PlexDoneSlot {
  /// Phase epoch the worker last finished. Producer waits until `done >= currentPhase` before
  /// admitting the next phase.
  std::atomic<std::uint64_t> done{0};
};

/// Stack-resident state shared by the producer and background workers across all phases of a
///        single `runPlex` call.
///
/// Layout invariants:
/// - `currentPhase` lives on its own cache line so the producer's release-store does not invalidate
///   per-worker state every phase.
/// - `phaseCancelled` lives on its own line so cancellation broadcast does not interfere with the
///   producer's phase-publish hot path.
/// - `done[w]` lives on a dedicated line via `PlexDoneSlot` so adjacent workers' release-stores
///   do not contend for the same cache line.
///
/// The state is owned by the producer's stack via a `std::unique_ptr` so the trailing `done[]`
/// vector is heap-resident with cache-line alignment but the producer still controls the lifetime.
/// Workers receive a non-owning pointer through the dispatch closure.
///
/// Padding-suppression note: the layout keeps every contended atomic on its own
/// `kCacheLine`-sized line, so the analyser's "excessive padding" warning is the design trade-off
/// we want -- false-sharing avoidance over byte-tight packing.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct PlexState {
  /// Total number of phases the user requested.
  std::size_t nPhases = 0;

  /// Per-phase row range upper bound passed in by the caller; used for slot partitioning.
  std::size_t n = 0;

  /// Number of participants (producer + background workers) collaborating in the plex.
  std::uint32_t participants = 0;

  /// Phase epoch published by the producer. Workers acquire-spin until
  /// `currentPhase >= localPhase` before admitting their slice for `localPhase`.
  ///
  /// Initial value is `0`; the producer publishes `1, 2, ..., nPhases` in order. Workers complete
  /// phase `p` when they observe `currentPhase >= p`, then signal `done[slot] = p`.
  alignas(kCacheLine) std::atomic<std::uint64_t> currentPhase{0};

  /// Cancellation flag flipped by the producer's cancellation observer.
  ///
  /// Worker bodies check this between phases and exit cleanly when set. The flag is
  /// release-written by the producer at most once per call (when the user's token transitions to
  /// stopped), and acquire-read by workers between phases.
  alignas(kCacheLine) std::atomic<std::uint32_t> phaseCancelled{0};

  /// First-exception capture slot shared across all participants.
  ///
  /// Workers `compare_exchange` this from null to a heap-allocated `std::exception_ptr` to record
  /// the first failure deterministically; subsequent throws drop. The producer reads the slot
  /// after joining and rethrows if non-null.
  alignas(kCacheLine) std::atomic<std::exception_ptr *> firstException{nullptr};

  /// Custom deleter that destroys each `PlexDoneSlot` in place and frees the aligned heap
  /// block.
  ///
  /// Mirrors `ThreadPool::WorkerArrayDeleter`: the slot type holds an `std::atomic` which is not
  /// copy- or move-insertable, so `std::vector` cannot hold it directly. Bare aligned `operator
  /// new[]` plus per-element placement-new is the established pattern in the pool.
  struct DoneArrayDeleter {
    /// Number of slots in the block; supplied by `allocateDone`.
    std::size_t count = 0;

    /// Destroy and deallocate the slot block previously created via aligned `operator new`.
    ///
    /// ptr Pointer to the first slot; may be `nullptr`.
    void operator()(PlexDoneSlot *ptr) const noexcept {
      if (ptr == nullptr) {
        return;
      }
      for (std::size_t i = count; i > 0; --i) {
        (ptr + (i - 1))->~PlexDoneSlot();
      }
      ::operator delete(static_cast<void *>(ptr), std::align_val_t{kCacheLine});
    }
  };

  /// Per-worker completion slots, sized `participants`. Index 0 is the producer; indices
  /// `[1, participants)` are background workers.
  ///
  /// Allocated through aligned `operator new[]` so each slot's first byte falls on a cache-line
  /// boundary; the `alignas(kCacheLine)` on `PlexDoneSlot` then keeps adjacent slots on
  /// separate lines, eliminating false sharing on the worker-write hot path.
  std::unique_ptr<PlexDoneSlot, DoneArrayDeleter> done{nullptr, DoneArrayDeleter{}};

  /// Allocate the per-worker `done` slots for |p| participants.
  ///
  /// Each slot is default-constructed with `done = 0`. Throws `std::bad_alloc` on failure.
  ///
  /// p Number of participants the plex will dispatch.
  void allocateDone(std::size_t p) {
    void *raw = ::operator new(sizeof(PlexDoneSlot) * p, std::align_val_t{kCacheLine});
    auto *first = static_cast<PlexDoneSlot *>(raw);
    for (std::size_t i = 0; i < p; ++i) {
      ::new (static_cast<void *>(first + i)) PlexDoneSlot();
    }
    done = std::unique_ptr<PlexDoneSlot, DoneArrayDeleter>(first, DoneArrayDeleter{p});
    participants = static_cast<std::uint32_t>(p);
  }

  /// Subscript a slot by index.
  ///
  /// The slot itself owns mutable atomics; the accessor is `const` because reading from the
  /// `unique_ptr` does not modify the owning state, and the returned slot's atomics carry their
  /// own internal mutability.
  ///
  /// idx Slot index in `[0, participants)`.
  /// Reference to the slot.
  [[nodiscard]] PlexDoneSlot &doneSlot(std::size_t idx) const noexcept { return done.get()[idx]; }

  /// Compute a slot's contiguous row range over `[0, n)` using static partitioning.
  ///
  /// The partition is `lo = (n * slot) / participants`, `hi = (n * (slot + 1)) / participants`,
  /// matching the prim_mst_backend.h convention so the migration produces bit-identical block
  /// boundaries.
  ///
  /// slot Worker slot index in `[0, participants)`.
  /// `(lo, hi)` pair denoting the slot's contiguous range over `[0, n)`.
  [[nodiscard]] std::pair<std::size_t, std::size_t> slotRange(std::uint32_t slot) const noexcept {
    using u128 = unsigned __int128;
    const std::size_t lo =
        static_cast<std::size_t>((static_cast<u128>(n) * slot) / participants);
    const std::size_t hi = static_cast<std::size_t>(
        (static_cast<u128>(n) * (slot + 1U)) / participants);
    return {lo, hi};
  }
};

} // namespace citor::detail
