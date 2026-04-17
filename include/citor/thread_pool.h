#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

#include <memory>
#include <new>

#include "citor/cancellation.h"
#include "citor/cpos/bulk_for_queries.h"
#include "citor/cpos/fork_join.h"
#include "citor/cpos/parallel_chain.h"
#include "citor/cpos/parallel_for.h"
#include "citor/cpos/parallel_reduce.h"
#include "citor/cpos/parallel_scan.h"
#include "citor/cpos/run_plex.h"
#include "citor/cpos/submit_detached.h"
#include "citor/detail/chain_state.h"
#include "citor/detail/chase_lev_deque.h"
#include "citor/detail/forkjoin_state.h"
#include "citor/detail/futex_park.h"
#include "citor/detail/inline_fallback.h"
#include "citor/detail/job_descriptor.h"
#include "citor/detail/kahan.h"
#include "citor/detail/plex_state.h"
#include "citor/detail/pool_control.h"
#include "citor/detail/reduce_tree.h"
#include "citor/detail/scan_state.h"
#include "citor/detail/topology.h"
#include "citor/detail/worker_loop.h"
#include "citor/detail/worker_state.h"
#include "citor/function_ref.h"
#include "citor/hints.h"

namespace citor {

class PoolGroup;

/// Origin tag for a `ThreadPool` instance.
///
/// `Standalone` is the default for user-created pools (`ThreadPool(participants)`). `Arena` is set
/// by `PoolGroup` for the per-CCD pools it owns; the `Arena` tag also activates the cross-arena
/// deadlock guard in every primitive's dispatch fast path.
enum class PoolKind : std::uint8_t {
  /// Default: a user-owned pool with no `PoolGroup` participation.
  Standalone,
  /// Owned by a `PoolGroup` and pinned to one CCD; participates in the cross-arena guard.
  Arena
};

/// Per-instance worker pool that owns the lifecycle of background pthreads.
///
/// Construction spawns `participants - 1` background pthreads, pins them when affinity is
/// requested, and parks them on a shared futex; destruction sets the shutdown bit, broadcasts a
/// wake, and joins each pthread. Primitive entry points (`parallelFor`, `parallelReduce`, etc.)
/// are member functions on the same class so they share this lifecycle.
///
/// The class is non-copyable and non-movable: workers carry interior pointers to per-instance
/// `WorkerState` slots, so any rebase of the pool's address would invalidate them. The public API
/// exposes the participant count, the calling-thread's worker index (0 outside any pool), and a
/// predicate that reports whether the calling thread is a pool worker.
///
/// Padding-suppression note: the class layout groups members for readability over
/// byte-tight packing. A few unused bytes at the boundary between `Topology` (variable-sized
/// vectors) and the heap-allocated worker pointers are the design trade-off we want.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
class ThreadPool {
public:
  /// Construct a pool with |participants| total participants (producer + background).
  ///
  /// The constructor:
  /// 1. Probes topology via `detectTopology()` to populate physical-core / CCD groupings.
  /// 2. Truncates |participants| to at most the number of physical cores in the process affinity
  ///    mask; the producer counts as participant 0, so `participants - 1` background pthreads are
  ///    spawned.
  /// 3. Creates each background pthread, pins it to its assigned physical core, and lets it run
  ///    `workerMainLoop` until the shutdown bit flips.
  ///
  /// participants Total participants including the producer; must be at least 1.
  /// `std::invalid_argument` when |participants| is 0.
  /// `std::system_error` when pthread creation fails.
  explicit ThreadPool(std::size_t participants)
      : m_topology(detail::detectTopology()), m_workers(nullptr, WorkerArrayDeleter{}),
        m_chainDoneSlots(nullptr, ChainDoneSlotDeleter{}) {
    if (participants == 0) {
      throw std::invalid_argument("ThreadPool: participants must be >= 1");
    }
    initWorkers(participants, m_topology.physicalCores);
  }

private:
  /// Tag selecting the per-CCD arena constructor used by `PoolGroup`.
  struct ArenaTag {};

