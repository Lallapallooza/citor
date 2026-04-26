#pragma once

#include <atomic>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#endif

#include "citor/detail/dispatch_dynamic.h"
#include "citor/detail/dispatch_static.h"
#include "citor/detail/futex_park.h"
#include "citor/detail/job_descriptor.h"
#include "citor/detail/pool_control.h"
#include "citor/detail/worker_state.h"
#include "citor/hints.h"

namespace citor::detail {

/// Spin-then-park budget applied after a bulk job completes.
///
/// Two bounds gate the spin: a PAUSE iteration count and a TSC cycle budget. Both must trip before
/// the worker invokes `FUTEX_WAIT_PRIVATE`. The PAUSE bound captures co-tenancy cases where the
/// scheduler de-schedules our worker (TSC keeps advancing while iterations stay constant); the TSC
/// bound captures the inverse (busy spinning while iterations advance fast).
///
/// The Karlin 1991 SOSP optimum sets `maxCycles` to roughly twice the wakeup latency; on Linux 6.x
/// the futex round-trip is 2-3 microseconds, which at 5 GHz is 10-15k cycles. The default budget
/// is conservative -- it errs on the side of parking earlier -- and is tuned at
/// startup once the empty-fan-out benchmark exists.
struct SpinPolicy {
  /// Maximum PAUSE iterations before checking the TSC bound.
  std::uint32_t maxIters = 0;

  /// Maximum TSC cycles to spin before parking.
  std::uint64_t maxCycles = 0;
};

/// Default spin budget applied to workers between jobs.
///
/// The PAUSE bound and the TSC bound together cover both co-tenancy stalls and runaway loops:
/// the iteration counter trips when the scheduler de-schedules the worker (TSC keeps advancing
/// while iterations stay constant); the TSC bound trips when iterations advance fast but no new
/// generation has arrived. The cycle bound dominates in practice because a single PAUSE
/// instruction takes on the order of one hundred cycles on modern x86-64.
///
/// The cycle budget is sized above the typical back-to-back dispatch interval on a multi-CCD
/// fan-out so workers do not park between hot dispatches; parking each round costs a
/// `FUTEX_WAIT` / `FUTEX_WAKE` round-trip that dominates the empty-fan-out floor. A worker that
/// idles past the budget still parks promptly so an idle pool does not burn CPU indefinitely.
#ifndef CITOR_SPIN_AFTER_BULK_JOB_MAX_ITERS
#define CITOR_SPIN_AFTER_BULK_JOB_MAX_ITERS 8192U
#endif
#ifndef CITOR_SPIN_AFTER_BULK_JOB_MAX_CYCLES
#define CITOR_SPIN_AFTER_BULK_JOB_MAX_CYCLES 1000000ULL
#endif
inline constexpr SpinPolicy kSpinAfterBulkJob{.maxIters = CITOR_SPIN_AFTER_BULK_JOB_MAX_ITERS,
                                              .maxCycles = CITOR_SPIN_AFTER_BULK_JOB_MAX_CYCLES};

/// Insert a single PAUSE / YIELD hint to back off without de-scheduling.
///
/// `_mm_pause` on x86-64 is the spin-loop hint of choice (P0514R4); it lets the CPU drop hyper-
/// thread issue slots without yielding the scheduler quantum. Non-x86 builds fall through to a
/// compiler barrier so the loop is still legal but does not impose a spin-tax beyond loop control.
inline void cpuRelax() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
  _mm_pause();
#else
  std::atomic_signal_fence(std::memory_order_acq_rel);
#endif
}

/// Read the current TSC for spin-budget bookkeeping.
///
/// Returns 0 on platforms without a TSC; the caller's loop falls through to the iteration bound.
inline std::uint64_t readTsc() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
  unsigned int aux = 0;
  return __builtin_ia32_rdtscp(&aux);
#else
  return 0ULL;
#endif
}

/// Check whether a generation observed by the worker indicates the pool should exit.
///
/// The worker exits when the shutdown bit is set AND the active job slot is empty. The
/// active-job check guards the race where the producer publishes a final job concurrently with
/// the shutdown signal: workers must drain that job before exiting.
///
/// control Pool's shared control block.
/// `true` when the worker should leave the loop.
inline bool shouldExit(const PoolControl &control) noexcept {
  const std::uint64_t gen = control.generation.load(std::memory_order_acquire);
  if ((gen & PoolControl::kShutdownBit) == 0) {
    return false;
  }
  return control.activeJob.load(std::memory_order_acquire) == nullptr;
}

