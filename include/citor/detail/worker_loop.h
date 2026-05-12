#pragma once

#include <atomic>
#include <cstdint>

#if defined(_M_X64) && defined(_MSC_VER)
#include <intrin.h>
#endif

#include "citor/detail/cpu_relax.h"
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
/// Two bounds gate the spin: a PAUSE iteration count and a TSC cycle budget.
/// Both must trip before the worker invokes `FUTEX_WAIT_PRIVATE`. The PAUSE
/// bound captures co-tenancy cases where the scheduler de-schedules our
/// worker (TSC keeps advancing while iterations stay constant); the TSC
/// bound captures the inverse (busy spinning while iterations advance fast).
///
/// The Karlin 1991 SOSP optimum sets `maxCycles` to roughly twice the wakeup
/// latency.
struct SpinPolicy {
  /// Maximum PAUSE iterations before checking the TSC bound.
  std::uint32_t maxIters = 0;

  /// Maximum TSC cycles to spin before parking.
  std::uint64_t maxCycles = 0;
};

#ifndef CITOR_SPIN_AFTER_BULK_JOB_MAX_ITERS
#define CITOR_SPIN_AFTER_BULK_JOB_MAX_ITERS 8192U
#endif
#ifndef CITOR_SPIN_AFTER_BULK_JOB_MAX_CYCLES
#define CITOR_SPIN_AFTER_BULK_JOB_MAX_CYCLES 1000000ULL
#endif
/// Default spin budget applied to workers between jobs. The PAUSE and TSC
/// bounds together cover co-tenancy stalls and runaway loops; the cycle
/// budget sits above the typical back-to-back dispatch interval so workers
/// do not park between hot dispatches. A worker that idles past the budget
/// still parks promptly.
inline constexpr SpinPolicy kSpinAfterBulkJob{
    .maxIters = CITOR_SPIN_AFTER_BULK_JOB_MAX_ITERS,
    .maxCycles = CITOR_SPIN_AFTER_BULK_JOB_MAX_CYCLES};

/// Read the current TSC for spin-budget bookkeeping. Returns 0 on platforms
/// without a TSC; the caller's loop falls through to the iteration bound.
inline std::uint64_t readTsc() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
  unsigned int aux = 0;
#if defined(_M_X64) && defined(_MSC_VER)
  return __rdtscp(&aux);
#else
  return __builtin_ia32_rdtscp(&aux);
#endif
#else
  return 0ULL;
#endif
}

/// Check whether a generation observed by the worker indicates the pool
/// should exit.
///
/// The worker exits when the shutdown bit is set AND the active job slot is
/// empty. The active-job check guards the race where the producer publishes
/// a final job concurrently with the shutdown signal: workers must drain
/// that job before exiting.
inline bool shouldExit(const PoolControl &control) noexcept {
  const std::uint64_t gen = control.generation.load(std::memory_order_acquire);
  if ((gen & PoolControl::kShutdownBit) == 0) {
    return false;
  }
  return control.activeJob.load(std::memory_order_acquire) == nullptr;
}

/// Run the worker's share of the active job with the balance fixed at
/// compile time.
///
/// Producer call sites know the dispatch shape from the primitive's hint
/// type and use this overload to compile away the runtime branch on
/// `desc.balance`. Workers, which read the descriptor's runtime balance
/// after observing a new generation, use the runtime overload below.
template <Balance BalanceV>
inline void runActiveJobStatic(JobDescriptor &desc,
                               std::uint32_t rank) noexcept {
  if constexpr (BalanceV == Balance::StaticUniform) {
    runStaticPartition(desc, rank);
  } else {
    // Dynamic counter handles `DynamicChunked` directly; the work-stealing
    // tiers (`Steal`, `Recursive`) are reserved for opt-in primitives and
    // fall back here so the call still completes deterministically until
    // the deque-based tier ships.
    runDynamicCounter(desc, rank);
  }
}

