#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>

#include "citor/cancellation.h"
#include "citor/function_ref.h"
#include "citor/hints.h"

namespace citor::detail {

/// Stack-resident shared state for one in-flight `citor::ThreadPool::forkJoin`
/// call.
///
/// The producer constructs a `ForkJoinState` on its stack, populates the
/// immutable shape, and publishes a non-owning pointer to every participating
/// worker via the worker-loop dispatch descriptor. Workers consume tasks from
/// their own Chase-Lev deque, falling back to victim stealing when their deque
/// drains. The producer participates as slot 0 and joins on the `pendingTasks`
/// countdown reaching zero.
///
/// Layout: every field shares one `kCacheLine`-aligned line. Recursive spawn
/// touches the state once per call. A line-per-atomic layout would pay one
/// Read-For-Ownership transaction per cold line, instead of one for the whole
/// struct. False sharing does not bite on the common path. A call writes
/// `forkJoinCancelled` and `firstException` only when it cancels or throws,
/// so the producer's hot `pendingTasks` poll keeps the line stable.
struct alignas(kCacheLine) ForkJoinState {
  /// Number of participants (producer + background workers) collaborating in
  /// the call.
  std::uint32_t participants = 0;

  /// Cancellation flag broadcast by the producer's cancellation observer or by
  /// any participant that observed an exception. A worker acquire-reads it on
  /// entry to each task and skips the body once it is set, so outstanding
  /// tasks retire without spawning children. The steady-state common path
  /// never writes it, so peer `fetch_sub` traffic on `pendingTasks` keeps the
  /// line uncontended.
  std::atomic<std::uint32_t> forkJoinCancelled{0};

  /// CCD index for each participant slot; used by the victim-selection RNG
  /// to bias stealing toward same-CCD victims when
  /// `StealPolicy::ClusterLocal` is requested. Sized `participants`.
  const std::uint32_t *ccdOfSlot = nullptr;

  /// CCD-local steal-policy flag derived from the call's
  /// `HintsT::stealPolicy`. When `true`, the victim-selection probe order
  /// biases toward same-CCD workers; when `false`, the probe order is a
  /// uniform xorshift random.
  bool preferSameCcd = false;

  /// Cancellation token observed by workers at task-boundary chunks; copied
  /// from the producer when `forkJoin` is invoked. Workers stop emitting fresh
  /// tasks once the token is stopped.
  CancellationToken token;

  /// Outstanding task count. Producer initializes with the root task pack size;
  /// each task `fetch_sub(1)` after retiring; the producer's join spins on `<=
  /// 0`.
  std::atomic<std::int64_t> pendingTasks{0};

  /// First-exception capture slot. Workers CAS from null on throw; producer
  /// reads after join. Allocation only happens on the cold throw path;
  /// default-init nullptr keeps the line clean for the no-throw common path so
  /// peer fetch_sub on `pendingTasks` does not bounce against a contended
  /// exception slot.
  std::atomic<std::exception_ptr *> firstException{nullptr};
};

/// Type-erased recursive task descriptor stored in a worker's Chase-Lev deque.
///
/// The deque holds raw `void *` payloads and the worker dereferences them as
/// `Task *` pointers back to descriptors that live either on the producer's
/// stack (root tasks) or in a per-worker arena (recursive children spawned
/// during a forkJoin body). The descriptor's `body` is a `FunctionRef` over
/// `void()`, kept by the call site for the duration of the synchronous call.
///
/// The owning lifetime of `Task` is tied to the synchronous
/// `ThreadPool::forkJoin` call: once the producer's join observes `pendingTasks
/// == 0`, every task body has retired, no worker holds a pointer to any
/// descriptor, and the producer's stack frame can unwind safely.
struct Task {
  /// Non-owning reference to the user's callable. The callable lives in the
  /// producer or in a recursive task arena for the duration of the synchronous
  /// call.
  FunctionRef<void()> body;

  /// Pointer to the call's shared state, used by the worker to record
  /// exceptions and decrement the outstanding-task counter after the body
  /// retires.
  ForkJoinState *state = nullptr;
};

} // namespace citor::detail
