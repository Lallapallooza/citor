#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <new>
#include <type_traits>

#include "citor/detail/job_descriptor.h"
#include "citor/detail/worker_state.h"
#include "citor/hints.h"

namespace citor::detail {

/// CAS-claim a generation on a worker's `claimedAt` slot.
///
/// Returns true when this caller successfully bumped `slot.claimedAt` from a value `<currentGen`
/// up to `currentGen`. Returns false when the slot is already at `>= currentGen` (i.e. another
/// caller -- producer cold-collapse vs. the worker -- has already claimed this rank for the
/// current dispatch).
[[gnu::always_inline]] inline bool tryClaimRank(WorkerState &slot,
                                                std::uint64_t currentGen) noexcept {
  std::uint64_t expected = slot.claimedAt.load(std::memory_order_acquire);
  while (expected < currentGen) {
    if (slot.claimedAt.compare_exchange_weak(expected, currentGen, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
      return true;
    }
  }
  return false;
}

/// Sentinel returned by `BlockClaim::next` when no more blocks are available.
inline constexpr std::size_t kNoBlock = static_cast<std::size_t>(-1);

/// Per-balance block-claim policy.
///
/// The dispatch worker entry and slot-0 runner share a single iteration loop that calls
/// `BlockClaim<B>::next(...)` to obtain the next block id. Balance variants differ only in this
/// one function: `StaticUniform` rank-strides; `DynamicChunked` races on the shared atomic.
///
/// Returning `kNoBlock` ends the loop. The hybrid Static/Dynamic distinction the engine had
/// before this refactor is now expressed entirely as the choice of policy.
template <Balance B> struct BlockClaim;

/// Static rank-strided iteration: rank R runs blocks `{R, R+P, R+2P, ...}` until past
/// `blockCount`. No atomic on the hot path.
template <> struct BlockClaim<Balance::StaticUniform> {
  [[gnu::always_inline]] static std::size_t next(JobDescriptor *desc, std::uint32_t rank,
                                                 std::size_t prevIdx) noexcept {
    const std::size_t participants = desc->participants;
    const std::size_t blockCount = desc->blockCount;
    const std::size_t cand = (prevIdx == kNoBlock) ? rank : prevIdx + participants;
    return cand < blockCount ? cand : kNoBlock;
  }
};

/// Dynamic atomic-counter iteration: workers race `desc->nextBlock.fetch_add` for blocks.
/// The dispatcher initialises `nextBlock` to a starting offset (either `0` for pure dynamic
/// or `participants` for the strided-Phase-A + atomic-tail hybrid the bench uses).
template <> struct BlockClaim<Balance::DynamicChunked> {
  [[gnu::always_inline]] static std::size_t next(JobDescriptor *desc, std::uint32_t rank,
                                                 std::size_t prevIdx) noexcept {
    const std::size_t blockCount = desc->blockCount;
    // Phase A: when this is the first call (`prevIdx == kNoBlock`) and `rank < blockCount`,
    // the rank claims its statically-assigned block (the index equal to its rank). This
    // mirrors `StaticUniform`'s first iteration on non-oversubscribed dispatches: with
    // `blockCount <= participants` every rank takes one block in Phase A, no fetch_add fires,
    // and the dispatch incurs zero atomic-counter contention.
    const std::size_t participants = desc->participants;
    if (prevIdx == kNoBlock && rank < blockCount && rank < participants) {
      return rank;
    }
    // Phase B: drain the atomic tail. The dispatcher initialised `nextBlock` to `participants`
    // when the partition is oversubscribed, so a fetch_add returning `< blockCount` yields a
    // block id outside the strided coverage of Phase A. On non-oversubscribed dispatches this
    // returns `>= blockCount` immediately and exits without contention.
    if (blockCount <= participants) {
      return kNoBlock;
    }
    const std::uint64_t claimed = desc->nextBlock.fetch_add(1, std::memory_order_relaxed);
    return claimed < blockCount ? static_cast<std::size_t>(claimed) : kNoBlock;
  }
};

/// Untyped block iteration loop shared by `runStaticPartition` and `runDynamicCounter`.
///
/// Calls `desc.body(lo, hi)` (FunctionRef indirect) for each block returned by
/// `BlockClaim<B>::next`. Used when the body's static type is not available at the call site
/// (worker-side fallbacks; chain stages with type-erased bodies).
template <Balance B> inline void runPartition(JobDescriptor &desc, std::uint32_t rank) noexcept {
  const std::size_t chunk = desc.chunk;
  const std::size_t first = desc.first;
  const std::size_t last = desc.last;

  for (std::size_t blockId = kNoBlock;
       (blockId = BlockClaim<B>::next(&desc, rank, blockId)) != kNoBlock;) {
    if (desc.firstException.load(std::memory_order_acquire) != nullptr) [[unlikely]] {
      return;
    }
    if (desc.token.stop_requested()) [[unlikely]] {
      return;
    }
    const std::size_t lo = first + (blockId * chunk);
    const std::size_t hi = std::min(lo + chunk, last);
    try {
      desc.body(lo, hi);
    } catch (...) {
      auto *eptr = new (std::nothrow) std::exception_ptr(std::current_exception());
      if (eptr == nullptr) {
        std::terminate();
      }
      std::exception_ptr *expected = nullptr; // NOLINT(misc-const-correctness)
      if (!desc.firstException.compare_exchange_strong(expected, eptr, std::memory_order_release,
                                                       std::memory_order_acquire)) {
        delete eptr;
      } else {
        desc.exceptionWorkerId.store(rank, std::memory_order_release);
      }
      return;
    }
  }
}

template <Balance B>
[[gnu::always_inline]] inline std::size_t
nextTypedBlock(JobDescriptor &desc, std::size_t blockCount, std::size_t participants,
               std::uint32_t rank, std::size_t prevIdx) noexcept {
  if constexpr (B == Balance::StaticUniform) {
    const std::size_t cand = (prevIdx == kNoBlock) ? rank : prevIdx + participants;
    return cand < blockCount ? cand : kNoBlock;
  } else {
    if (prevIdx == kNoBlock && rank < blockCount && rank < participants) {
      return rank;
    }
    if (blockCount <= participants) {
      return kNoBlock;
    }
    const std::uint64_t claimed = desc.nextBlock.fetch_add(1, std::memory_order_relaxed);
    return claimed < blockCount ? static_cast<std::size_t>(claimed) : kNoBlock;
  }
}

/// Static-balance untyped runner: kept as a name alias for legacy callers.
inline void runStaticPartition(JobDescriptor &desc, std::uint32_t rank) noexcept {
  runPartition<Balance::StaticUniform>(desc, rank);
}

/// Dynamic-balance untyped runner: kept as a name alias for legacy callers.
inline void runDynamicCounter(JobDescriptor &desc, std::uint32_t rank) noexcept {
  // Cold-collapse CAS-claim: producer's join-wait may race the worker for this rank's
  // claim slot. Loser returns; producer (or the winning worker) is responsible for
  // stamping `mailbox = doneSentinel` so the join rendezvous fires.
  if (desc.workerStateBase != nullptr) [[unlikely]] {
    auto *wsBase = static_cast<WorkerState *>(desc.workerStateBase);
    if (!tryClaimRank(wsBase[rank], desc.generation)) {
      return;
    }
  }
  runPartition<Balance::DynamicChunked>(desc, rank);
}

/// Typed slot-0 partition runner: same as `runPartition` but calls `fn(lo, hi)` directly
///        instead of going through `desc.body`'s `FunctionRef` indirection.
///
/// Used by the producer's slot-0 path inside `dispatchOneStaticLockedBody` when the caller has
/// the body's static type available (parallelFor / parallelReduce / bulkForQueries
/// instantiations pass the lambda type as a template parameter).
template <Balance B, class FOp>
inline void runPartitionTyped(JobDescriptor &desc, std::uint32_t rank, FOp &fn) noexcept {
  const std::size_t chunk = desc.chunk;
  const std::size_t first = desc.first;
  const std::size_t last = desc.last;
  const std::size_t blockCount = desc.blockCount;
  const std::size_t participants = desc.participants;

  for (std::size_t blockId = kNoBlock;
       (blockId = nextTypedBlock<B>(desc, blockCount, participants, rank, blockId)) != kNoBlock;) {
    if (desc.firstException.load(std::memory_order_acquire) != nullptr) [[unlikely]] {
      return;
    }
    if (desc.token.stop_requested()) [[unlikely]] {
      return;
    }
    const std::size_t lo = first + (blockId * chunk);
    const std::size_t hi = std::min(lo + chunk, last);
    try {
      fn(lo, hi);
    } catch (...) {
      auto *eptr = new (std::nothrow) std::exception_ptr(std::current_exception());
      if (eptr == nullptr) {
        std::terminate();
      }
      std::exception_ptr *expected = nullptr; // NOLINT(misc-const-correctness)
      if (!desc.firstException.compare_exchange_strong(expected, eptr, std::memory_order_release,
                                                       std::memory_order_acquire)) {
        delete eptr;
      } else {
        desc.exceptionWorkerId.store(rank, std::memory_order_release);
      }
      return;
    }
  }
}

/// Legacy alias for the typed Static slot-0 runner. Existing call sites use this name.
template <class FOp>
[[gnu::always_inline]] inline void runStaticPartitionTyped(JobDescriptor &desc, std::uint32_t rank,
                                                           FOp &fn) noexcept {
  runPartitionTyped<Balance::StaticUniform>(desc, rank, fn);
}

template <class HintsT, class FOp>
[[gnu::always_inline]] inline void
runSingleRankBlockTyped(JobDescriptor &desc, std::uint32_t rank, FOp &fn, std::size_t blockCount,
                        std::size_t chunk, std::size_t first, std::size_t last) noexcept {
  if (rank >= blockCount) {
    return;
  }
  constexpr bool kHasCancellation = requires { HintsT::cancellationChecks; };
  constexpr bool kCancellationActive = []() {
    if constexpr (kHasCancellation) {
      return HintsT::cancellationChecks;
    }
    return true;
  }();
  constexpr bool kBodyNoexcept = std::is_nothrow_invocable_v<FOp &, std::size_t, std::size_t>;

  if constexpr (!kBodyNoexcept) {
    if (desc.firstException.load(std::memory_order_acquire) != nullptr) [[unlikely]] {
      return;
    }
  }
  if constexpr (kCancellationActive) {
    if (desc.token.stop_requested()) [[unlikely]] {
      return;
    }
  }
  const std::size_t lo = first + (static_cast<std::size_t>(rank) * chunk);
  const std::size_t hi = std::min(lo + chunk, last);
  if constexpr (kBodyNoexcept) {
    fn(lo, hi);
  } else {
    try {
      fn(lo, hi);
    } catch (...) {
      auto *eptr = new (std::nothrow) std::exception_ptr(std::current_exception());
      if (eptr == nullptr) {
        std::terminate();
      }
      std::exception_ptr *expected = nullptr; // NOLINT(misc-const-correctness)
      if (!desc.firstException.compare_exchange_strong(expected, eptr, std::memory_order_release,
                                                       std::memory_order_acquire)) {
        delete eptr;
      } else {
        desc.exceptionWorkerId.store(rank, std::memory_order_release);
      }
    }
  }
}

template <Balance B, class HintsT, class FOp>
[[gnu::always_inline]] inline void runPartitionTypedHinted(JobDescriptor &desc, std::uint32_t rank,
                                                           FOp &fn) noexcept {
  const std::size_t chunk = desc.chunk;
  const std::size_t first = desc.first;
  const std::size_t last = desc.last;
  const std::size_t blockCount = desc.blockCount;
  const std::size_t participants = desc.participants;

  if (blockCount <= participants) {
    runSingleRankBlockTyped<HintsT>(desc, rank, fn, blockCount, chunk, first, last);
    return;
  }

  constexpr bool kHasCancellation = requires { HintsT::cancellationChecks; };
  constexpr bool kCancellationActive = []() {
    if constexpr (kHasCancellation) {
      return HintsT::cancellationChecks;
    }
    return true;
  }();
  constexpr bool kBodyNoexcept = std::is_nothrow_invocable_v<FOp &, std::size_t, std::size_t>;

  for (std::size_t blockId = kNoBlock;
       (blockId = nextTypedBlock<B>(desc, blockCount, participants, rank, blockId)) != kNoBlock;) {
    if constexpr (!kBodyNoexcept) {
      if (desc.firstException.load(std::memory_order_acquire) != nullptr) [[unlikely]] {
        return;
      }
    }
    if constexpr (kCancellationActive) {
      if (desc.token.stop_requested()) [[unlikely]] {
        return;
      }
    }
    const std::size_t lo = first + (blockId * chunk);
    const std::size_t hi = std::min(lo + chunk, last);
    if constexpr (kBodyNoexcept) {
      fn(lo, hi);
    } else {
      try {
        fn(lo, hi);
      } catch (...) {
        auto *eptr = new (std::nothrow) std::exception_ptr(std::current_exception());
        if (eptr == nullptr) {
          std::terminate();
        }
        std::exception_ptr *expected = nullptr; // NOLINT(misc-const-correctness)
        if (!desc.firstException.compare_exchange_strong(expected, eptr, std::memory_order_release,
                                                         std::memory_order_acquire)) {
          delete eptr;
        } else {
          desc.exceptionWorkerId.store(rank, std::memory_order_release);
        }
        return;
      }
    }
  }
}

/// Per-(HintsT, F) cached job parameters for the typed worker entry. Same-command reuse: when
/// the producer detects an identical key vs the previous dispatch, it bumps only mailbox.gen
/// without re-publishing desc fields. Worker reads cached values from TLS instead of the
/// producer's TLS desc cache line, eliminating that line transit on the hot path.
struct alignas(kCacheLine) CachedTypedForJob {
  std::size_t blockCount{0};
  std::size_t participants{0};
  std::size_t chunk{0};
  std::size_t first{0};
  std::size_t last{0};
  void *fnPtr{nullptr};
  void *workerStateBase{nullptr};
  bool primed{false};
};

template <class HintsT, class F> inline CachedTypedForJob &cachedTypedForSlot() noexcept {
  static thread_local CachedTypedForJob cache;
  return cache;
}

/// Monomorphized typed worker entry, parameterized by balance.
///
/// One template body covers both `Balance::StaticUniform` and `Balance::DynamicChunked`. The
/// only per-balance difference -- which block id the worker runs next -- is delegated to the
/// `BlockClaim<B>` policy. Compile-time elides:
///   - `desc->token.stop_requested()` when `HintsT::cancellationChecks` is false
///   - try/catch frame when `F` is nothrow_invocable
///   - per-block exception load when `F` is nothrow_invocable
///
/// The `desc->nextBlock` value is read by `BlockClaim<DynamicChunked>::next` directly off the
/// descriptor, NOT from the TLS cache -- it is the only mutable contended field per dispatch.
/// Static iteration uses `desc->participants`, which IS hoisted through the cached slot since
/// it is loop-invariant across blocks within a dispatch.
template <Balance B, class HintsT, class F>
inline void typedWorkerEntry(JobDescriptor *desc, std::uint32_t rankPacked,
                             std::uint64_t generation) noexcept {
  // High bit of rankPacked encodes producer's "reuse" hint: when set, the producer's TLS key
  // matched the previous dispatch (same fn / range / chunk / participants), and the worker
  // can safely skip reading desc fields entirely -- they're identical to the cached values.
  constexpr std::uint32_t kReuseFlag = 0x80000000U;
  constexpr std::uint32_t kSkipClaimFlag = 0x40000000U;
  const bool reuse = (rankPacked & kReuseFlag) != 0U;
  const bool skipClaim = (rankPacked & kSkipClaimFlag) != 0U;
  const std::uint32_t rank = rankPacked & ~(kReuseFlag | kSkipClaimFlag);
  auto &cache = cachedTypedForSlot<HintsT, F>();
  if (!reuse || !cache.primed) [[unlikely]] {
    cache.blockCount = desc->blockCount;
    cache.participants = desc->participants;
    cache.chunk = desc->chunk;
    cache.first = desc->first;
    cache.last = desc->last;
    cache.fnPtr = desc->fnPtr;
    cache.workerStateBase = desc->workerStateBase;
    cache.primed = true;
  }
  // Cold-collapse CAS-claim: race the producer's join-wait fallback for the right to run rank
  // R's blocks. The loser returns immediately. The cache refresh above runs unconditionally so
  // workers that lose the race still have a fresh cache for the next dispatch -- otherwise a
  // string of cold dispatches with reuse=true would leave the worker's TLS cache stale forever.
  if (!skipClaim && cache.workerStateBase != nullptr) [[unlikely]] {
    auto *wsBase = static_cast<WorkerState *>(cache.workerStateBase);
    if (!tryClaimRank(wsBase[rank], generation)) {
      return;
    }
  }
  const std::size_t chunk = cache.chunk;
  const std::size_t first = cache.first;
  const std::size_t last = cache.last;
  const std::size_t blockCount = cache.blockCount;
  const std::size_t participants = cache.participants;
  auto &fn = *static_cast<F *>(cache.fnPtr);

  constexpr bool kHasCancellation = requires { HintsT::cancellationChecks; };
  constexpr bool kCancellationActive = []() {
    if constexpr (kHasCancellation) {
      return HintsT::cancellationChecks;
    }
    return true;
  }();
  constexpr bool kBodyNoexcept = std::is_nothrow_invocable_v<F &, std::size_t, std::size_t>;

  if (blockCount <= participants) {
    runSingleRankBlockTyped<HintsT>(*desc, rank, fn, blockCount, chunk, first, last);
    return;
  }

  for (std::size_t blockId = kNoBlock;
       (blockId = nextTypedBlock<B>(*desc, blockCount, participants, rank, blockId)) != kNoBlock;) {
    if constexpr (!kBodyNoexcept) {
      if (desc->firstException.load(std::memory_order_acquire) != nullptr) [[unlikely]] {
        return;
      }
    }
    if constexpr (kCancellationActive) {
      if (desc->token.stop_requested()) [[unlikely]] {
        return;
      }
    }
    const std::size_t lo = first + (blockId * chunk);
    const std::size_t hi = std::min(lo + chunk, last);
    if constexpr (kBodyNoexcept) {
      fn(lo, hi);
    } else {
      try {
        fn(lo, hi);
      } catch (...) {
        auto *eptr = new (std::nothrow) std::exception_ptr(std::current_exception());
        if (eptr == nullptr) {
          std::terminate();
        }
        std::exception_ptr *expected = nullptr; // NOLINT(misc-const-correctness)
        if (!desc->firstException.compare_exchange_strong(expected, eptr, std::memory_order_release,
                                                          std::memory_order_acquire)) {
          delete eptr;
        } else {
          desc->exceptionWorkerId.store(rank, std::memory_order_release);
        }
        return;
      }
    }
  }
}

/// Legacy alias for the typed Static worker entry. Existing call sites that reference this
/// name as the `desc.workerEntry` function pointer continue to work.
template <class HintsT, class F>
inline void typedStaticUniformWorkerEntry(JobDescriptor *desc, std::uint32_t rankPacked,
                                          std::uint64_t generation) noexcept {
  typedWorkerEntry<Balance::StaticUniform, HintsT, F>(desc, rankPacked, generation);
}

/// Typed worker entry for `Balance::DynamicChunked`. Used by parallelFor's Dynamic dispatcher
/// path; reuses every optimization on the typed path (TLS cache, reuse-bit, cold-collapse,
/// monomorphized direct call) via the unified `typedWorkerEntry` template.
template <class HintsT, class F>
inline void typedDynamicChunkedWorkerEntry(JobDescriptor *desc, std::uint32_t rankPacked,
                                           std::uint64_t generation) noexcept {
  typedWorkerEntry<Balance::DynamicChunked, HintsT, F>(desc, rankPacked, generation);
}

} // namespace citor::detail
