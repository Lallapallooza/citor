#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>

#include "citor/cancellation.h"
#include "citor/function_ref.h"
#include "citor/hints.h"

namespace citor::detail {

/// Stack-resident job-publish protocol used by every fan-out primitive.
///
/// A `JobDescriptor` lives on the producer's stack for the duration of one synchronous primitive
/// call. The producer fills it before publishing the new generation; workers read it via the
/// acquire-load on `PoolControl::activeJob` after observing the matching generation. The
/// descriptor is single-writer (producer fills, then publishes), many-reader (workers consume).
///
/// Layout:
/// - The first cache line holds the immutable descriptor body (range bounds, chunk shape,
///   participants, balance / priority, body / token). Workers acquire-load these once after
///   observing the matching generation.
/// - The contended atomics (`nextBlock`, `firstException`, `exceptionWorkerId`) sit on dedicated
///   `kCacheLine`-sized lines so concurrent dynamic-counter increments and exception CAS attempts
///   do not invalidate the immutable body.
///
/// The descriptor's `body` is a `FunctionRef` pointing into a closure that lives on the producer's
/// stack. Because every primitive in v1 is synchronous (the producer joins before returning), the
/// closure outlives the descriptor by construction.
///
/// The padding overhead trades several hundred bytes of stack against MESI cache-coherency
/// traffic on the contended atomics, which is the dominant hot-path cost.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct alignas(kCacheLine) JobDescriptor {
  /// Generation value the producer published this descriptor under; workers stamp `doneEpoch`
  /// with the same value once their share completes so the producer's join can rendezvous.
  std::uint64_t generation = 0;

  /// Inclusive start of the iteration range.
  std::size_t first = 0;

  /// Exclusive end of the iteration range.
  std::size_t last = 0;

  /// Static block grain. Must be at least 1; a primitive computes the value once before
  /// publishing.
  std::size_t chunk = 0;

  /// Total number of blocks the producer carved the range into.
  std::size_t blockCount = 0;

  /// Number of participants (producer + background workers) the dispatch was sized against.
  std::uint32_t participants = 0;

  /// Compile-time balance choice serialized into the descriptor for the runtime path; the
  /// member-template path bakes the choice into the call shape via `if constexpr`.
  Balance balance = Balance::StaticUniform;

  /// Compile-time priority choice serialized into the descriptor for the runtime path.
  Priority priority = Priority::Throughput;

  /// Non-owning reference to the user's closure. Lives on the producer's stack for the duration
  /// of the synchronous primitive call.
  FunctionRef<void(std::size_t, std::size_t)> body;

  /// Cancellation token observed by workers at chunk boundaries when the call site requested it.
  CancellationToken token;

  /// Centralized counter used by `Balance::DynamicChunked`; workers race on a relaxed
  /// `fetch_add(1)` to claim the next block id. Sits on its own line so contention here does not
  /// invalidate the immutable descriptor body.
  alignas(kCacheLine) std::atomic<std::uint64_t> nextBlock{0};

  /// First-exception capture slot. Workers `compare_exchange` this from null to a heap-allocated
  /// `std::exception_ptr` to record the first failure deterministically; subsequent throws drop.
  /// Worker rank that filled the slot is captured alongside so the slot's line carries both
  /// values that change together.
  alignas(kCacheLine) std::atomic<std::exception_ptr *> firstException{nullptr};

  /// Worker rank that filled `firstException`; used to break ties between simultaneous throws.
  std::atomic<std::uint32_t> exceptionWorkerId{0};
};

} // namespace citor::detail
