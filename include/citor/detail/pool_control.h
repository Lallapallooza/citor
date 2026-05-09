#pragma once

#include <atomic>
#include <cstdint>

#include "citor/hints.h"

namespace citor::detail {

/// Process-internal control word shared between producer and workers.
///
/// Four contended atomics (`generation`, `futexWord`, `activeJob`, `hotSpinDepth`) plus a const
/// `participants` count form the source of truth for pool state. Each contended atomic is on its own
/// `kCacheLine`-sized line so MESI traffic on one never invalidates another. The layout places
/// `generation` (release publish), `futexWord` (parking token), `activeJob` (descriptor pointer), a
/// low-latency spin-depth gate, and `participants` on dedicated 128-byte lines.
///
/// The 64-bit `generation` carries both flags and a monotonic phase counter. Bits 0 (shutdown) and
/// 1 (cancel) are reserved; the producer increments by 4 per published job so the high 62 bits act
/// as the ABA-free phase counter. A 32-bit phase would be at risk of wrapping under sustained
/// dispatch; 64 bits is overkill but free given the cache-line padding.
///
/// The `futexWord` is parking-only: workers re-check `generation` after every wait return, so
/// spurious or duplicated wakes are correctness-neutral. Updates use `relaxed` atomics; the
/// happens-before chain runs through `generation` (release) instead.
///
/// `activeJob` is published with `release`; observed with `acquire`. The slot is `nullptr` until
/// a primitive publishes a `JobDescriptor`; the engine itself never writes here.
struct PoolControl {
  /// Bit flag in `generation` indicating the pool has been told to shut down.
  ///
  /// Set once by the destructor's `fetch_or`; never cleared. Workers observing this exit the loop.
  static constexpr std::uint64_t kShutdownBit = 1ULL << 0;

  /// Bit flag in `generation` reserved for global cancellation broadcasts.
  ///
  /// Reserved for pool-wide cancellation; the bit lives here so the `generation` layout is stable
  /// once the cancellation path lands without needing to shuffle the flag-bit assignments.
  static constexpr std::uint64_t kCancelBit = 1ULL << 1;

  /// Bit set by a worker on its `mailbox` line to acknowledge dispatch completion.
  ///
  /// Same-line ack protocol: the producer publishes the new phase with this bit clear; the
  /// worker stamps `mailbox = phase | kDoneBit` after running its share. The producer's join
  /// reads the worker's mailbox (the same line it published to) and waits for the DONE bit
  /// to appear. Removes the separate `doneEpoch` cache-line transit on the hot path.
  ///
  /// Lives in the bit-1 slot that was reserved for cancel broadcasts. The cancel path is
  /// carried by `CancellationToken`, not by a generation/mailbox flag, so the bit was free.
  static constexpr std::uint64_t kDoneBit = 1ULL << 1;

  /// Bit set by the producer on the worker's `mailbox` when this dispatch reuses the previous
  /// dispatch's typed-runner cached parameters (same-command reuse fast path). When the worker
  /// observes this bit, it skips reading desc fields entirely and uses its TLS-cached
  /// (HintsT, F) job parameters. Producer sets it only when:
  ///   1. Hints opt-in (StaticUniform balance, no cancellation, nothrow body, lvalue F)
  ///   2. The current dispatch's key matches the producer's TLS-cached key
  ///   3. Low-latency scope is active (so workers spin and observe the bit promptly)
  static constexpr std::uint64_t kReuseBit = 1ULL << 2;

  /// Bit set by the producer on low-latency dispatches where every worker has already
  /// acknowledged hot-spin mode and the fanout is large enough that per-rank cold-collapse
  /// ownership CAS traffic costs more than it can save. Workers that observe this bit skip the
  /// `claimedAt` CAS and stamp DONE with a plain release store.
  static constexpr std::uint64_t kSkipClaimBit = 1ULL << 3;

  /// Bit set by the worker on its `mailbox` after the producer self-stamped `doneSentinel`
  /// in the cold-collapse path. The worker's natural CAS at the end of a dispatch fails
  /// when the producer raced ahead, leaving acquire-only ordering on the failure path. This
  /// bit lets the worker re-emit a release-store on the same line so the producer's next
  /// dispatch can acquire-load it before constructing a fresh `JobDescriptor` on the same
  /// stack address. Only inspected by the producer during the publish path's pre-write
  /// wait when a prior dispatch's cold-collapse left an unacked slot.
  static constexpr std::uint64_t kAckedBit = 1ULL << 4;

  /// Number of low bits reserved for flags; producer increments `generation` by `1 << kPhaseShift`.
  /// Bits: 0=shutdown, 1=done (worker->producer ack), 2=reuse (producer->worker hot-path hint),
  /// 3=skip cold-collapse claim on hot large fanouts, 4=cold-collapse ack (worker->producer).
  static constexpr std::uint64_t kPhaseShift = 5;

  /// Increment applied per published phase so flags survive the bump.
  static constexpr std::uint64_t kPhaseStep = 1ULL << kPhaseShift;

  /// Mask of all flag bits below the phase counter.
  static constexpr std::uint64_t kFlagMask = kPhaseStep - 1;

  /// Source-of-truth phase counter.
  ///
  /// Bit 0 = shutdown, bit 1 = cancel-broadcast, bits 2..63 = monotonic phase. Producer publishes
  /// a new phase via `release`; workers read with `acquire`. Together with `activeJob` this is the
  /// acquire/release pair that orders descriptor visibility. `activeJob` is co-located on the
  /// same cache line so the worker's first acquire-load of `generation` also pulls in the
  /// new descriptor pointer with one cache-line transit.
  alignas(kCacheLine) std::atomic<std::uint64_t> generation{0};

