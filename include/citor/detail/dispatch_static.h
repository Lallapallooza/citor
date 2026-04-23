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

} // namespace citor::detail
