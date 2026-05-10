#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <utility>

#include "citor/hints.h"

namespace citor::detail {

/// Per-worker per-stage completion slot used by every barrier kind of
///        `citor::ThreadPool::parallelChain`.
///
/// Each slot lives on its own `citor::kCacheLine` -sized line so a worker's
/// release-store on `done` for one stage cannot invalidate a neighbouring slot
/// the producer or downstream worker is acquiring. The owning worker writes
/// `done` exactly once per stage (or zero times when its slice is empty) via
/// release; downstream consumers read it via acquire to confirm the upstream
/// worker's slice for that stage has retired.
///
/// Padding-suppression note: the layout keeps the atomic on its own
/// `kCacheLine`-sized line, so the analyser's "excessive padding" warning is
/// the design trade-off we want -- false-sharing avoidance over byte-tight
/// packing.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct alignas(kCacheLine) ChainDoneSlot {
  /// Stage epoch the worker last finished. Downstream consumer waits until
  /// `done >= stageIdx + 1` before admitting the next stage's body for this
  /// worker's slot.
  std::atomic<std::uint64_t> done{0};
};

/// Per-stage dynamic-chain claim counter.
///
/// Dynamic-chain mode allocates one counter per stage before dispatch.
/// Participants claim blocks from the current stage's counter, so advancing to
/// the next stage does not require slot 0 to reset a shared counter and publish
/// a separate ready epoch. Each counter lives on its own line because it is the
/// only contended atomic while that stage is running.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct alignas(kCacheLine) ChainDynamicStageCounter {
  /// Next dynamic block id to claim for this stage.
  std::atomic<std::size_t> next{0};
};