/// Run the worker's share of the active job, dispatching by `Balance` kind
/// at runtime. Reads the published `JobDescriptor` from `control.activeJob`
/// (already acquire-loaded by the caller's logic) and runs the appropriate
/// dispatch tier. Static-uniform takes the worker-strided branch;
/// dynamic-chunked takes the relaxed-counter branch.
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
///   1. Loads `activeJob` (acquire) -- pairs with the producer's
///      release-store.
///   2. Runs its share of the job via `runActiveJob`.
///   3. Stamps `mailbox |= kDoneBit` (release) -- pairs with the
///      producer's acquire-load on the join path. Same-line ack: the
///      mailbox line carries publish + ack so the producer's join reads
///      one cache line per worker, not two.
///
/// Lost-wakeup defense: the worker reads `futexWord` BEFORE re-checking
/// `generation`, then passes the captured token to `futexWaitPrivate` so
/// the kernel rejects the wait with `EAGAIN` if a wake raced ahead. After
/// the syscall returns the worker re-checks `generation` again; spurious
/// wakes are correctness-neutral.
inline void workerMainLoop(WorkerState &self, PoolControl &control) noexcept {
  std::uint64_t lastSeenMailbox = 0;
  // Cache the worker's identity locally; `WorkerState::workerId` lives on
  // a read-only line but the local lets the compiler keep it in a register
  // across the dispatch+stamp loop.
  const std::uint32_t workerId = self.workerId;
  JobDescriptor *cachedDesc = nullptr; // NOLINT(misc-const-correctness)
  void (*cachedWorkerEntry)(JobDescriptor *, std::uint32_t,
                            std::uint64_t) noexcept = nullptr;
  bool cachedColdCollapse = false;
  std::uint64_t mailbox = self.mailbox.load(std::memory_order_acquire);

  while (true) {
    if ((mailbox & PoolControl::kShutdownBit) != 0) {
      // Drain any pending active job before exiting; shutdown is signaled
      // by stamping the worker's mailbox with the shutdown bit set.
      if (control.activeJob.load(std::memory_order_acquire) == nullptr) {
        return;
      }
    }

    // Same-line ack protocol: producer publishes new phase with DONE bit
    // CLEAR. The reuse bit (kReuseBit) is set by the producer when this
    // dispatch reuses cached params; the worker observes it and skips
    // reading desc fields, calling the typed runner's cached fn directly.
    // We compare phase ignoring DONE.
    const std::uint64_t mailboxPhase = mailbox & ~PoolControl::kDoneBit;
    const std::uint64_t lastPhase = lastSeenMailbox & ~PoolControl::kDoneBit;
    if (mailboxPhase != lastPhase) {
      const bool reuse = (mailbox & PoolControl::kReuseBit) != 0U;
      void *raw =
          reuse && cachedDesc != nullptr ? cachedDesc : self.mailboxDesc;
      bool coldCollapseDispatch = false;
      if (raw != nullptr) {
        auto *desc = static_cast<JobDescriptor *>(raw);
        auto *workerEntry = cachedWorkerEntry;
        bool coldCollapseCapable = cachedColdCollapse;
        if (!reuse || desc != cachedDesc) [[unlikely]] {
          workerEntry = desc->workerEntry;
          coldCollapseCapable = desc->workerStateBase != nullptr;
          cachedDesc = desc;
          cachedWorkerEntry = workerEntry;
          cachedColdCollapse = coldCollapseCapable;
        }
        coldCollapseDispatch =
            coldCollapseCapable && (mailbox & PoolControl::kSkipClaimBit) == 0U;
        if (workerEntry != nullptr) {
          // Pass kReuseBit-aware mailbox to the runner: when set, runner
          // uses TLS cache and skips reading desc fields, eliminating the
          // producer's TLS desc cache-line transit on steady-state
          // repeated calls. Branchless mailbox flags -> rankPacked high
          // bits. Same observable result as ternaries, one fewer
          // dependency in the call's arg.
          static_assert(PoolControl::kReuseBit == (1ULL << 2));
          static_assert(PoolControl::kSkipClaimBit == (1ULL << 3));
          const auto packedFlags = static_cast<std::uint32_t>(
              ((mailbox & PoolControl::kReuseBit) << 29) |
              ((mailbox & PoolControl::kSkipClaimBit) << 27));
          const std::uint64_t generation =
              mailbox & ~(PoolControl::kPhaseStep - 1ULL);
          workerEntry(desc, workerId | packedFlags, generation);
        } else {
          runActiveJob(*desc, workerId);
        }
      }
      // (mailbox & ~kDoneBit) | kDoneBit is identity to (mailbox |
      // kDoneBit) when kDoneBit is already clear in mailbox. The producer
      // publishes new phases with kDoneBit clear (the bit is reserved for
      // the worker's stamp), so this equality holds whenever we reach this
      // branch (i.e. the phase changed).
      const std::uint64_t doneVal = mailbox | PoolControl::kDoneBit;
      if (coldCollapseDispatch) [[unlikely]] {
        // Cold-collapse opt-in dispatches let the producer return as soon
        // as it has satisfied the join via a producer-side mailbox stamp,
        // which means the producer can publish the NEXT dispatch's
        // `publishedMb` before this worker finishes its own stamp. A
        // naive `mailbox.store(doneVal)` would clobber that next-gen
        // mailbox value and deadlock the next dispatch's join. CAS the
        // stamp instead: it commits only when the mailbox is still the
        // value this worker observed. If a later dispatch has overtaken
        // the slot, the CAS fails, `expected` holds the new value, and we
        // re-enter the loop to process that next dispatch. The CAS
        // overhead is gated on the rare path -- only primitives that
        // wired `workerStateBase` (currently parallelFor) pay it.
        std::uint64_t expected = mailbox;
        // Always set `kAckedBit` in the target so the producer's
        // next-dispatch publish-side ack-wait observes the worker's
        // release-store regardless of whether this CAS wins the race
        // against the producer's cold-collapse self-stamp or loses it.
        const std::uint64_t doneAcked = doneVal | PoolControl::kAckedBit;
        if (self.mailbox.compare_exchange_strong(expected, doneAcked,
                                                 std::memory_order_release,
                                                 std::memory_order_acquire)) {
          lastSeenMailbox = doneAcked;
          mailbox = doneAcked;
        } else {
          // CAS lost the race against the producer's cold-collapse
          // self-stamp. Re-store `expected | kAckedBit` with release so the
          // producer's next-dispatch publish can acquire-load it before
          // reusing the prior dispatch's stack-frame address for a fresh
          // `JobDescriptor`. Also update `lastSeenMailbox` so the phase
          // compare on the next iteration does not re-enter this branch
          // and re-read descriptor fields (which would race the producer's
          // already-running next dispatch).
          const std::uint64_t ackedMb = expected | PoolControl::kAckedBit;
          self.mailbox.store(ackedMb, std::memory_order_release);
          lastSeenMailbox = ackedMb;
          mailbox = ackedMb;
        }
      } else {
        self.mailbox.store(doneVal, std::memory_order_release);
        lastSeenMailbox = doneVal;
        mailbox = self.mailbox.load(std::memory_order_acquire);
      }
      continue;
    }

    // No new work. Spin within the budget, then park on the futex.
    const bool lowLatencyActive =
        control.hotSpinDepth.load(std::memory_order_acquire) != 0U;
    if (lowLatencyActive) {
      const std::uint64_t hotEpoch =
          control.hotSpinEpoch.load(std::memory_order_acquire);
      if (self.hotSpinEpoch.load(std::memory_order_relaxed) != hotEpoch) {
        self.hotSpinEpoch.store(hotEpoch, std::memory_order_release);
      }
    }
    // Under low-latency scope the spin's cycle budget is dead weight: the
    // contract is "do not park", so an iter-cap exit just routes back into
    // the outer `while(true)` and re-enters spin. Skipping the serializing
    // `rdtscp` removes the ride-along stall the worker pays right after
    // stamping the same-line DONE ack and before observing the next
    // published mailbox.
    const std::uint64_t startTsc = lowLatencyActive ? 0ULL : readTsc();
    bool parkRequired = true;
    // Tight-first-N spin on the worker's own mailbox line. Skip PAUSE
    // briefly so the worker can detect a "publish already arrived" case
    // without adding backoff slack, then fall back to PAUSE-spin for the
    // longer-tail case.
    constexpr std::uint32_t kTightProbeIters = 8U;
    for (std::uint32_t iter = 0; iter < kTightProbeIters; ++iter) {
      const std::uint64_t spinMb = self.mailbox.load(std::memory_order_acquire);
      if (spinMb != lastSeenMailbox) {
        mailbox = spinMb;
        parkRequired = false;
        break;
      }
    }
    if (parkRequired && lowLatencyActive) {
      // Low-latency scope explicitly trades CPU burn for dispatch
      // response. Poll without PAUSE here: the guarded caller has already
      // requested a hot worker fleet, and PAUSE adds wake-detection slack
      // on the exact path this scope is meant to tighten.
      constexpr std::uint32_t kLowLatencyPollIters = 512U;
      for (std::uint32_t iter = 0; iter < kLowLatencyPollIters; ++iter) {
        const std::uint64_t spinMb =
            self.mailbox.load(std::memory_order_acquire);
        if (spinMb != lastSeenMailbox) {
          mailbox = spinMb;
          parkRequired = false;
          break;
        }
      }
    }
    if (parkRequired && !lowLatencyActive) {
      // `rdtscp` serializes the pipeline and costs roughly the same as a
      // small batch of pause-load-compare iterations on Zen, so reading
      // the TSC every iter doubled the spin loop body. Sample it every 64
      // iters instead.
      for (std::uint32_t iter = 0; iter < kSpinAfterBulkJob.maxIters; ++iter) {
        cpuRelax();
        const std::uint64_t spinMb =
            self.mailbox.load(std::memory_order_acquire);
        if (spinMb != lastSeenMailbox) {
          mailbox = spinMb;
          parkRequired = false;
          break;
        }
        if ((iter & 63U) == 63U &&
            readTsc() - startTsc >= kSpinAfterBulkJob.maxCycles) {
          break;
        }
      }
    }
    if (!parkRequired) {
      continue;
    }
    if (control.hotSpinDepth.load(std::memory_order_acquire) != 0U) {
      const std::uint64_t hotEpoch =
          control.hotSpinEpoch.load(std::memory_order_acquire);
      if (self.hotSpinEpoch.load(std::memory_order_relaxed) != hotEpoch) {
        self.hotSpinEpoch.store(hotEpoch, std::memory_order_release);
      }
      mailbox = self.mailbox.load(std::memory_order_acquire);
      continue;
    }

    // Capture parking token, re-check source-of-truth, then enter futex
    // wait. The double-check closes the lost-wakeup race: if a wake
    // happens between reading the token and the syscall, the kernel
    // returns EAGAIN immediately because the token's value moved.
    const std::uint32_t parkToken =
        control.futexWord.load(std::memory_order_relaxed);
    const std::uint64_t recheckMb =
        self.mailbox.load(std::memory_order_acquire);
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
    (void)futexWaitPrivate(&control.futexWord, parkToken,
                           static_cast<const void *>(nullptr));
#endif
    self.wakes.store(self.wakes.load(std::memory_order_relaxed) + 1U,
                     std::memory_order_relaxed);
    mailbox = self.mailbox.load(std::memory_order_acquire);
    // Chain-wake propagation (oneTBB private_server.cpp wake_some /
    // propagate_chain_reaction pattern). When this worker's futex_wait
    // returns and the mailbox phase has advanced past `lastSeenMailbox`,
    // the producer dispatched a new generation while we were parked. Wake
    // up to two more parked workers via futex_wake(N=2) so the chain
    // doubles each hop and reaches every parked worker in log2(N)
    // syscalls instead of forcing the producer to issue one INT_MAX
    // broadcast on its critical path. Skip on shutdown -- shutdownAndJoin
    // already does a single INT_MAX broadcast that's sequenced-after the
    // descriptor cleanup, and we don't want a parked worker waking on
    // shutdown to spawn an INT_MAX-equivalent chain.
    const std::uint64_t newPhase = mailbox & ~PoolControl::kDoneBit;
    const std::uint64_t oldPhase = lastSeenMailbox & ~PoolControl::kDoneBit;
    if (newPhase != oldPhase && (mailbox & PoolControl::kShutdownBit) == 0) {
      (void)futexWakePrivate(&control.futexWord, 2);
    }
  }
}

} // namespace citor::detail
