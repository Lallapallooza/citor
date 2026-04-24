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

  /// Number of low bits reserved for flags; producer increments `generation` by `1 << kPhaseShift`.
  static constexpr std::uint64_t kPhaseShift = 2;

  /// Increment applied per published phase so flags survive the bump.
  static constexpr std::uint64_t kPhaseStep = 1ULL << kPhaseShift;

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

} // namespace citor::detail
