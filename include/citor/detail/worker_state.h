#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "citor/hints.h"

namespace citor::detail {

/// Reserved slot for the per-worker Chase-Lev work-stealing deque. Holds a
/// `void *` placeholder so the `WorkerState` layout, sizing, and alignment
/// remain stable. The deque type owns the heap-allocated payload pointed to
/// by `storage`; the `void *` width keeps `WorkerState` trivially-zeroed
/// without coupling the engine to the deque header.
struct ChaseLevDequeSlot {
  /// Pointer to the heap-allocated deque payload, owned by the
  /// work-stealing deque type.
  void *storage = nullptr;
};

/// Per-worker state owned by the pool, one instance per participant.
///
/// `WorkerState` carries the worker's identity, mailbox publish/ack slot,
/// observability counters, and a pointer to the deque. Every contended
/// atomic sits on its own `kCacheLine`-sized line so a write on the
/// mailbox does not invalidate the worker's identity or counters.
///
/// Counters are relaxed-atomic so workers can update them on the hot path
/// without synchronizing other state; readers (telemetry, tests) accept
/// order-tolerant snapshots.
struct WorkerState {
  /// Per-worker mailbox: the producer's publish line for this slot, doubling
  /// as the worker's same-line DONE ack via `PoolControl::kDoneBit`.
  ///
  /// Producer publishes a new dispatch by writing this slot's mailbox to
  /// the dispatch's phase counter (bit 0 = shutdown, bits 2..63 = monotonic
  /// phase, matching `PoolControl::generation`'s layout). The worker spins
  /// on its own mailbox instead of the shared `generation`, eliminating the
  /// N-readers-on-one-line coherence storm under fan-out.
  ///
  /// Co-located with `mailboxDesc`: producer writes `mailboxDesc` first
  /// (relaxed) then `mailbox` (release) so the worker's acquire-load on
  /// `mailbox` synchronizes with both writes. Worker reads `mailboxDesc`
  /// directly off this private cache line instead of going through the
  /// shared `m_control.activeJob`, eliminating the N-readers shared-line
  /// read on every dispatch.
  ///
  /// After running its share the worker stamps `mailbox |= kDoneBit` (release)
  /// so the producer's join reads done state on the same cache line it just
  /// published the dispatch on. One line per worker on the join path replaces
  /// the old two-line publish + done-epoch protocol.
  ///
  /// Lives alone on a 128-byte line because the producer writes it on
  /// every dispatch and the worker reads it every spin iteration.
  alignas(kCacheLine) std::atomic<std::uint64_t> mailbox{0};

  /// Per-worker descriptor pointer co-located with `mailbox`. Producer
  /// writes this immediately before bumping `mailbox`; the worker's
  /// acquire-load on `mailbox` picks both up via release ordering.
  /// `nullptr` outside an active dispatch.
  void *mailboxDesc = nullptr;

  /// Identity, affinity, and link to topology metadata. `workerId` is the
  /// worker's slot index in the pool's vector; `cpuId` is the CPU id the
  /// worker was pinned to (when affinity is requested); `ccdId` is the CCD
  /// index resolved from topology.
  alignas(kCacheLine) std::uint32_t workerId = 0;

  /// CPU id the worker is pinned to; equals `UINT32_MAX` when affinity was
  /// not requested.
  std::uint32_t cpuId = UINT32_MAX;

  /// CCD (or shared-L3) group index, or `UINT32_MAX` when topology was
  /// unavailable.
  std::uint32_t ccdId = UINT32_MAX;

  /// Relaxed-atomic counters used for observability and tests. Each counter
  /// sits on its own line so observability traffic does not pollute another
  /// worker's hot path.
  alignas(kCacheLine) std::atomic<std::uint64_t> parks{0};

  /// Number of `FUTEX_WAKE_PRIVATE` calls observed by this worker.
  alignas(kCacheLine) std::atomic<std::uint64_t> wakes{0};

  /// Number of dispatches this worker participated in.
  alignas(kCacheLine) std::atomic<std::uint64_t> dispatches{0};

  /// Low-latency scope epoch most recently observed by this worker while
  /// idle.
  alignas(kCacheLine) std::atomic<std::uint64_t> hotSpinEpoch{0};

  /// Total steal probes attempted by this worker.
  alignas(kCacheLine) std::atomic<std::uint64_t> stealAttempts{0};

  /// Total steal probes that successfully dequeued a task.
  alignas(kCacheLine) std::atomic<std::uint64_t> stealSuccesses{0};

  /// Per-rank generation claim slot for producer/worker cold-collapse
  /// races. For dispatches that opt into cold-collapse
  /// (`JobDescriptor::workerStateBase != nullptr`), the producer and the
  /// worker race on this slot via `compare_exchange`: the side that bumps
  /// `claimedAt` from `<currentGen` to `currentGen` wins the right to run
  /// rank R's blocks. The loser observes the new value and skips its
  /// share. The winner stamps `mailbox = doneSentinel` after running the
  /// partition so the producer's join wait is satisfied.
  ///
  /// Lives alone on a 128-byte line because the producer's cold-collapse
  /// loop CAS-probes every background worker's slot in turn; co-locating
  /// with `mailbox` would invalidate the wakeup line during the probe.
  /// Default value `0` is below any real generation (workers' first
  /// dispatch sees gen >= `kPhaseStep` > 0).
  alignas(kCacheLine) std::atomic<std::uint64_t> claimedAt{0};

  /// Reserved deque slot.
  alignas(kCacheLine) ChaseLevDequeSlot deque{};
};

// Hot-path offsets must stay pinned: the dispatch loop indexes `mailbox`
// and the per-worker counters through these constants and a shift would
// silently move state into another cache line.
static_assert(offsetof(WorkerState, mailbox) == 0);
static_assert(offsetof(WorkerState, workerId) == kCacheLine);
static_assert(offsetof(WorkerState, parks) == kCacheLine * 2);
static_assert(offsetof(WorkerState, wakes) == kCacheLine * 3);
static_assert(offsetof(WorkerState, dispatches) == kCacheLine * 4);
static_assert(offsetof(WorkerState, hotSpinEpoch) == kCacheLine * 5);
static_assert(offsetof(WorkerState, stealAttempts) == kCacheLine * 6);
static_assert(offsetof(WorkerState, stealSuccesses) == kCacheLine * 7);
static_assert(offsetof(WorkerState, claimedAt) == kCacheLine * 8);
static_assert(offsetof(WorkerState, deque) == kCacheLine * 9);

// The full struct must fit comfortably in L2 across the worker fleet
// (16 workers x 4 KiB = 64 KiB).
static_assert(sizeof(WorkerState) <= 4096);

} // namespace citor::detail