  /// Construct an `Arena` pool pinned to a specific subset of CPU ids.
  ///
  /// Used exclusively by `PoolGroup` to spawn one pool per CCD. The supplied |cpuPins| list
  /// overrides the topology-derived physical-core selection so workers stay inside one CCD; the
  /// |arenaIndex| is recorded on every worker's `ThreadContext` so the cross-arena guard can
  /// fall through to inline-on-caller without consulting the `PoolGroup`.
  ///
  /// participants Total participants for this arena (producer + background).
  /// cpuPins      Logical CPU ids to pin the workers to; one CPU per participant.
  /// arenaIndex   Zero-based index of this arena in the owning `PoolGroup`.
  ThreadPool(ArenaTag /*tag*/, std::size_t participants, const std::vector<std::uint32_t> &cpuPins,
             std::uint32_t arenaIndex)
      : m_kind(PoolKind::Arena), m_arenaIndex(arenaIndex), m_topology(detail::detectTopology()),
        m_workers(nullptr, WorkerArrayDeleter{}),
        m_chainDoneSlots(nullptr, ChainDoneSlotDeleter{}) {
    if (participants == 0) {
      throw std::invalid_argument("ThreadPool: participants must be >= 1");
    }
    initWorkers(participants, cpuPins);
  }

  friend class PoolGroup;

  /// Shared body for both constructors: spawn |participants| workers pinned to |cpuPins|.
  ///
  /// Refactored out of the original constructor body so the `Arena` overload can supply a custom
  /// pin list without duplicating the worker-allocation, deque-creation, and pthread-spawn code.
  void initWorkers(std::size_t participants, const std::vector<std::uint32_t> &cpuPins) {
    std::size_t maxByTopology = participants;
    if (!cpuPins.empty()) {
      maxByTopology = cpuPins.size();
    } else if (m_topology.physicalCount > 0U) {
      maxByTopology = m_topology.physicalCount;
    }
    const std::size_t effective = participants <= maxByTopology ? participants : maxByTopology;

    m_control.participants = static_cast<std::uint32_t>(effective);

    void *raw =
        ::operator new(sizeof(detail::WorkerState) * effective, std::align_val_t{kCacheLine});
    auto *first = static_cast<detail::WorkerState *>(raw);
    for (std::size_t i = 0; i < effective; ++i) {
      ::new (static_cast<void *>(first + i)) detail::WorkerState();
    }
    m_workers = std::unique_ptr<detail::WorkerState, WorkerArrayDeleter>(
        first, WorkerArrayDeleter{effective});

    // Pre-allocate the per-worker `done` slot block reused by every `parallelChain` dispatch. The
    // block is owned by the pool so chain calls pay no allocator round trip on the dispatch hot
    // path; each call zero-resets the slots at entry to honour the chain's fresh-epoch contract.
    void *rawSlots =
        ::operator new(sizeof(detail::ChainDoneSlot) * effective, std::align_val_t{kCacheLine});
    auto *firstSlot = static_cast<detail::ChainDoneSlot *>(rawSlots);
    for (std::size_t i = 0; i < effective; ++i) {
      ::new (static_cast<void *>(firstSlot + i)) detail::ChainDoneSlot();
    }
    m_chainDoneSlots = std::unique_ptr<detail::ChainDoneSlot, ChainDoneSlotDeleter>(
        firstSlot, ChainDoneSlotDeleter{effective});

    for (std::size_t i = 0; i < effective; ++i) {
      auto *w = m_workers.get() + i;
      w->workerId = static_cast<std::uint32_t>(i);
      const std::uint32_t cpu = (i < cpuPins.size()) ? cpuPins[i] : UINT32_MAX;
      w->cpuId = cpu;
      if (cpu != UINT32_MAX && cpu < m_topology.ccdOfCpu.size()) {
        w->ccdId = m_topology.ccdOfCpu[cpu];
      }
    }

    // Per-worker Chase-Lev work-stealing deques. Allocated up-front (one per participant) so the
    // forkJoin steal probe pays no allocator round-trip on its hot path. Each deque stores
    // `Task *` payloads pointing at descriptors that live on the producer's stack (root tasks)
    // or on a recursive worker's stack (children spawned during a body).
    m_workerDeques.reserve(effective);
    for (std::size_t i = 0; i < effective; ++i) {
      m_workerDeques.emplace_back(std::make_unique<detail::ChaseLevDeque<detail::Task *>>());
    }

    // Cache the per-slot CCD index in a contiguous array so the forkJoin victim-selection probe
    // skips the indirection through `WorkerState::ccdId` on the steal hot path.
    m_ccdOfSlot.assign(effective, UINT32_MAX);
    for (std::size_t i = 0; i < effective; ++i) {
      m_ccdOfSlot[i] = (m_workers.get() + i)->ccdId;
    }

    if (effective <= 1) {
      return;
    }
    m_workerThreads.resize(effective - 1);
    m_workerSpawnArgs.reserve(effective - 1);
    for (std::size_t i = 1; i < effective; ++i) {
      auto &arg = m_workerSpawnArgs.emplace_back();
      arg.pool = this;
      arg.workerIndex = i;
    }

#ifdef __linux__
    for (std::size_t i = 1; i < effective; ++i) {
      pthread_attr_t attrs;
      const int initRc = pthread_attr_init(&attrs);
      if (initRc != 0) {
        m_workerThreads.resize(i - 1);
        m_workerSpawnArgs.resize(i - 1);
        shutdownAndJoin();
        throw std::runtime_error("ThreadPool: pthread_attr_init failed");
      }
      (void)pthread_attr_setstacksize(&attrs, std::size_t{256U} * std::size_t{1024U});
      const int rc = pthread_create(&m_workerThreads[i - 1], &attrs, &ThreadPool::workerEntry,
                                    &m_workerSpawnArgs[i - 1]);
      (void)pthread_attr_destroy(&attrs);
      if (rc != 0) {
        m_workerThreads.resize(i - 1);
        m_workerSpawnArgs.resize(i - 1);
        shutdownAndJoin();
        throw std::runtime_error("ThreadPool: pthread_create failed");
      }
    }
#else
    m_fallbackThreads.reserve(effective - 1);
    for (std::size_t i = 1; i < effective; ++i) {
      try {
        m_fallbackThreads.emplace_back(&ThreadPool::workerEntryStdThread, this, i);
      } catch (...) {
        shutdownAndJoin();
        throw;
      }
    }
#endif
  }

public:
  /// Destroy the pool: drain pending work, signal shutdown, wake every worker, then join.
  ///
  /// Detached tasks submitted via `submitDetached` are drained first so any state captured by a
  /// still-running body is torn down inside the body, not under us. The synchronous-worker
  /// shutdown path runs only after the detached counter reaches zero.
  ~ThreadPool() {
    waitForDetachedDrain();
    shutdownAndJoin();
  }

