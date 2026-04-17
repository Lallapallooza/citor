#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <new>

#include "citor/detail/job_descriptor.h"

namespace citor::detail {

/// Centralized dynamic-counter execution for `Balance::DynamicChunked`.
///
/// Workers race on a single relaxed atomic (`desc.nextBlock`) to claim the next block id. The
/// relaxed memory order is sufficient: block-id uniqueness is the only invariant, and visibility
/// of the descriptor's body and range fields was already established by the worker's acquire-load
/// on `PoolControl::generation` matched with the producer's release-store on
/// `PoolControl::activeJob`.
///
/// Cancellation and exception handling mirror `runStaticPartition`: stopped tokens halt new-block
/// admission, and the first thrown exception is funneled into `desc.firstException`.
///
/// desc Job descriptor; the dynamic counter and exception capture slots are mutated.
/// rank Worker's slot index in the dispatch.
inline void runDynamicCounter(JobDescriptor &desc, std::uint32_t rank) noexcept {
  const std::size_t blockCount = desc.blockCount;
  const std::size_t chunk = desc.chunk;
  const std::size_t first = desc.first;
  const std::size_t last = desc.last;

  while (true) {
    if (desc.firstException.load(std::memory_order_acquire) != nullptr) {
      return;
    }
    if (desc.token.stop_requested()) {
      return;
    }
    const std::uint64_t blockId = desc.nextBlock.fetch_add(1, std::memory_order_relaxed);
    if (blockId >= blockCount) {
      return;
    }
    const std::size_t lo = first + (static_cast<std::size_t>(blockId) * chunk);
    const std::size_t hi = std::min(lo + chunk, last);
    if (lo >= hi) {
      continue;
    }
    try {
      desc.body(lo, hi);
    } catch (...) {
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