/// Run the worker's share of the active job with the balance fixed at compile time.
///
/// Producer call sites know the dispatch shape from the primitive's hint type and use this
/// overload to compile away the runtime branch on `desc.balance`. Workers, which read the
/// descriptor's runtime balance after observing a new generation, use the runtime overload below.
///
/// BalanceV Compile-time balance choice; selects the dispatch tier without a runtime
///                  branch.
/// desc     Active job descriptor.
/// rank     Worker's slot index in the dispatch.
template <Balance BalanceV>
inline void runActiveJobStatic(JobDescriptor &desc, std::uint32_t rank) noexcept {
  if constexpr (BalanceV == Balance::StaticUniform) {
    runStaticPartition(desc, rank);
  } else {
    // Dynamic counter handles `DynamicChunked` directly; the work-stealing tiers (`Steal`,
    // `Recursive`) are reserved for opt-in primitives in later slices and fall back here so the
    // call still completes deterministically until the deque-based tier ships.
    runDynamicCounter(desc, rank);
  }
}

/// Run the worker's share of the active job, dispatching by `Balance` kind at runtime.
///
/// Reads the published `JobDescriptor` from `control.activeJob` (already acquire-loaded by the
/// caller's logic) and runs the appropriate dispatch tier. Static-uniform takes the worker-strided
/// branch; dynamic-chunked takes the relaxed-counter branch. Future tiers (steal, recursive)
/// extend the switch without changing the caller's protocol.
///
/// desc Active job descriptor.
/// rank Worker's slot index in the dispatch.
inline void runActiveJob(JobDescriptor &desc, std::uint32_t rank) noexcept {
  if (desc.balance == Balance::StaticUniform) {
    runActiveJobStatic<Balance::StaticUniform>(desc, rank);
    return;
  }
  runActiveJobStatic<Balance::DynamicChunked>(desc, rank);
}

