#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "citor/hints.h"

namespace citor::detail {

/// Reserved slot for the per-worker Chase-Lev work-stealing deque.
///
/// Holds a `void *` placeholder so the `WorkerState` layout, sizing, and alignment remain stable
/// before the work-stealing deque type is defined. The deque type owns the heap-allocated payload
/// pointed to by `storage`; the `void *` width keeps `WorkerState` trivially-zeroed without
/// coupling the engine to the deque header.
struct ChaseLevDequeSlot {
  /// Pointer to the heap-allocated deque payload, owned by the work-stealing deque type.
  void *storage = nullptr;
};

/// Reserved slot for the per-worker rdtscp trace ring buffer.
///
/// Holds a `void *` placeholder so observability wiring can land without shuffling the
/// `WorkerState` layout. The trace-ring type owns the heap-allocated event records keyed by
/// `__rdtscp` once tracing is enabled.
struct TraceRingSlot {
  /// Pointer to the heap-allocated trace ring; populated by the trace-ring type when tracing is
  /// enabled.
  void *storage = nullptr;
};

/// Per-worker state owned by the pool, one instance per participant.
///
/// `WorkerState` carries the worker's identity, its done-epoch publishing slot, observability
/// counters, and pointers to the deque + trace ring shipped in later slices. Every contended
/// atomic sits on its own `kCacheLine`-sized line so a write on `doneEpoch` does not invalidate
/// the worker's identity or counters.
///
/// Counters are relaxed-atomic so workers can update them on the hot path without synchronizing
/// other state; readers (telemetry, tests) accept order-tolerant snapshots.
struct WorkerState {
  /// Per-worker mailbox: the producer's publish line for this slot.
  ///
  /// Producer publishes a new dispatch by writing this slot's mailbox to the dispatch's phase
  /// counter (bit 0 = shutdown, bits 2..63 = monotonic phase, matching `PoolControl::generation`'s
  /// layout). The worker spins on its own mailbox instead of the shared `generation`, eliminating
  /// the N-readers-on-one-line coherence storm that scales as O(N) wakeup latency under fan-out.
  ///
  /// Lives alone on a 128-byte line because the producer writes it on every dispatch and the
  /// worker reads it every spin iteration; false sharing with any other field on the worker's
  /// critical hot path would defeat the redesign.
  alignas(kCacheLine) std::atomic<std::uint64_t> mailbox{0};

  /// Per-worker done-epoch that the producer's join path waits on.
  ///
  /// Worker writes the mailbox phase value it last completed via `release`. The producer reads
  /// via `acquire`; the pair establishes happens-before for any data the worker wrote during the
  /// job. Lives alone on a 128-byte line because the producer reads it on the critical join path
  /// and false sharing here would gate every dispatch.
  alignas(kCacheLine) std::atomic<std::uint64_t> doneEpoch{0};

  /// Identity, affinity, and link to topology metadata.
  ///
  /// `workerId` is the worker's slot index in the pool's vector; `cpuId` is the CPU id the worker
  /// was pinned to (when affinity is requested); `ccdId` is the CCD index resolved from topology;
  /// `tlsArena` is reserved for the per-worker scratch arena populated by the primitive layer.
  alignas(kCacheLine) std::uint32_t workerId = 0;

  /// CPU id the worker is pinned to; equals `UINT32_MAX` when affinity was not requested.
  std::uint32_t cpuId = UINT32_MAX;

  /// CCD (or shared-L3) group index, or `UINT32_MAX` when topology was unavailable.
  std::uint32_t ccdId = UINT32_MAX;

  /// Pointer to per-worker scratch arena; populated by the primitive layer when scratch lands.
  void *tlsArena = nullptr;

  /// Bytes available in `tlsArena`; populated by the primitive layer when scratch lands.
  std::size_t tlsArenaBytes = 0;

  /// Relaxed-atomic counters used for observability and tests.
  ///
  /// Each counter sits on its own line because the test that asserts `parks > 0` after an idle
  /// sleep is exactly the kind of operation that should not pollute another worker's hot path.
  alignas(kCacheLine) std::atomic<std::uint64_t> parks{0};

  /// Number of `FUTEX_WAKE_PRIVATE` calls observed by this worker.
  alignas(kCacheLine) std::atomic<std::uint64_t> wakes{0};

  /// Number of dispatches this worker participated in.
  alignas(kCacheLine) std::atomic<std::uint64_t> dispatches{0};

  /// Low-latency scope epoch most recently observed by this worker while idle.
  alignas(kCacheLine) std::atomic<std::uint64_t> hotSpinEpoch{0};

  /// Total steal probes attempted by this worker.
  alignas(kCacheLine) std::atomic<std::uint64_t> stealAttempts{0};

  /// Total steal probes that successfully dequeued a task.
  alignas(kCacheLine) std::atomic<std::uint64_t> stealSuccesses{0};

  /// Reserved deque slot; populated by the work-stealing deque type when it lands.
  alignas(kCacheLine) ChaseLevDequeSlot deque{};

  /// Reserved trace ring slot; populated by the trace-ring type when tracing lands.
  alignas(kCacheLine) TraceRingSlot trace{};
};

} // namespace citor::detail