/// Stack-resident state shared by the producer and background workers across
/// all stages of a
///        single `parallelChain` call.
///
/// Layout invariants:
/// - `chainCancelled` lives on its own line so cancellation broadcast does not
/// interfere with the
///   producer's hot path.
/// - `done[w]` lives on a dedicated line via `ChainDoneSlot` so adjacent
/// workers' release-stores
///   do not contend for the same cache line.
///
/// The state itself lives on the producer's stack across a `parallelChain`
/// call. The trailing per-worker `done` slots are NOT owned by `ChainState`:
/// the pool pre-allocates a contiguous `ChainDoneSlot` block once at
/// construction time (sized `participants()`), and every chain call borrows
/// that block via `doneSlots`. The producer zero-resets each slot at entry to
/// honour the chain's "fresh epoch per call" contract; this avoids `operator
/// new` / `operator delete` on the dispatch hot path, which the spec's <= 2 us
/// target rules out.
///
/// The Global rendezvous is fully decentralized: every slot (including slot 0)
/// stamps `done[slot] = s + 1` (release) after running stage `s`, and every
/// slot's pre-stage barrier is an independent scan over `done[w] >= s + 1` for
/// every other slot `w`. No participant acts as a serial gate; the wait loops
/// execute in parallel across all workers, mirroring the `runPlex` per-slot
/// done-epoch pattern documented in `plex_state.h`. The cancellation handshake
/// is encoded by stamping `done = nStages` on the cancelled slot's line, which
/// satisfies any active stage's wait condition unconditionally and lets the
/// spin loop drop a per-iteration `chainCancelled` poll.
///
/// For `BarrierKind::ProducerSerial` the producer runs the serial body alone
/// while non-producer workers spin on slot 0's `done`.
///
/// Padding-suppression note: the layout keeps every contended atomic on its own
/// `kCacheLine`-sized line, so the analyser's "excessive padding" warning is
/// the design trade-off we want -- false-sharing avoidance over byte-tight
/// packing.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct ChainState {
  /// Number of participants (producer + background workers) collaborating in
  /// the chain.
  std::uint32_t participants = 0;

  /// Total number of stages the chain was constructed with; used by workers as
  /// a phase ceiling.
  std::size_t nStages = 0;

  /// Row-range upper bound passed in by the caller; used for slot partitioning.
  std::size_t n = 0;

  /// Cancellation flag flipped by the producer's cancellation observer.
  ///
  /// Worker bodies check this between stages and exit cleanly when set;
  /// cancelled slots stamp `done = nStages` so peers waiting on `done >=
  /// target` for any active stage proceed without needing to poll this flag
  /// inside the spin loop. The flag is release-written by the slot that
  /// observed the cancellation, and acquire-read by other slots at stage
  /// boundaries (not in the spin loop).
  alignas(kCacheLine) std::atomic<std::uint32_t> chainCancelled{0};

  /// First-exception capture slot shared across all participants.
  ///
  /// Workers `compare_exchange` this from null to a heap-allocated
  /// `std::exception_ptr` to record the first failure deterministically;
  /// subsequent throws drop. The producer reads the slot after joining and
  /// rethrows if non-null. Allocation only happens on the cold throw path.
  alignas(kCacheLine) std::atomic<std::exception_ptr *> firstException{nullptr};

  /// Borrowed pointer to the pool's pre-allocated per-worker completion slots.
  ///
  /// The pool owns the storage; the chain call reserves a fresh interval
  /// `[epochBase, epochBase + nStages]` in the pool's monotonically-advancing
  /// epoch counter so successive calls observe disjoint targets without
  /// zero-resetting the slots. Sized `participants` valid elements; reading
  /// past that index is undefined.
  ChainDoneSlot *doneSlots = nullptr;

  /// Borrowed pointer to per-stage dynamic block counters.
  ///
  /// Present only when dynamic-chain mode is active. The producer owns the
  /// stack-resident counter array and zeroes every stage before dispatch, so no
  /// in-flight participant resets counters between stages.
  ChainDynamicStageCounter *dynamicStageCounters = nullptr;

  /// Dynamic-chain chunk size over `[0, n)`.
  std::size_t dynamicChunk = 0;

  /// Dynamic-chain number of chunks over `[0, n)`.
  std::size_t dynamicBlockCount = 0;

  /// Per-call base of the pool's monotonic done-epoch counter.
  ///
  /// Stamps are absolute: `done = epochBase + I + 1` where `I` is the
  /// just-finished stage index. Waits compare against `epochBase + target`. The
  /// producer reserves the interval under the dispatch gate before publishing,
  /// so prior-dispatch values cannot satisfy a current wait. Cancellation
  /// stamps `epochBase + nStages` so peers waiting on any active stage advance.
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

  /// Subscript a dynamic-stage counter by stage index.
  ///
  /// Valid only when dynamic-chain mode is active and `dynamicStageCounters`
  /// points at the producer-owned counter array.
  ///
  /// stage Stage index in `[0, nStages)`.
  /// Reference to the stage's dynamic claim counter.
  [[nodiscard]] ChainDynamicStageCounter &
  dynamicStageCounter(std::size_t stage) const noexcept {
    return dynamicStageCounters[stage];
  }

  /// Compute a slot's contiguous row range over `[0, n)` using static
  /// partitioning.
  ///
  /// The partition is `lo = (n * slot) / participants`, `hi = (n * (slot + 1))
  /// / participants`. Per-stage chunk identity is preserved across stages: a
  /// stage's chunk `c` is the slice
  /// `[lo, hi)` produced for `slot = c`.
  ///
  /// slot Worker slot index in `[0, participants)`.
  /// `(lo, hi)` pair denoting the slot's contiguous range over `[0, n)`.
  [[nodiscard]] std::pair<std::size_t, std::size_t>
  slotRange(std::uint32_t slot) const noexcept {
    __extension__ using u128 = unsigned __int128;
    const auto lo =
        static_cast<std::size_t>((static_cast<u128>(n) * slot) / participants);
    const auto hi = static_cast<std::size_t>(
        (static_cast<u128>(n) * (slot + 1U)) / participants);
    return {lo, hi};
  }

  /// Compute a dynamic-chain block range over `[0, n)`.
  ///
  /// Unlike `slotRange`, block identity is not tied to participant identity.
  /// Faster slots claim additional block ids from that stage's dynamic counter.
  ///
  /// block Dynamic block id in `[0, dynamicBlockCount)`.
  /// `(lo, hi)` pair denoting the block's range over `[0, n)`.
  [[nodiscard]] std::pair<std::size_t, std::size_t>
  dynamicBlockRange(std::size_t block) const noexcept {
    const std::size_t lo = block * dynamicChunk;
    const std::size_t hi = (dynamicChunk < n - lo) ? lo + dynamicChunk : n;
    return {lo, hi};
  }
};

} // namespace citor::detail