/// Spin-then-park between dispatches, exiting cleanly on shutdown.
///
/// On every observed generation change the worker:
///   1. Loads `activeJob` (acquire) -- pairs with the producer's release-store.
///   2. Runs its share of the job via `runActiveJob`.
///   3. Stamps `doneEpoch = generation` (release) -- pairs with the producer's acquire-load on
///      the join path.
///
/// Lost-wakeup defense: the worker reads `futexWord` BEFORE re-checking `generation`, then passes
/// the captured token to `futexWaitPrivate` so the kernel rejects the wait with `EAGAIN` if a wake
/// raced ahead. After the syscall returns the worker re-checks `generation` again; spurious wakes
/// are correctness-neutral.
///
/// self    Worker's per-thread state (mutable; counters incremented).
/// control Pool's shared control block.
inline void workerMainLoop(WorkerState &self, PoolControl &control) noexcept {
  std::uint64_t lastSeenMailbox = 0;
  // Cache the worker's identity locally; `WorkerState::workerId` lives on a
  // read-only line but the local lets the compiler keep it in a register
  // across the dispatch+stamp loop.
  const std::uint32_t workerId = self.workerId;
  std::uint64_t mailbox = self.mailbox.load(std::memory_order_acquire);

  while (true) {
    if ((mailbox & PoolControl::kShutdownBit) != 0) {
      // Drain any pending active job before exiting; shutdown is signaled by stamping the
      // worker's mailbox with the shutdown bit set.
      if (control.activeJob.load(std::memory_order_acquire) == nullptr) {
        return;
      }
    }

    if (mailbox != lastSeenMailbox) {
      void *raw = self.mailboxDesc;
      if (raw != nullptr) {
        auto *desc = static_cast<JobDescriptor *>(raw);
        if (desc->workerEntry != nullptr) {
          desc->workerEntry(desc, workerId);
        } else {
          runActiveJob(*desc, workerId);
        }
      }
      // Publish done-epoch with `release`: anything the body wrote is now visible to the
      // producer's acquire-load on this slot.
      self.doneEpoch.store(mailbox, std::memory_order_release);
      lastSeenMailbox = mailbox;
      mailbox = self.mailbox.load(std::memory_order_acquire);
      continue;
    }

    // No new work. Spin within the budget, then park on the futex.
    const bool lowLatencyActive = control.hotSpinDepth.load(std::memory_order_acquire) != 0U;
    if (lowLatencyActive) {
      const std::uint64_t hotEpoch = control.hotSpinEpoch.load(std::memory_order_acquire);
      self.hotSpinEpoch.store(hotEpoch, std::memory_order_release);
    }
    // Under low-latency scope the spin's cycle budget is dead weight: the contract is
    // "do not park", so an iter-cap exit just routes back into the outer `while(true)` and
    // re-enters spin. Skipping the `rdtscp` (serializing, ~30-50 cycles per dispatch in
    // steady state) removes the ride-along stall the worker pays right after stamping
    // `doneEpoch` and before observing the next published mailbox.
    const std::uint64_t startTsc = lowLatencyActive ? 0ULL : readTsc();
    bool parkRequired = true;
    // Tight-first-N spin on the worker's own mailbox line. The producer's release-store on
    // mailbox propagates intra-CCD in tens of nanoseconds. PAUSE adds back-off slack which means each
    // PAUSE-load iter adds detection slack. For the first 8 iterations skip PAUSE so
    // the worker detects a "publish-already-arrived" case (cache line was prefetched on the
    // producer's store edge) within a handful of cycles of the load, before falling back to PAUSE-spin
    // for the longer-tail case.
    constexpr std::uint32_t kTightProbeIters = 8U;
    for (std::uint32_t iter = 0; iter < kTightProbeIters; ++iter) {
      const std::uint64_t spinMb = self.mailbox.load(std::memory_order_acquire);
      if (spinMb != lastSeenMailbox) {
        mailbox = spinMb;
        parkRequired = false;
        break;
      }
    }
    if (parkRequired) {
      // `rdtscp` serializes the pipeline and costs roughly the same as a small batch of
      // pause-load-compare iterations on Zen, so reading the TSC every iter doubled the spin
      // loop body. Sample it every 64 iters instead.
      for (std::uint32_t iter = 0; iter < kSpinAfterBulkJob.maxIters; ++iter) {
        cpuRelax();
        const std::uint64_t spinMb = self.mailbox.load(std::memory_order_acquire);
        if (spinMb != lastSeenMailbox) {
          mailbox = spinMb;
          parkRequired = false;
          break;
        }
        if (!lowLatencyActive && (iter & 63U) == 63U &&
            readTsc() - startTsc >= kSpinAfterBulkJob.maxCycles) {
          break;
        }
      }
    }
    if (!parkRequired) {
      continue;
    }
    if (control.hotSpinDepth.load(std::memory_order_acquire) != 0U) {
      const std::uint64_t hotEpoch = control.hotSpinEpoch.load(std::memory_order_acquire);
      self.hotSpinEpoch.store(hotEpoch, std::memory_order_release);
      mailbox = self.mailbox.load(std::memory_order_acquire);
      continue;
    }

    // Capture parking token, re-check source-of-truth, then enter futex wait. The double-check
    // closes the lost-wakeup race: if a wake happens between reading the token and the syscall,
    // the kernel returns EAGAIN immediately because the token's value moved.
    const std::uint32_t parkToken = control.futexWord.load(std::memory_order_relaxed);
    const std::uint64_t recheckMb = self.mailbox.load(std::memory_order_acquire);
    if (recheckMb != lastSeenMailbox) {
      mailbox = recheckMb;
      continue;
    }
    if ((recheckMb & PoolControl::kShutdownBit) != 0 &&
        control.activeJob.load(std::memory_order_acquire) == nullptr) {
      return;
    }
    self.parks.store(self.parks.load(std::memory_order_relaxed) + 1U,
                     std::memory_order_relaxed);
#ifdef __linux__
    (void)futexWaitPrivate(&control.futexWord, parkToken, nullptr);
#else
    (void)futexWaitPrivate(&control.futexWord, parkToken, static_cast<const void *>(nullptr));
#endif
    self.wakes.store(self.wakes.load(std::memory_order_relaxed) + 1U,
                     std::memory_order_relaxed);
    mailbox = self.mailbox.load(std::memory_order_acquire);
  }
}

} // namespace citor::detail
