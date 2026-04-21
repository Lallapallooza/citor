#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>

#include "citor/cancellation.h"
#include "citor/function_ref.h"
#include "citor/hints.h"

namespace citor::detail {

/// Stack-resident shared state for one in-flight `citor::ThreadPool::forkJoin` call.
///
/// The producer constructs a `ForkJoinState` on its stack, populates the immutable shape, and
/// publishes a non-owning pointer to every participating worker via the worker-loop dispatch
/// descriptor. Workers consume tasks from their own Chase-Lev deque, falling back to victim
/// stealing when their deque drains. The producer participates as slot 0 and joins on the
/// `pendingTasks` countdown reaching zero.
///
/// Layout invariants:
/// - `pendingTasks` lives on its own line; it is the single contended atomic on the dispatch
///   completion path. Workers `fetch_sub(1)` on it after retiring a task so the producer's
///   spin-then-park loop can detect zero without reading any per-worker slot.
/// - `forkJoinCancelled` lives on its own line so cancellation broadcast does not interfere with
///   the per-task hot path. Workers acquire-read this flag at task-boundary chunks; a stopped
///   token causes any victim probe to stop emitting fresh task descriptors.
/// - `firstException` is on its own line; the CAS-from-null path is cold.
///
/// Padding-suppression note: the layout keeps every contended atomic on its own
/// `kCacheLine`-sized line, so the analyser's "excessive padding" warning is the design trade-off
/// we want -- false-sharing avoidance over byte-tight packing.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct ForkJoinState {
  /// Number of participants (producer + background workers) collaborating in the call.
  std::uint32_t participants = 0;

  /// CCD index for each participant slot; used by the victim-selection RNG to bias stealing
  /// toward same-CCD victims when `Affinity::CcdLocal` is requested. Sized `participants`.
  const std::uint32_t *ccdOfSlot = nullptr;

  /// CCD-local affinity flag derived from the call's `HintsT::affinity`. When `true`, the
  /// victim-selection probe order biases toward same-CCD workers; when `false`, the probe
  /// order is a uniform xorshift random.
  bool preferSameCcd = false;

  /// Cancellation token observed by workers at task-boundary chunks; copied from the producer
  /// when `forkJoin` is invoked. Workers stop emitting fresh tasks once the token is stopped.
  CancellationToken token;

  /// Outstanding task count.
  ///
  /// Producer initializes with the root task pack size (typically the variadic count of `fns...`).
  /// Each task increments by `1` per child it spawns recursively, then decrements by `1` after
  /// its body retires. The producer joins on `pendingTasks == 0`.
  alignas(kCacheLine) std::atomic<std::int64_t> pendingTasks{0};

  /// Cancellation flag broadcast by the producer's cancellation observer.
  ///
  /// Set once by the producer when the call's `CancellationToken` transitions to stopped, or
  /// by any participant that observed an exception (so the stop response is uniform). Worker
  /// task bodies read this flag at chunk boundaries and either skip or short-circuit, relying on
  /// the synchronous join contract of `ThreadPool::forkJoin` to rendezvous with the producer.
  alignas(kCacheLine) std::atomic<std::uint32_t> forkJoinCancelled{0};

  /// First-exception capture slot shared across all participants.
  ///
  /// Workers `compare_exchange` this from null to a heap-allocated `std::exception_ptr` to record
  /// the first failure deterministically; subsequent throws drop. The producer reads the slot
  /// after joining and rethrows if non-null. Allocation only happens on the cold throw path.
  alignas(kCacheLine) std::atomic<std::exception_ptr *> firstException{nullptr};
};

/// Type-erased recursive task descriptor stored in a worker's Chase-Lev deque.
///
/// The deque holds raw `void *` payloads and the worker dereferences them as `Task *` pointers
/// back to descriptors that live either on the producer's stack (root tasks) or in a per-worker
/// arena (recursive children spawned during a forkJoin body). The descriptor's `body` is a
/// `FunctionRef` over `void()`, kept by the call site for the duration of the synchronous call.
///
/// The owning lifetime of `Task` is tied to the synchronous `ThreadPool::forkJoin` call: once
/// the producer's join observes `pendingTasks == 0`, every task body has retired, no worker
/// holds a pointer to any descriptor, and the producer's stack frame can unwind safely.
struct Task {
  /// Non-owning reference to the user's callable. The callable lives in the producer or in a
  /// recursive task arena for the duration of the synchronous call.
  FunctionRef<void()> body;

  /// Pointer to the call's shared state, used by the worker to record exceptions and decrement
  /// the outstanding-task counter after the body retires.
  ForkJoinState *state = nullptr;
};

} // namespace citor::detail