  /// Pools own raw pthreads with interior pointers; copying is structurally unsafe.
  ThreadPool(const ThreadPool &) = delete;
  /// Pools own raw pthreads with interior pointers; copying is structurally unsafe.
  ThreadPool &operator=(const ThreadPool &) = delete;
  /// Workers carry interior pointers; moving would invalidate them mid-flight.
  ThreadPool(ThreadPool &&) = delete;
  /// Workers carry interior pointers; moving would invalidate them mid-flight.
  ThreadPool &operator=(ThreadPool &&) = delete;

  /// Return the effective number of participants (producer + background workers).
  ///
  /// Effective participant count; always at least 1.
  [[nodiscard]] std::size_t participants() const noexcept { return m_control.participants; }

  /// Number of CCD (or shared-L3) groups detected by the topology probe.
  ///
  /// CCD count; always at least 1 on a host with at least one physical core.
  [[nodiscard]] std::uint32_t ccdCount() const noexcept { return m_topology.ccdCount; }

  /// Origin tag of this pool: `Standalone` (user-owned) or `Arena` (`PoolGroup`-owned).
  ///
  /// Read at the entry of every primitive's dispatch path: when the calling thread is a worker on
  /// a different `Arena` pool, the dispatch falls through to inline-on-caller execution to avoid
  /// the cross-arena deadlock that would result from a worker blocking on another arena's queue.
  ///
  /// `PoolKind::Arena` when this pool was constructed by a `PoolGroup`; `Standalone`
  ///         otherwise.
  [[nodiscard]] PoolKind kind() const noexcept { return m_kind; }

