#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <thread>

#include "citor/detail/futex_park.h"
#include "citor/detail/job_descriptor.h"
#include "citor/detail/pool_control.h"
#include "citor/detail/worker_loop.h"
#include "citor/detail/worker_state.h"

namespace {

using namespace std::chrono_literals;

template <class Pred>
bool waitUntil(Pred pred, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }
    std::this_thread::yield();
  }
  return pred();
}

struct OvertakenMailboxProbe {
  static inline std::atomic<int> entryCalls{0};
  static inline std::atomic<bool> firstEntered{false};
  static inline std::atomic<bool> allowFirstReturn{false};

  static void reset() noexcept {
    entryCalls.store(0, std::memory_order_relaxed);
    firstEntered.store(false, std::memory_order_relaxed);
    allowFirstReturn.store(false, std::memory_order_relaxed);
  }

  static void entry(citor::detail::JobDescriptor *, std::uint32_t,
                    std::uint64_t) noexcept {
    const int call = entryCalls.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (call == 1) {
      firstEntered.store(true, std::memory_order_release);
      while (!allowFirstReturn.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    }
  }
};

void wakeControl(citor::detail::PoolControl &control) noexcept {
  const std::uint32_t next =
      control.futexWord.load(std::memory_order_relaxed) + 1U;
  control.futexWord.store(next, std::memory_order_release);
  (void)citor::detail::futexWakePrivate(&control.futexWord, INT_MAX);
}

void shutdownWorker(citor::detail::WorkerState &worker,
                    citor::detail::PoolControl &control) noexcept {
  control.activeJob.store(nullptr, std::memory_order_release);
  control.generation.fetch_or(citor::detail::PoolControl::kShutdownBit,
                              std::memory_order_release);

  const std::uint64_t prev = worker.mailbox.load(std::memory_order_relaxed);
  const std::uint64_t flagBits =
      prev & (citor::detail::PoolControl::kPhaseStep - 1ULL);
  const std::uint64_t shutdownMb =
      ((prev + citor::detail::PoolControl::kPhaseStep) &
       ~(citor::detail::PoolControl::kPhaseStep - 1ULL)) |
      flagBits | citor::detail::PoolControl::kShutdownBit;

  worker.mailbox.store(shutdownMb, std::memory_order_release);
  wakeControl(control);
}

} // namespace

// Regression for the cold-collapse CAS-fail path. The worker enters
// generation 1, blocks inside its body, the producer publishes
// generation 2 over the same mailbox, the worker's done-stamp CAS
// fails, fetch_or sets kAckedBit on the new mailbox value. The next
// loop iteration must observe generation 2 as a phase change and run
// its body; setting `lastSeenMailbox` to the new value would mark the
// phase as already seen and drop the second dispatch.
TEST(WorkerMailboxProtocol,
     OvertakingGenerationIsProcessedAfterColdCollapseCasFailure) {
  OvertakenMailboxProbe::reset();

  citor::detail::PoolControl control;
  control.participants = 2;
  control.pendingMaskBits = 0b10;

  citor::detail::WorkerState worker;
  worker.workerId = 1;

  citor::detail::JobDescriptor desc;
  desc.workerEntry = &OvertakenMailboxProbe::entry;
  // Must be non-null so the worker loop takes the cold-collapse CAS-stamp
  // path after `workerEntry` returns.
  desc.workerStateBase = &worker;

  std::thread t([&] { citor::detail::workerMainLoop(worker, control); });

  const std::uint64_t phase1 = citor::detail::PoolControl::kPhaseStep;
  const std::uint64_t phase2 = phase1 + citor::detail::PoolControl::kPhaseStep;

  // Publish generation 1.
  control.activeJob.store(&desc, std::memory_order_release);
  control.generation.store(phase1, std::memory_order_release);
  worker.mailboxDesc = &desc;
  worker.mailbox.store(phase1, std::memory_order_release);
  wakeControl(control);

  ASSERT_TRUE(waitUntil(
      [] {
        return OvertakenMailboxProbe::firstEntered.load(
            std::memory_order_acquire);
      },
      2s));

  // Publish generation 2 over the same mailbox while the worker is still
  // inside generation 1's workerEntry. When the worker returns from the
  // body and tries to CAS-stamp generation 1's doneSentinel, the CAS will
  // fail because mailbox already contains generation 2.
  control.activeJob.store(&desc, std::memory_order_release);
  control.generation.store(phase2, std::memory_order_release);
  worker.mailboxDesc = &desc;
  worker.mailbox.store(phase2, std::memory_order_release);

  OvertakenMailboxProbe::allowFirstReturn.store(true,
                                                std::memory_order_release);

  const bool processedSecondGeneration = waitUntil(
      [] {
        return OvertakenMailboxProbe::entryCalls.load(
                   std::memory_order_acquire) >= 2;
      },
      500ms);

  shutdownWorker(worker, control);
  t.join();

  EXPECT_TRUE(processedSecondGeneration)
      << "Worker treated an overtaking mailbox generation as already seen. "
         "The CAS-fail path must leave lastSeenMailbox at the old generation "
         "so the next loop iteration's phase compare fires on the overtake.";
}