  /// Descriptor pointer published alongside each new generation.
  ///
  /// Co-located with `generation` on the same cache line: the producer writes both in program
  /// order (`activeJob` first, then `generation`), and on x86 TSO the worker's acquire-load of
  /// `generation` synchronizes-with both stores in a single cache-line fetch. The slot is
  /// cleared to `nullptr` once in `shutdownAndJoin` (BEFORE the shutdown bit is set on
  /// `generation`) so worker `shouldExit` semantics are preserved without per-dispatch clears.
  std::atomic<void *> activeJob{nullptr};

  /// 32-bit parking token used as the futex address.
  ///
  /// Updates are relaxed: the futex word identifies the wait queue and gates re-entry into the
  /// kernel; happens-before runs through `generation`'s release/acquire pair instead. ABA-safe by
  /// construction because callers re-read `generation` after a wake before assuming a new job
  /// landed.
  alignas(kCacheLine) std::atomic<std::uint32_t> futexWord{0};

  /// Active low-latency scopes that keep workers spinning instead of parking.
  ///
  /// When non-zero, idle workers re-enter their spin loop after the normal spin budget instead of
  /// calling into futex wait. Producers may skip the per-dispatch futex wake while this gate is set
  /// because a scope transition wakes parked workers once before hot dispatch begins.
  alignas(kCacheLine) std::atomic<std::uint32_t> hotSpinDepth{0};

  /// Monotonic epoch bumped when entering low-latency mode so workers can acknowledge readiness.
  alignas(kCacheLine) std::atomic<std::uint64_t> hotSpinEpoch{0};

  /// Number of participants the pool was constructed with (producer + background workers).
  ///
  /// Read by every worker but never modified after construction; placed on its own line so reads
  /// never share a cache line with the contended atomics above.
  alignas(kCacheLine) std::uint32_t participants = 0;

  /// Pre-computed bitmask of background-worker slots `[1, participants)` for the join's
  ///        pending set; producer slot 0 already cleared.
  ///
  /// Constant for the pool's lifetime (set once at construction). Co-located on the
  /// `participants` cache line so the producer's dispatch picks both fields from a single
  /// cache-line fetch instead of reaching into `ThreadPool`'s own member layout.
  std::uint64_t pendingMaskBits = 0;
};

/// Pool-level relaxed-atomic counters for diagnostics.
///
/// Three monotonic pool-scoped counters incremented at the dispatch publish, inline-fallback, and
/// cancellation-observed sites. Worker-scoped counters (futex parks/wakes, steal attempts) live
/// on `WorkerState` and are aggregated into `PoolCountersSnapshot` by `snapshotCounters()`.
///
/// **Compile-time gated by `CITOR_ENABLE_POOL_COUNTERS`.** When the macro is undefined (the
/// default), the struct has no atomic members and the increment sites compile to no-ops; the
/// dispatch hot path pays zero extra atomics. When defined, each increment is `relaxed` and the
/// struct is on its own cache line so the counter line never bounces with `PoolControl`'s
/// contended atomics. Reset is not provided; counters are cumulative for the pool's lifetime.
#ifdef CITOR_ENABLE_POOL_COUNTERS
struct alignas(kCacheLine) PoolCounters {
  /// Producer-side dispatches that reached the worker fan-out path (one increment per published
  /// generation).
  std::atomic<std::uint64_t> dispatches{0};

  /// Calls that hit the `runInline` short-circuit (single participant, range too small per
  /// `minTaskUs`, cross-arena guard, or empty range).
  std::atomic<std::uint64_t> inlineFallbacks{0};

  /// Producer observed a cancellation request before fan-out and skipped the body.
  std::atomic<std::uint64_t> cancellationStops{0};
};
#define CITOR_COUNTERS_INC(member)                                                                 \
  do {                                                                                             \
    m_counters.member.fetch_add(1, std::memory_order_relaxed);                                     \
  } while (0)
#else
struct PoolCounters {}; // empty stub; the member is zero-sized when counters are disabled.
#define CITOR_COUNTERS_INC(member)                                                                 \
  do {                                                                                             \
  } while (0)
#endif

/// Snapshot POD returned by `ThreadPool::snapshotCounters()`. Pool-scoped fields come from
/// `PoolCounters`; worker-scoped fields are aggregated by summing the matching field across
/// every `WorkerState`. Each load is `relaxed` so values may not reflect a single point in time.
struct PoolCountersSnapshot {
  /// Producer dispatches that reached fan-out (matches `PoolCounters::dispatches`).
  std::uint64_t dispatches = 0;
  /// `runInline` short-circuits (matches `PoolCounters::inlineFallbacks`).
  std::uint64_t inlineFallbacks = 0;
  /// Producer-observed cancellation stops (matches `PoolCounters::cancellationStops`).
  std::uint64_t cancellationStops = 0;
  /// Sum of `WorkerState::parks` across workers (worker-side `FUTEX_WAIT_PRIVATE` invocations).
  std::uint64_t futexParks = 0;
  /// Sum of `WorkerState::wakes` across workers (worker-side futex returns).
  std::uint64_t futexWakes = 0;
  /// Sum of `WorkerState::stealAttempts` across workers (forkJoin steal probes).
  std::uint64_t stealAttempts = 0;
  /// Sum of `WorkerState::stealSuccesses` across workers (forkJoin steals that found work).
  std::uint64_t stealSuccesses = 0;
};

} // namespace citor::detail