  /// Index of this pool inside its owning `PoolGroup`.
  ///
  /// Returns `0` for `Standalone` pools and for the first arena. Used as the participant token
  /// carried in `ThreadContext::arenaIndex` so the cross-arena guard can compare without holding
  /// a back-pointer to the `PoolGroup`.
  ///
  /// Zero-based arena index.
  [[nodiscard]] std::uint32_t arenaIndex() const noexcept { return m_arenaIndex; }

  /// Slot index of the calling thread within any pool.
  ///
  /// Returns 0 when the calling thread is not currently inside a pool worker body.
  ///
  /// Worker slot index, or 0 outside any pool.
  [[nodiscard]] static std::size_t workerIndex() noexcept { return tlsContext().slot; }

  /// Test whether the calling thread is currently a pool worker.
  ///
  /// `true` when the calling thread is inside a pool worker body, otherwise `false`.
  [[nodiscard]] static bool insidePoolWorker() noexcept { return tlsContext().insidePoolWorker; }

  /// Return the calling thread's arena participant token.
  ///
  /// Used by `PoolGroup::localArena()` to pick the arena the calling thread is currently a worker
  /// of. Returns `static_cast<std::uint32_t>(-1)` when the thread is not a worker on any `Arena`
  /// pool (the producer thread, a user-spawned `std::thread`, or a worker on a `Standalone` pool).
  ///
  /// Arena index, or the sentinel `static_cast<std::uint32_t>(-1)` when no token is set.
  [[nodiscard]] static std::uint32_t currentArenaIndexHint() noexcept {
    const auto &ctx = tlsContext();
    if (!ctx.insidePoolWorker || ctx.kind != PoolKind::Arena) {
      return static_cast<std::uint32_t>(-1);
    }
    return ctx.arenaIndex;
  }

  /// Run |fn| over `[first, last)` in parallel using the policy from |HintsT|.
  ///
  /// The body is invoked once per block as `fn(blockFirst, blockAfterLast)`. Synchronous: the
  /// call returns only after every block has completed (or after the first thrown exception is
  /// propagated). The hint type's static-constexpr members drive compile-time policy:
  ///
  /// - `Balance::StaticUniform` runs the worker-strided block partition (no atomics on the hot
  ///   path); `Balance::DynamicChunked` races on the relaxed `nextBlock` counter; the other two
  ///   tiers fall back to dynamic until the work-stealing tier ships.
  /// - `chunk == 0` derives a sensible default from `(last - first) / participants`.
  /// - `n * estimatedItemNs * 1e-3 < minTaskUs * participants` runs the body inline on the
  ///   producer's thread; no worker is woken.
  ///
  /// HintsT Hint type whose static-constexpr members drive compile-time policy.
  /// F      Callable type invoked once per block as `F(std::size_t lo, std::size_t hi)`.
  /// first  Inclusive lower bound of the iteration range.
  /// last   Exclusive upper bound of the iteration range.
  /// fn     Callable invoked over each block.
  /// tok    Cancellation token observed at chunk boundaries.
  template <class HintsT, class F>
  void parallelFor(std::size_t first, std::size_t last, F &&fn, CancellationToken tok = {}) {
    if (last <= first) {
      return;
    }
    if (shouldFallThroughCrossArena()) {
      runInline(first, last, std::forward<F>(fn));
      return;
    }
    const std::size_t n = last - first;
    const std::size_t participants = m_control.participants;

    if constexpr (HintsT::estimatedItemNs > 0.0) {
      if (detail::shouldRunInline(n, participants, HintsT::estimatedItemNs, HintsT::minTaskUs)) {
        runInline(first, last, std::forward<F>(fn));
        return;
      }
    } else {
      if (participants <= 1) {
        runInline(first, last, std::forward<F>(fn));
        return;
      }
    }

    auto wrapper = [&fn](std::size_t lo, std::size_t hi) { fn(lo, hi); };
    const FunctionRef<void(std::size_t, std::size_t)> body{wrapper};

    detail::JobDescriptor desc;
    desc.first = first;
    desc.last = last;
    desc.participants = static_cast<std::uint32_t>(participants);
    desc.balance = HintsT::balance;
    desc.priority = HintsT::priority;
    desc.body = body;
    desc.token = std::move(tok);

    const std::size_t hintChunk = HintsT::chunk;
    fillBlockShape(desc, n, participants, hintChunk);

    dispatchOneStatic<HintsT::balance>(desc);
  }
};

} // namespace citor
