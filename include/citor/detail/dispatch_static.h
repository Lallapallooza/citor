#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <new>

#include "citor/detail/job_descriptor.h"

namespace citor::detail {

/// Worker-strided block execution for `Balance::StaticUniform`.
///
/// Worker |rank| handles blocks `rank, rank + participants, rank + 2 * participants, ...` until
/// the block id meets or exceeds `desc.blockCount`. Each block's `[lo, hi)` range is computed from
/// `(blockId, chunk, last)` with no atomic on the hot path.
///
/// The function never allocates, never throws (exceptions thrown by the body are funneled into
/// `desc.firstException`), and returns once the worker has consumed every block in its stride. The
/// caller is responsible for publishing the worker's done-epoch after this returns.
///
/// Cancellation: when the descriptor's token has been stopped, the worker stops admitting new
/// blocks. Already-running blocks complete; the contract is "no new work after stop".
///
/// desc Job descriptor; mutated only via the relaxed contention slots and via the exception
///             capture CAS.
/// rank Worker's slot index in the dispatch (`0` for the producer; `[1, participants)` for
///             background workers).
inline void runStaticPartition(JobDescriptor &desc, std::uint32_t rank) noexcept {
  const std::size_t participants = desc.participants;
  const std::size_t blockCount = desc.blockCount;
  const std::size_t chunk = desc.chunk;
  const std::size_t first = desc.first;
  const std::size_t last = desc.last;

  for (std::size_t blockId = rank; blockId < blockCount; blockId += participants) {
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
      // First-exception capture: only the first worker to observe a null slot wins. Allocation
      // failure here is deliberately fatal -- the surrounding terminate handler is preferable to
      // silently dropping the exception.
      auto *eptr = new (std::nothrow) std::exception_ptr(std::current_exception());
      if (eptr == nullptr) {
        std::terminate();
      }
      // The CAS expected slot must be non-const to be used with compare_exchange; tidy's
      // suggestion does not apply since the value is read-modified by the operation.
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

/// Typed slot-0 partition runner: same as `runStaticPartition` but calls `fn(lo, hi)`
///        directly instead of going through `desc.body`'s `FunctionRef` indirection.
///
/// Used by the producer's slot-0 path inside `dispatchOneStaticLockedBody` when the caller has
/// the body's static type available (parallelFor / parallelReduce / bulkForQueries instantiations
/// pass the lambda type as a template parameter). Workers still go through `desc.body` because
/// they cannot recover the body's type from the published descriptor.
///
/// Eliminates the `call *<func_ptr>` indirect call (a large slice of
/// `runStaticPartition`'s samples on this instruction) for the producer's slot-0 body.
template <class FOp>
inline void runStaticPartitionTyped(JobDescriptor &desc, std::uint32_t rank, FOp &fn) noexcept {
  const std::size_t participants = desc.participants;
  const std::size_t blockCount = desc.blockCount;
  const std::size_t chunk = desc.chunk;
  const std::size_t first = desc.first;
  const std::size_t last = desc.last;

  for (std::size_t blockId = rank; blockId < blockCount; blockId += participants) {
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

/// Monomorphized worker runner for parallelFor<HintsT, F> static-uniform dispatch.
///
/// Calls F directly via `desc->fnPtr` (no FunctionRef indirect). Compile-time elides:
///   - `desc->token.stop_requested()` when HintsT::cancellationChecks is false
///   - try/catch frame when F is nothrow_invocable
///   - per-block exception load when F is nothrow_invocable
/// Per-worker, per-(HintsT, F) cached job parameters. Same-command reuse: when the producer
/// detects an identical key vs the previous dispatch, it bumps only mailbox.gen without
/// re-publishing desc fields. Worker reads cached values from TLS instead of the producer's
/// TLS desc cache line, eliminating that line transit on the hot path.
struct alignas(kCacheLine) CachedStaticForJob {
  std::size_t participants{0};
  std::size_t blockCount{0};
  std::size_t chunk{0};
  std::size_t first{0};
  std::size_t last{0};
  void *fnPtr{nullptr};
  bool primed{false};
};

template <class HintsT, class F>
inline CachedStaticForJob &cachedStaticForSlot() noexcept {
  static thread_local CachedStaticForJob cache;
  return cache;
}

template <class HintsT, class F>
inline void typedStaticUniformWorkerEntry(JobDescriptor *desc, std::uint32_t rankPacked) noexcept {
  // High bit of rankPacked encodes producer's "reuse" hint: when set, the producer's TLS key
  // matched the previous dispatch (same fn / range / chunk / participants), and the worker can
  // safely skip reading desc fields entirely -- they're identical to the cached values.
  // Otherwise the worker reads desc and refreshes its TLS cache.
  constexpr std::uint32_t kReuseFlag = 0x80000000U;
  const bool reuse = (rankPacked & kReuseFlag) != 0U;
  const std::uint32_t rank = rankPacked & ~kReuseFlag;
  auto &cache = cachedStaticForSlot<HintsT, F>();
  if (!reuse || !cache.primed) {
    cache.participants = desc->participants;
    cache.blockCount = desc->blockCount;
    cache.chunk = desc->chunk;
    cache.first = desc->first;
    cache.last = desc->last;
    cache.fnPtr = desc->fnPtr;
    cache.primed = true;
  }
  const std::size_t participants = cache.participants;
  const std::size_t blockCount = cache.blockCount;
  const std::size_t chunk = cache.chunk;
  const std::size_t first = cache.first;
  const std::size_t last = cache.last;
  auto &fn = *static_cast<F *>(cache.fnPtr);

  constexpr bool kHasCancellation = requires { HintsT::cancellationChecks; };
  constexpr bool kCancellationActive = []() {
    if constexpr (kHasCancellation) {
      return HintsT::cancellationChecks;
    }
    return true;
  }();
  constexpr bool kBodyNoexcept = std::is_nothrow_invocable_v<F &, std::size_t, std::size_t>;

  for (std::size_t blockId = rank; blockId < blockCount; blockId += participants) {
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
        if (!desc->firstException.compare_exchange_strong(
                expected, eptr, std::memory_order_release, std::memory_order_acquire)) {
          delete eptr;
        } else {
          desc->exceptionWorkerId.store(rank, std::memory_order_release);
        }
        return;
      }
    }
  }
}

} // namespace citor::detail
