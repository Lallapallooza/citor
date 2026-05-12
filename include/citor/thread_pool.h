#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#endif

#include <memory>
#include <new>

#include "citor/cancellation.h"
#include "citor/cpos/bulk_for_queries.h"
#include "citor/cpos/fork_join.h"
#include "citor/cpos/inclusive_scan.h"
#include "citor/cpos/parallel_chain.h"
#include "citor/cpos/parallel_for.h"
#include "citor/cpos/parallel_reduce.h"
#include "citor/cpos/parallel_scan.h"
#include "citor/cpos/run_plex.h"
#include "citor/cpos/submit_detached.h"
#include "citor/detail/chain_state.h"
#include "citor/detail/chase_lev_deque.h"
#include "citor/detail/coherence_probe.h"
#include "citor/detail/cpu_relax.h"
#include "citor/detail/forkjoin_state.h"
#include "citor/detail/futex_park.h"
#include "citor/detail/hints_traits.h"
#include "citor/detail/inline_fallback.h"
#include "citor/detail/job_descriptor.h"
#include "citor/detail/kahan.h"
#include "citor/detail/lookback_scan.h"
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
/// `Standalone` is the default for user-created pools. `Arena` is set by
/// `PoolGroup` for the per-CCD pools it owns; the `Arena` tag activates the
/// cross-arena deadlock guard in every primitive's dispatch fast path.
enum class PoolKind : std::uint8_t {
  /// User-owned pool with no `PoolGroup` participation.
  Standalone,
  /// Owned by a `PoolGroup` and pinned to one CCD; participates in the
  /// cross-arena guard.
  Arena
};

// Per-instance worker pool owning the lifecycle of background pthreads.
//
// Construction spawns `participants - 1` background pthreads, pins them when
// affinity is requested, and parks them on a shared futex; destruction sets
// the shutdown bit, broadcasts a wake, and joins each pthread. Primitive
// entry points (`parallelFor`, `parallelReduce`, etc.) are member functions
// so they share this lifecycle.
//
// Non-copyable and non-movable: workers carry interior pointers to per-instance
// `WorkerState` slots, so any rebase of the pool's address would invalidate
// them.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
class ThreadPool {
public:
  /// Construct a pool with |participants| total participants (producer +
  /// background). Probes topology, truncates |participants| to at most the
  /// number of physical cores in the process affinity mask, then spawns
  /// `participants - 1` background pthreads pinned per |workerAffinity|.
  ///
  /// |workerAffinity| controls how each worker's CPU mask is configured:
  ///   * `Affinity::PerCpu` -- one CPU per worker. Strict; the kernel cannot
  ///     migrate the worker to any other CPU. Best for HPC / real-time use
  ///     where determinism matters more than the kernel's ability to
  ///     opportunistically rebalance.
  ///   * `Affinity::PerCluster` -- each worker is pinned to its CCD's full
  ///     logical-CPU set; the kernel may migrate within a CCD but not
  ///     across clusters. Preserves CCD-locality of caches while letting
  ///     the kernel route wakes via `select_idle_sibling` and rebalance
  ///     under intra-CCD load. Recommended default for memory-bound
  ///     primitives on multi-CCD parts.
  ///   * `Affinity::None` -- no per-worker pinning; workers inherit the
  ///     producer's process affinity mask. The kernel scheduler is free to
  ///     migrate at will.
  ///
  /// Throws `std::invalid_argument` when |participants| is 0; throws
  /// `std::system_error` when pthread attribute init, stack-size set, or
  /// thread create fails.
  explicit ThreadPool(std::size_t participants,
                      Affinity workerAffinity = Affinity::PerCpu)
      : m_workerAffinity(workerAffinity), m_topology(detail::detectTopology()),
        m_workers(nullptr, WorkerArrayDeleter{}),
        m_chainDoneSlots(nullptr, ChainDoneSlotDeleter{}),
        m_plexDoneSlots(nullptr, PlexDoneSlotDeleter{}) {
    if (participants == 0) {
      throw std::invalid_argument("ThreadPool: participants must be >= 1");
    }
    initWorkers(participants, m_topology.physicalCores);
  }

private:
  /// Tag selecting the per-CCD arena constructor used by `PoolGroup`.
  struct ArenaTag {};

  /// Construct an `Arena` pool pinned to a specific subset of CPU ids. Used
  /// exclusively by `PoolGroup` to spawn one pool per CCD. |cpuPins| overrides
  /// the topology-derived physical-core selection so workers stay inside one
  /// CCD; |arenaIndex| is recorded on every worker's `ThreadContext` so the
  /// cross-arena guard can fall through to inline-on-caller without consulting
  /// the `PoolGroup`.
  ThreadPool(ArenaTag /*tag*/, std::size_t participants,
             const std::vector<std::uint32_t> &cpuPins,
             std::uint32_t arenaIndex)
      : m_kind(PoolKind::Arena), m_arenaIndex(arenaIndex),
        m_topology(detail::detectTopology()),
        m_workers(nullptr, WorkerArrayDeleter{}),
        m_chainDoneSlots(nullptr, ChainDoneSlotDeleter{}),
        m_plexDoneSlots(nullptr, PlexDoneSlotDeleter{}) {
    if (participants == 0) {
      throw std::invalid_argument("ThreadPool: participants must be >= 1");
    }
    initWorkers(participants, cpuPins);
  }

  friend class PoolGroup;

  /// TLS auto-pin state per producer thread. Captured by `autoPinProducerOnce`
  /// on the first `parallelFor` from a thread for a given pool, restored by
  /// `autoPinRestoreIfOurs` from the pool's destructor when the destroying
  /// thread is the same producer. The `pool` field is a stable identity tag
  /// (compared, not dereferenced).
  struct AutoPinState {
#ifdef __linux__
    /// Producer's CPU affinity mask before auto-pin pinned it to slot 0's
    /// reserved CPU. Restored by the pool's destructor.
    cpu_set_t saved{};
#endif
    /// Stable identity tag for the pool that captured `saved`. Compared
    /// for equality only; never dereferenced.
    const void *pool = nullptr;
    /// True between the auto-pin save and the matching restore.
    bool restorePending = false;
  };
  /// Returns the calling thread's `AutoPinState` slot. Each producer
  /// thread has one; the pool tag in the slot disambiguates which pool
  /// owns the pin.
  static AutoPinState &autoPinTls() noexcept {
    static thread_local AutoPinState s_state;
    return s_state;
  }

  // Pool-side mirror of the auto-pin record. The destructor uses this to
  // restore the producer's affinity when the destroyer thread is not the
  // producer (in which case the TLS path's `state.pool == this` check would
  // fail and the producer would stay pinned past pool lifetime).
#ifdef __linux__
  /// True while this pool owns an active producer pin. The destructor
  /// reads this with acquire semantics to decide whether to restore.
  std::atomic<bool> m_autoPinActive{false};
  /// `pthread_self()` of the producer that was auto-pinned by this pool.
  pthread_t m_autoPinProducer{};
  /// Producer's pre-pin affinity mask, mirrored from
  /// `AutoPinState::saved` so the destructor can restore it from any
  /// thread.
  cpu_set_t m_autoPinSaved{};
#endif

  /// First-call binder: detect whether the producer is already pinned
  /// (single-CPU mask) and leave it alone if so; otherwise save the current
  /// mask and pin to slot 0's reserved CPU. Idempotent per (thread, pool) via
  /// the TLS pool tag.
  [[gnu::cold]] void autoPinProducerOnce() const noexcept {
#ifdef __linux__
    const std::uint32_t cpuId = producerCpu();
    if (cpuId == UINT32_MAX) {
      return;
    }
    auto &state = autoPinTls();
    cpu_set_t curr;
    CPU_ZERO(&curr);
    if (pthread_getaffinity_np(pthread_self(), sizeof(curr), &curr) != 0) {
      return;
    }
    if (CPU_COUNT(&curr) <= 1) {
      // Producer already pinned. If the pin came from an earlier citor pool on
      // this same thread (state.restorePending == true), DO NOT clobber the
      // saved-affinity record; that record is the only path back to the
      // producer's pre-citor mask. We just take ownership of the pin from the
      // prior pool by retaining `state.saved` and updating `state.pool` to
      // ourselves, so this pool's destructor performs the eventual restore. If
      // the pin came from outside citor (state.restorePending == false), leave
      // saved untouched and just record the pool tag so subsequent calls skip
      // the cost.
      state.pool = static_cast<const void *>(this);
      return;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<std::size_t>(cpuId), &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0) {
      // Only capture `saved` when no prior pool has already done so on this
      // thread. Otherwise the prior pool's saved record is the right one to
      // preserve -- the earlier non-pinned mask, not the intermediate
      // single-CPU mask we are replacing.
      if (!state.restorePending) {
        state.saved = curr;
        state.restorePending = true;
      }
      state.pool = static_cast<const void *>(this);
      // Mirror the pin record on the pool so the destructor can restore
      // even when running on a thread other than the producer. Mutable
      // because the auto-pin runs through a `const` accessor; the
      // destructor reads these fields under the synchronous lifetime
      // contract (no in-flight dispatch races destruction).
      auto *self = const_cast<ThreadPool *>(this);
      self->m_autoPinSaved = state.saved;
      self->m_autoPinProducer = pthread_self();
      self->m_autoPinActive.store(true, std::memory_order_release);
    }
#endif
  }

  /// Per-thread shared `JobDescriptor` storage used by `parallelFor` and
  /// `dispatchReduceJobTyped`. Hoisted out of the template so distinct
  /// lambda types do not each create their own TLS descriptor (256 B
  /// each, aligned to 128 B). With many unique lambdas the
  /// per-template-instantiation cost showed up as TLS bloat across
  /// long-lived threads. The desc is type-erased (its `body` is a
  /// `FunctionRef`), so sharing across instantiations is safe.
  [[gnu::always_inline]] static detail::JobDescriptor &
  sharedParallelForDesc() noexcept {
    thread_local detail::JobDescriptor desc;
    return desc;
  }

  /// Per-thread shared `JobDescriptor` for reduce dispatches. Same TLS
  /// sharing rationale as `sharedParallelForDesc`; kept on a separate
  /// instance so reduce and `parallelFor` do not stomp each other when a
  /// chained call sequence reuses the cached `desc` between dispatches.
  [[gnu::always_inline]] static detail::JobDescriptor &
  sharedReduceDesc() noexcept {
    thread_local detail::JobDescriptor desc;
    return desc;
  }

  /// Hot-path TLS gate: a quick `state.pool == this` check, fall through to
  /// the cold pinner only on a pool transition. Used by every chunk==0
  /// primitive so the syscall fires once per (thread, pool) regardless of
  /// which primitive made the first call.
  [[gnu::always_inline]] void
  ensureProducerPinnedForChunkZero() const noexcept {
    auto &state = autoPinTls();
    if (state.pool != static_cast<const void *>(this)) [[unlikely]] {
      autoPinProducerOnce();
    }
  }

  /// Pool-destruction restore: undoes the auto-pin recorded by
  /// `autoPinProducerOnce`. Uses the pool-side mirror (`m_autoPinProducer`,
  /// `m_autoPinSaved`) so the restore lands on the producer thread regardless
  /// of which thread invokes the destructor. The TLS record is cleared as
  /// well when the destroyer happens to be the producer, so a subsequent
  /// pool ctor on the same thread sees a clean state.
  [[gnu::cold, gnu::noinline]] void autoPinRestoreIfOurs() const noexcept {
#ifdef __linux__
    if (m_autoPinActive.load(std::memory_order_acquire)) {
      (void)pthread_setaffinity_np(m_autoPinProducer, sizeof(m_autoPinSaved),
                                   &m_autoPinSaved);
    }
    auto &state = autoPinTls();
    if (state.pool == static_cast<const void *>(this)) {
      state.restorePending = false;
      state.pool = nullptr;
    }
#endif
  }

  /// Reorders `|cpuPins|` so the producer's reserved CPU sits at index 0.
  /// Standalone pools default to CCD-local placement; the producer pin
  /// follows `topo.preferredCcd` so workers and producer first-touch on
  /// the same NUMA node. Returns the reordered pin list.
  static std::vector<std::uint32_t>
  reserveProducerCpuFirst(const std::vector<std::uint32_t> &cpuPins,
                          PoolKind kind, const detail::Topology &topo) {
    std::vector<std::uint32_t> pins = cpuPins;
#ifdef __linux__
    // Standalone pools default to CCD-local placement: workers spawn on the
    // same CCD as slot 0, and the producer is auto-pinned to slot 0 at
    // construction so user buffers first-touch on that CCD's NUMA node. Picking
    // the CCD by `sched_getcpu()` was bimodal -- on V-Cache the kernel could
    // schedule the constructor on the regular CCD (32 MiB L3) instead of the
    // V-Cache CCD (96 MiB L3) and the workload would spill L3 to DRAM. Pick
    // `topo.preferredCcd`
    // (= largest L3, deterministic tie-break) instead, then keep that CCD's
    // pins first while appending the rest in their detectTopology() order so
    // pools larger than one CCD still span every available core.
    if (kind != PoolKind::Standalone || pins.size() <= 1U) {
      return pins;
    }
    const std::uint32_t targetCcd = topo.preferredCcd;
    if (targetCcd >= topo.ccdGroups.size()) {
      return pins;
    }
    std::vector<std::uint32_t> reordered;
    reordered.reserve(pins.size());
    for (const std::uint32_t cpu : pins) {
      if (cpu < topo.ccdOfCpu.size() && topo.ccdOfCpu[cpu] == targetCcd) {
        reordered.push_back(cpu);
      }
    }
    for (const std::uint32_t cpu : pins) {
      if (cpu >= topo.ccdOfCpu.size() || topo.ccdOfCpu[cpu] != targetCcd) {
        reordered.push_back(cpu);
      }
    }
    if (reordered.size() == pins.size()) {
      pins = std::move(reordered);
    }
#else
    (void)kind;
    (void)topo;
#endif
    return pins;
  }

  /// Shared body for both constructors: spawn |participants| workers pinned to
  /// |cpuPins|.
  void initWorkers(std::size_t participants,
                   const std::vector<std::uint32_t> &cpuPins) {
    const std::vector<std::uint32_t> participantPins =
        reserveProducerCpuFirst(cpuPins, m_kind, m_topology);
    std::size_t maxByTopology = participants;
    if (!participantPins.empty()) {
      maxByTopology = participantPins.size();
    } else if (m_topology.physicalCount > 0U) {
      maxByTopology = m_topology.physicalCount;
    }
    const std::size_t effective =
        participants <= maxByTopology ? participants : maxByTopology;

    m_control.participants = static_cast<std::uint32_t>(effective);
    // Pre-compute the join scan's pending bitmask. The bitmask join path is
    // gated on `participants <= 64` at the call site; for larger pools the
    // mask is unused and join falls back to the per-slot scan over
    // `m_workers`. The construction below clamps to 64 bits to avoid the
    // `1ULL << effective` shift becoming UB when `effective > 64` on
    // many-core machines. Producer slot 0 is excluded so workers `[1, n)`
    // are the only bits set.
    if (effective >= 64U) {
      m_control.pendingMaskBits = ~std::uint64_t{0} & ~std::uint64_t{1};
    } else {
      m_control.pendingMaskBits =
          ((std::uint64_t{1} << effective) - 1U) & ~std::uint64_t{1};
    }

    void *raw = ::operator new(sizeof(detail::WorkerState) * effective,
                               std::align_val_t{kCacheLine});
    auto *first = static_cast<detail::WorkerState *>(raw);
    for (std::size_t i = 0; i < effective; ++i) {
      ::new (static_cast<void *>(first + i)) detail::WorkerState();
    }
    m_workers = std::unique_ptr<detail::WorkerState, WorkerArrayDeleter>(
        first, WorkerArrayDeleter{effective});

    // Pre-allocate the per-worker `done` slot block reused by every
    // `parallelChain` dispatch. The block is owned by the pool so chain calls
    // pay no allocator round trip on the dispatch hot path; each call
    // zero-resets the slots at entry to honour the chain's fresh-epoch
    // contract.
    void *rawSlots = ::operator new(sizeof(detail::ChainDoneSlot) * effective,
                                    std::align_val_t{kCacheLine});
    auto *firstSlot = static_cast<detail::ChainDoneSlot *>(rawSlots);
    for (std::size_t i = 0; i < effective; ++i) {
      ::new (static_cast<void *>(firstSlot + i)) detail::ChainDoneSlot();
    }
    m_chainDoneSlots =
        std::unique_ptr<detail::ChainDoneSlot, ChainDoneSlotDeleter>(
            firstSlot, ChainDoneSlotDeleter{effective});

    // Pre-allocate the per-worker plex `done` slot block. Mirrors the chain
    // block; each plex call borrows it through `state.doneSlots` and reserves a
    // fresh interval in `m_plexEpochBase`, so calls never `operator new` /
    // zero-reset on the dispatch hot path.
    void *rawPlexSlots = ::operator new(
        sizeof(detail::PlexDoneSlot) * effective, std::align_val_t{kCacheLine});
    auto *firstPlexSlot = static_cast<detail::PlexDoneSlot *>(rawPlexSlots);
    for (std::size_t i = 0; i < effective; ++i) {
      ::new (static_cast<void *>(firstPlexSlot + i)) detail::PlexDoneSlot();
    }
    m_plexDoneSlots =
        std::unique_ptr<detail::PlexDoneSlot, PlexDoneSlotDeleter>(
            firstPlexSlot, PlexDoneSlotDeleter{effective});

    for (std::size_t i = 0; i < effective; ++i) {
      auto *w = m_workers.get() + i;
      w->workerId = static_cast<std::uint32_t>(i);
      const std::uint32_t cpu =
          (i < participantPins.size()) ? participantPins[i] : UINT32_MAX;
      w->cpuId = cpu;
      if (cpu != UINT32_MAX && cpu < m_topology.ccdOfCpu.size()) {
        w->ccdId = m_topology.ccdOfCpu[cpu];
      }
    }

    // Per-worker Chase-Lev work-stealing deques. Allocated up-front (one per
    // participant) so the forkJoin steal probe pays no allocator round-trip on
    // its hot path. Each deque stores `Task *` payloads pointing at descriptors
    // that live on the producer's stack (root tasks) or on a recursive worker's
    // stack (children spawned during a body).
    m_workerDeques.reserve(effective);
    for (std::size_t i = 0; i < effective; ++i) {
      m_workerDeques.emplace_back(
          std::make_unique<detail::ChaseLevDeque<detail::Task *>>());
    }

    // Cache the per-slot CCD index in a contiguous array so the forkJoin
    // victim-selection probe skips the indirection through `WorkerState::ccdId`
    // on the steal hot path.
    m_ccdOfSlot.assign(effective, UINT32_MAX);
    for (std::size_t i = 0; i < effective; ++i) {
      m_ccdOfSlot[i] = (m_workers.get() + i)->ccdId;
    }

    // Cache background-worker CPU ids for `bindProducerSlot`'s exclusion logic.
    // Built once at construction so the producer-side `ProducerAffinityGuard`
    // does not allocate per scope.
    m_workerCpus.clear();
    m_workerCpus.reserve(effective > 0 ? effective - 1 : 0);
    for (std::size_t i = 1; i < effective; ++i) {
      const std::uint32_t cpu = (m_workers.get() + i)->cpuId;
      if (cpu != UINT32_MAX) {
        m_workerCpus.push_back(cpu);
      }
    }

    // Precompute steal-victim lists for `forkJoin::trySteal`: the same-CCD ring
    // (used when the call requests CCD-local affinity), the cross-CCD ring (the
    // CCD-local fallback), and the all-victim ring (used when the call did not
    // request CCD-local affinity). Storing slot indices directly removes the
    // per-step `% participants` modulo from the inner steal loop and the
    // per-step CCD comparison; the cross-CCD ring complements the same-CCD ring
    // so the CCD-local fallback never re-probes victims the same-CCD pass
    // already exhausted.
    m_sameCcdVictims.assign(effective, {});
    m_crossCcdVictims.assign(effective, {});
    m_allVictims.assign(effective, {});
    for (std::size_t self = 0; self < effective; ++self) {
      const std::uint32_t selfCcd = m_ccdOfSlot[self];
      m_sameCcdVictims[self].reserve(effective - 1);
      m_crossCcdVictims[self].reserve(effective - 1);
      m_allVictims[self].reserve(effective - 1);
      for (std::size_t v = 0; v < effective; ++v) {
        if (v == self) {
          continue;
        }
        m_allVictims[self].push_back(static_cast<std::uint32_t>(v));
        if (selfCcd != UINT32_MAX && m_ccdOfSlot[v] == selfCcd) {
          m_sameCcdVictims[self].push_back(static_cast<std::uint32_t>(v));
        } else {
          m_crossCcdVictims[self].push_back(static_cast<std::uint32_t>(v));
        }
      }
    }

    if (effective <= 1) {
      return;
    }
#ifdef __linux__
    m_workerThreads.resize(effective - 1);
#endif
    m_workerSpawnArgs.reserve(effective - 1);
    for (std::size_t i = 1; i < effective; ++i) {
      auto &arg = m_workerSpawnArgs.emplace_back();
      arg.pool = this;
      arg.workerIndex = i;
    }

#ifdef __linux__
    pthread_attr_t attrs;
    const int initRc = pthread_attr_init(&attrs);
    if (initRc != 0) {
      shutdownAndJoin();
      throw std::system_error(initRc, std::generic_category(),
                              "ThreadPool: pthread_attr_init");
    }
    // Worker pthread stack size. Build-time configurable via the
    // `CITOR_WORKER_STACK_KIB` CMake option (default 1024 KiB). Sized for
    // recursive forkJoin bodies under TSan/ASan shadow stacks
    // -- a 256 KiB default historically overflowed UTS-style workloads under
    // sanitizer. Production builds with shallow recursion can drop this knob to
    // 256. The attribute is set once and reused for every worker; non-zero
    // return is propagated as `std::system_error` rather than silently ignored.
    // `pthread_attr_setstacksize` may reject sub-`PTHREAD_STACK_MIN` or
    // non-page-multiple sizes, so we surface the rc.
#ifndef CITOR_WORKER_STACK_KIB
#define CITOR_WORKER_STACK_KIB 8192
#endif
    const std::size_t kWorkerStackBytes =
        std::size_t{CITOR_WORKER_STACK_KIB} * 1024U;
    const int stackRc = pthread_attr_setstacksize(&attrs, kWorkerStackBytes);
    if (stackRc != 0) {
      (void)pthread_attr_destroy(&attrs);
      shutdownAndJoin();
      throw std::system_error(stackRc, std::generic_category(),
                              "ThreadPool: pthread_attr_setstacksize");
    }
    // Pre-bind worker affinity in pthread_attr before `pthread_create` so the
    // kernel schedules the new thread directly onto its target CPU. The
    // pre-existing `bindAffinityOnce` in `workerEntry` then becomes a no-op
    // confirmation.
    for (std::size_t i = 1; i < effective; ++i) {
      pthread_attr_t localAttrs;
      const int locInitRc = pthread_attr_init(&localAttrs);
      if (locInitRc != 0) {
        (void)pthread_attr_destroy(&attrs);
        shutdownAndJoin();
        throw std::system_error(locInitRc, std::generic_category(),
                                "ThreadPool: pthread_attr_init");
      }
      const int locStackRc =
          pthread_attr_setstacksize(&localAttrs, kWorkerStackBytes);
      if (locStackRc != 0) {
        (void)pthread_attr_destroy(&localAttrs);
        (void)pthread_attr_destroy(&attrs);
        shutdownAndJoin();
        throw std::system_error(locStackRc, std::generic_category(),
                                "ThreadPool: pthread_attr_setstacksize");
      }
      const std::uint32_t cpu = (m_workers.get() + i)->cpuId;
      if (cpu != UINT32_MAX && m_workerAffinity != Affinity::None) {
        cpu_set_t affSet;
        CPU_ZERO(&affSet);
        if (m_workerAffinity == Affinity::PerCluster) {
          // Pin to every CPU in this worker's CCD (shared L3 group). The
          // kernel can migrate the worker within the CCD but not across
          // clusters; preserves CCD locality while letting `wake_affine`
          // and idle-balance opportunistically rebalance under transient
          // intra-CCD load.
          const std::uint32_t ccdIdx = (m_workers.get() + i)->ccdId;
          if (ccdIdx != UINT32_MAX && ccdIdx < m_topology.ccdGroups.size() &&
              !m_topology.ccdGroups[ccdIdx].empty()) {
            for (auto c : m_topology.ccdGroups[ccdIdx]) {
              CPU_SET(c, &affSet);
            }
          } else {
            CPU_SET(cpu, &affSet);
          }
        } else {
          // Affinity::PerCpu (strict, single-CPU pin). Original behaviour.
          CPU_SET(cpu, &affSet);
        }
        const int affRc =
            pthread_attr_setaffinity_np(&localAttrs, sizeof(affSet), &affSet);
        if (affRc != 0) {
          (void)pthread_attr_destroy(&localAttrs);
          (void)pthread_attr_destroy(&attrs);
          shutdownAndJoin();
          throw std::system_error(affRc, std::generic_category(),
                                  "ThreadPool: pthread_attr_setaffinity_np");
        }
      }
      const int rc =
          pthread_create(&m_workerThreads[i - 1], &localAttrs,
                         &ThreadPool::workerEntry, &m_workerSpawnArgs[i - 1]);
      (void)pthread_attr_destroy(&localAttrs);
      if (rc != 0) {
        m_workerThreads.resize(i - 1);
        m_workerSpawnArgs.resize(i - 1);
        (void)pthread_attr_destroy(&attrs);
        shutdownAndJoin();
        throw std::system_error(rc, std::generic_category(),
                                "ThreadPool: pthread_create");
      }
    }
    (void)pthread_attr_destroy(&attrs);
#else
    m_fallbackThreads.reserve(effective - 1);
    for (std::size_t i = 1; i < effective; ++i) {
      try {
        m_fallbackThreads.emplace_back(&ThreadPool::workerEntryStdThread, this,
                                       i);
      } catch (...) {
        shutdownAndJoin();
        throw;
      }
    }
#endif
    // Constructor barrier: spin until every worker has finished its trampoline
    // and is committed to entering `workerMainLoop`. Without this, the first
    // dispatch's join can wait on a worker that has not yet been scheduled past
    // its startup, which inflates cold-fan-out latency.
    const auto workersExpected = static_cast<std::uint32_t>(effective - 1);
    while (m_workersReady.load(std::memory_order_acquire) < workersExpected) {
      detail::cpuRelax();
    }
    // Pin the constructor (= producer) to slot 0's CPU NOW, before the caller
    // has a chance to allocate workload buffers. Otherwise the buffers
    // first-touch on whichever CPU the kernel happened to schedule the
    // constructor on, while workers may sit on a different CCD; on a 2-CCD
    // host that thrashes the IO die for every cache line and the workload
    // stalls 5-10x. Standalone pools only -- Arena pools are owned by
    // PoolGroup and the producer is shared across arenas, so a per-arena pin
    // would be wrong.
    if (m_kind == PoolKind::Standalone) {
      autoPinProducerOnce();
    }
    // One-time coherence probe. Measures the constant factor by which a
    // cache-line ping-pong between two cores on different CCDs is slower
    // than a ping-pong between two cores on the same CCD. Primitives that
    // benefit from CCD-aware partitioning (`parallelScan`, future
    // tile-decoupled scans, ...) read this ratio to size their cross-CCD
    // share without any hardware-specific constants in the engine.
    //
    // Skipped on single-CCD topologies (the probe would be a no-op),
    // skipped on `PoolKind::Arena` (the parent `PoolGroup` is responsible
    // for any cross-arena cost model -- per-arena probes would be both
    // wasted work and biased by the arena's narrower CPU set).
    if (m_kind == PoolKind::Standalone && m_topology.ccdCount > 1U) {
      // Build the flat CPU list the probe should walk: producer CPU plus
      // every worker's pinned CPU. Skip CPUs we could not pin (UINT32_MAX
      // sentinel from `initWorkers` on a permission-restricted host) so
      // the probe never tries to thread-spawn on a CPU it cannot reach.
      std::vector<std::uint32_t> probeCpus;
      probeCpus.reserve(effective);
      for (std::size_t i = 0; i < effective; ++i) {
        const std::uint32_t cpu = (m_workers.get() + i)->cpuId;
        if (cpu != UINT32_MAX) {
          probeCpus.push_back(cpu);
        }
      }
      // 1024 round-trips per pair amortises ping-pong jitter; the probe
      // runs in N-1 disjoint-pair-parallel rounds so wall time scales
      // with (cpus - 1) * single-pair-probe-time. On a single-CCD probe
      // the histogram is unimodal and `clusterByLatency` falls back to
      // the sysfs prior.
      m_coherenceProbe =
          detail::runCoherenceProbe(probeCpus, m_topology.ccdGroups);
    }
    // Pre-resolve the parallelScan-specific topology fields. Inputs are all
    // pool-immutable post-ctor (slot CCDs, cpu ids, probe cluster ids and
    // ratio); resolving them here turns the per-call topology resolution in
    // `runScanParallel` into O(1) field reads.
    initScanScratch(effective);
    // Best-effort lock the WorkerState array into RAM. The hot-path `mailbox` /
    // `doneEpoch` lines must never page-fault: a fault on the producer's
    // acquire-spin would replace cache traffic with a kernel round-trip and the
    // join would silently stall. The locked region is small enough for normal
    // process limits. EPERM (no CAP_IPC_LOCK) and ENOMEM (rlimit exhausted) are
    // non-fatal: the pool runs correctly without the lock, just exposed to the
    // page-fault tail risk.
#ifdef __linux__
    if (m_workers != nullptr) {
      (void)mlock(m_workers.get(), sizeof(detail::WorkerState) * effective);
    }
#elif defined(_WIN32)
    // Windows peer of `mlock`. `VirtualLock` ensures the page is resident
    // in physical RAM so a worker that wakes mid-dispatch never pays a
    // page-fault penalty observing its mailbox. The locked region is
    // small (a few cache lines per worker); failures (e.g. process
    // working-set quota too tight) are non-fatal.
    if (m_workers != nullptr) {
      (void)::VirtualLock(m_workers.get(),
                          sizeof(detail::WorkerState) * effective);
    }
#endif
  }

  /// Pre-resolve the parallelScan-specific topology fields. Inputs are all
  /// pool-immutable post-ctor: the per-slot CCD vector, each worker's
  /// `cpuId`, the coherence probe's matrix CPU list, the probe's per-CPU
  /// cluster ids, and the probe's max cross-over-intra ratio. Output is
  /// stored on the pool so the scan path can replace its per-call topology
  /// resolution (slot -> cluster mapping, contiguity check, asymmetric-bias
  /// derivation) with O(1) field reads.
  void initScanScratch(std::size_t participants) noexcept {
    m_scanClusterIdOfSlot.assign(participants, 0U);
    m_scanClusterFirstSlot.clear();
    m_scanClusterSlotCount.clear();
    m_scanNumClusters = 0;
    m_scanUseHierarchical = false;
    m_scanAsymmetricNum = 8U;
    m_scanProducerCcd = UINT32_MAX;
    m_scanSlotsOnProducerCcd = 0;

    if (m_ccdOfSlot.empty()) {
      return;
    }
    const std::uint32_t producerCcd = m_ccdOfSlot[0];
    std::uint32_t slotsOnProducer = 0;
    bool foundCross = false;
    for (std::size_t s = 0; s < participants; ++s) {
      if (s < m_ccdOfSlot.size() && m_ccdOfSlot[s] == producerCcd) {
        ++slotsOnProducer;
      } else {
        foundCross = true;
      }
    }
    m_scanProducerCcd = producerCcd;
    m_scanSlotsOnProducerCcd = slotsOnProducer;

    if (foundCross && slotsOnProducer > 0U && slotsOnProducer < participants) {
      const std::uint32_t crossSlots =
          static_cast<std::uint32_t>(participants) - slotsOnProducer;
      const double biasFactor =
          m_coherenceProbe.valid &&
                  m_coherenceProbe.maxCrossOverIntraRatio > 1.0
              ? m_coherenceProbe.maxCrossOverIntraRatio
              : 2.0;
      const auto producerWeight = static_cast<std::uint64_t>(
          std::llround(biasFactor * static_cast<double>(slotsOnProducer)));
      const std::uint64_t totalWeight =
          producerWeight + static_cast<std::uint64_t>(crossSlots);
      std::uint32_t derived = 8U;
      if (totalWeight > 0U) {
        derived =
            static_cast<std::uint32_t>((16ULL * producerWeight) / totalWeight);
      }
      m_scanAsymmetricNum =
          std::clamp(derived, std::uint32_t{9U}, std::uint32_t{15U});
    }

    // Hierarchical requires: probe found multiple clusters, enough
    // participants to amortise the per-cluster reduce, and crucially that
    // slots actually span more than one CCD. The last gate guards against
    // false-positive clustering when every worker sits on a single CCD --
    // a 4-CPU same-CCD probe has occasionally clustered into 2-3 spurious
    // groups due to per-pair latency jitter, and routing the scan through
    // the multi-cluster path under that condition deadlocks on the exception
    // path (a non-leader cluster's leader can throw before publishing its
    // cluster stamp, blocking the producer's cross-cluster wait).
    if (!m_coherenceProbe.valid || m_coherenceProbe.clusters.numClusters < 2U ||
        participants < 4U || !foundCross || slotsOnProducer >= participants) {
      return;
    }

    bool resolveOk = true;
    for (std::size_t s = 0; s < participants; ++s) {
      const std::uint32_t cpuId = (m_workers.get() + s)->cpuId;
      std::uint32_t cpuIdx = UINT32_MAX;
      for (std::size_t i = 0; i < m_coherenceProbe.matrix.cpus.size(); ++i) {
        if (m_coherenceProbe.matrix.cpus[i] == cpuId) {
          cpuIdx = static_cast<std::uint32_t>(i);
          break;
        }
      }
      if (cpuIdx == UINT32_MAX) {
        resolveOk = false;
        break;
      }
      m_scanClusterIdOfSlot[s] =
          m_coherenceProbe.clusters.clusterIdOfCpuIndex[cpuIdx];
    }
    if (!resolveOk) {
      m_scanClusterIdOfSlot.assign(participants, 0U);
      return;
    }

    const std::uint32_t numClusters = m_coherenceProbe.clusters.numClusters;
    m_scanClusterFirstSlot.assign(numClusters, UINT32_MAX);
    m_scanClusterSlotCount.assign(numClusters, 0U);
    for (std::size_t s = 0; s < participants; ++s) {
      const auto k = m_scanClusterIdOfSlot[s];
      if (k >= numClusters) {
        return;
      }
      if (m_scanClusterFirstSlot[k] == UINT32_MAX) {
        m_scanClusterFirstSlot[k] = static_cast<std::uint32_t>(s);
      }
      ++m_scanClusterSlotCount[k];
    }
    bool contiguous = true;
    for (std::uint32_t k = 0; k < numClusters; ++k) {
      if (m_scanClusterSlotCount[k] == 0U) {
        continue;
      }
      const std::uint32_t first = m_scanClusterFirstSlot[k];
      for (std::uint32_t i = 0; i < m_scanClusterSlotCount[k]; ++i) {
        if (first + i >= participants ||
            m_scanClusterIdOfSlot[first + i] != k) {
          contiguous = false;
          break;
        }
      }
      if (!contiguous) {
        break;
      }
    }
    std::uint32_t nonEmpty = 0;
    for (std::uint32_t k = 0; k < numClusters; ++k) {
      if (m_scanClusterSlotCount[k] > 0U) {
        ++nonEmpty;
      }
    }
    const bool producerInClusterZero = m_scanClusterIdOfSlot[0] == 0U;
    if (contiguous && nonEmpty >= 2U && producerInClusterZero) {
      m_scanUseHierarchical = true;
      m_scanNumClusters = numClusters;
    }
  }

public:
  // Scoped affinity guard for the caller acting as the pool's producer slot.
  //
  // Workers are pinned to participant slots 1..N. Slot 0 is the caller, so a
  // caller that wants full-core participation without displacing a pinned
  // worker can bind itself to the CPU reserved for slot 0 outside a hot
  // dispatch loop. The guard restores the previous affinity mask on
  // destruction.
  class ProducerAffinityGuard {
  public:
    ProducerAffinityGuard() noexcept = default;

    /// Saves the caller's affinity mask and pins the caller to `|cpuId|`
    /// (or, when SMT siblings are exposed, to the same-CCD subset of the
    /// caller's mask excluding `|workerCpus|`). The destructor restores
    /// the saved mask. `|workerCpus|` and `|workerCpuCount|` describe the
    /// pool's worker pin list so the producer never displaces a hot
    /// worker.
    ProducerAffinityGuard(std::uint32_t cpuId, const detail::Topology &topo,
                          const std::uint32_t *workerCpus,
                          std::size_t workerCpuCount) noexcept {
#ifdef __linux__
      if (cpuId == UINT32_MAX) {
        return;
      }
      CPU_ZERO(&m_saved);
      if (pthread_getaffinity_np(pthread_self(), sizeof(m_saved), &m_saved) !=
          0) {
        return;
      }
      // When the target CPU's SMT sibling is also inside the current mask, the
      // SMT pair is closed (no out-of-mask kthread can land on the sibling).
      // Pin to the single CPU. Otherwise the sibling is exposed to
      // kthreads/IRQs and a single-CPU pin would be bimodal under SMT
      // contention. Fall back to the same-CCD subset of the current mask,
      // excluding worker-pinned CPUs so the producer never displaces a
      // hot-spinning worker off its own CPU while the kernel migrates the
      // producer freely inside the worker-free CCD subset to dodge transient
      // SMT collisions.
      const std::vector<std::uint32_t> siblings = detail::readCpuList(
          "/sys/devices/system/cpu/cpu" + std::to_string(cpuId) +
          "/topology/thread_siblings_list");
      bool siblingExposed = false;
      cpu_set_t set;
      for (const std::uint32_t sib : siblings) {
        if (sib == cpuId) {
          continue;
        }
        if (CPU_ISSET(static_cast<std::size_t>(sib), &m_saved)) {
          continue;
        }
        siblingExposed = true;
        break;
      }
      CPU_ZERO(&set);
      if (!siblingExposed) {
        CPU_SET(static_cast<std::size_t>(cpuId), &set);
      } else if (cpuId < topo.ccdOfCpu.size()) {
        const std::uint32_t ccd = topo.ccdOfCpu[cpuId];
        if (ccd < topo.ccdGroups.size()) {
          for (const std::uint32_t peer : topo.ccdGroups[ccd]) {
            if (!CPU_ISSET(static_cast<std::size_t>(peer), &m_saved)) {
              continue;
            }
            bool isWorkerPin = false;
            for (std::size_t i = 0; i < workerCpuCount; ++i) {
              if (workerCpus[i] == peer) {
                isWorkerPin = true;
                break;
              }
            }
            if (isWorkerPin) {
              continue;
            }
            CPU_SET(static_cast<std::size_t>(peer), &set);
          }
        }
        if (CPU_COUNT(&set) == 0) {
          // Every CCD peer is either out-of-mask or worker-pinned; fall back to
          // the single-CPU pin on the producer's reserved slot.
          CPU_SET(static_cast<std::size_t>(cpuId), &set);
        }
      } else {
        return;
      }
      if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0) {
        m_restore = true;
      }
#else
      (void)cpuId;
      (void)topo;
      (void)workerCpus;
      (void)workerCpuCount;
#endif
    }

    ~ProducerAffinityGuard() {
#ifdef __linux__
      if (m_restore) {
        (void)pthread_setaffinity_np(pthread_self(), sizeof(m_saved), &m_saved);
      }
#endif
    }

    ProducerAffinityGuard(const ProducerAffinityGuard &) = delete;
    ProducerAffinityGuard &operator=(const ProducerAffinityGuard &) = delete;
    ProducerAffinityGuard(ProducerAffinityGuard &&) = delete;
    ProducerAffinityGuard &operator=(ProducerAffinityGuard &&) = delete;

  private:
#ifdef __linux__
    /// Caller's affinity mask captured at construction; restored by the
    /// destructor when `m_restore` is true.
    cpu_set_t m_saved{};
    /// True when the constructor successfully applied a new pin and the
    /// destructor must restore `m_saved`.
    bool m_restore = false;
#endif
  };

  /// Per-thread depth counter for `LowLatencyGuard`. Lets `DispatchLease`
  /// distinguish "this thread owns an active LL guard" from "some other
  /// thread does". Only the owning thread can safely skip the dispatch
  /// mutex, since the LL contract is single-producer per pool. Other
  /// threads still take the mutex and serialize against the LL holder.
  static std::uint32_t &lowLatencyOwnerDepthTls() noexcept {
    static thread_local std::uint32_t s_depth = 0;
    return s_depth;
  }

  // Scoped mode for latency-sensitive producer loops.
  //
  // While at least one guard is alive, idle workers keep spinning instead of
  // parking and dispatch skips the per-call futex wake. The constructor wakes
  // parked workers once; the destructor restores the normal spin-then-park
  // policy.
  class LowLatencyGuard {
  public:
    LowLatencyGuard() noexcept = default;

    /// Engages low-latency mode on `|pool|`. Bumps the pool's hot-spin
    /// epoch, wakes parked workers, and waits for every worker to
    /// acknowledge the new epoch. The destructor restores the normal
    /// spin-then-park policy.
    explicit LowLatencyGuard(ThreadPool &pool) noexcept : m_pool(&pool) {
      ++lowLatencyOwnerDepthTls();
      auto &control = m_pool->m_control;
      const std::uint64_t epoch =
          control.hotSpinEpoch.fetch_add(1U, std::memory_order_acq_rel) + 1U;
      control.hotSpinDepth.fetch_add(1U, std::memory_order_acq_rel);
      const auto wakeWorkers = [&control]() noexcept {
        const std::uint32_t nextFutex =
            control.futexWord.load(std::memory_order_relaxed) + 1U;
        control.futexWord.store(nextFutex, std::memory_order_release);
        (void)detail::futexWakePrivate(&control.futexWord, INT_MAX);
      };
      wakeWorkers();
      auto *workers = m_pool->m_workers.get();
      const std::uint32_t participants = control.participants;
      // Workers store the LATEST observed `control.hotSpinEpoch` into their
      // slot. When two guard ctors stack, worker storage is monotonic in the
      // global epoch, so the loser's exact-equality wait would deadlock on a
      // slot already advanced past it. Wait for
      // `>= epoch` so any worker that has seen our epoch (or a later one)
      // satisfies the ack. The contract a guard cares about is "every worker
      // has acknowledged hotSpin mode at least as recently as my engagement,"
      // which `>=` captures exactly.
      //
      // The constructor is setup, not dispatch. If a worker is descheduled
      // during the transition, a pure spin can block the producer from giving
      // the scheduler a chance to run the worker that must publish the ack.
      // Retry the wake and yield after bounded spin batches.
      constexpr std::uint32_t kAckSpinBatch = 256U;
      for (std::uint32_t slot = 1; slot < participants; ++slot) {
        std::uint32_t spins = 0;
        while ((workers + slot)->hotSpinEpoch.load(std::memory_order_acquire) <
               epoch) {
          detail::cpuRelax();
          if ((++spins & (kAckSpinBatch - 1U)) == 0U) {
            wakeWorkers();
            std::this_thread::yield();
          }
        }
      }
    }

    ~LowLatencyGuard() {
      if (m_pool != nullptr) {
        m_pool->m_control.hotSpinDepth.fetch_sub(1U, std::memory_order_acq_rel);
        --lowLatencyOwnerDepthTls();
      }
    }

    LowLatencyGuard(const LowLatencyGuard &) = delete;
    LowLatencyGuard &operator=(const LowLatencyGuard &) = delete;
    LowLatencyGuard(LowLatencyGuard &&) = delete;
    LowLatencyGuard &operator=(LowLatencyGuard &&) = delete;

  private:
    /// Pool the guard engaged hot-spin mode on. Null when the guard was
    /// default-constructed.
    ThreadPool *m_pool = nullptr;
  };

  // Destroy the pool: drain pending work, signal shutdown, wake every worker,
  // then join. Detached tasks are drained first so state captured by a
  // still-running body is torn down inside the body, not under us. The
  // synchronous-worker shutdown path runs only after the detached counter
  // reaches zero.
  ~ThreadPool() {
    waitForDetachedDrain();
    shutdownAndJoin();
    autoPinRestoreIfOurs();
  }

  // Pools own raw pthreads with interior pointers; copy/move would invalidate
  // them mid-flight.
  ThreadPool(const ThreadPool &) = delete;
  ThreadPool &operator=(const ThreadPool &) = delete;
  ThreadPool(ThreadPool &&) = delete;
  ThreadPool &operator=(ThreadPool &&) = delete;

  /// Effective number of participants (producer + background workers); always
  /// at least 1.
  [[nodiscard]] std::size_t participants() const noexcept {
    return m_control.participants;
  }

  /// CPU reserved for the caller's slot-0 producer participation.
  /// Returns `UINT32_MAX` when affinity pinning is unavailable. Stable for the
  /// pool's lifetime; target used by `bindProducerSlot`.
  [[nodiscard]] std::uint32_t producerCpu() const noexcept {
    return m_workers ? m_workers->cpuId : UINT32_MAX;
  }

  /// Bind the calling thread to the pool's slot-0 CPU until the returned guard
  /// is destroyed. The dispatch hot path never changes caller affinity, but
  /// benchmarks and long-lived producer threads can opt into the same placement
  /// contract the background workers use.
  [[nodiscard]] ProducerAffinityGuard bindProducerSlot() const noexcept {
    return {producerCpu(), m_topology, m_workerCpus.data(),
            m_workerCpus.size()};
  }

  /// Read the pool's diagnostic counters as a non-atomic snapshot.
  ///
  /// Each field loads with `std::memory_order_relaxed`; the snapshot is
  /// approximate and not consistent across fields beyond per-counter
  /// monotonicity. Counters are cumulative for the pool's lifetime.
  ///
  /// Pool-level fields (`dispatches`, `inlineFallbacks`, `cancellationStops`)
  /// are zero unless `CITOR_ENABLE_POOL_COUNTERS` is defined; when off, the
  /// dispatch hot path pays no extra atomics. Worker-aggregated fields
  /// (`futexParks`, `futexWakes`, `stealAttempts`, `stealSuccesses`) are always
  /// populated.
  [[nodiscard]] detail::PoolCountersSnapshot snapshotCounters() const noexcept {
    detail::PoolCountersSnapshot s;
#ifdef CITOR_ENABLE_POOL_COUNTERS
    s.dispatches = m_counters.dispatches.load(std::memory_order_relaxed);
    s.inlineFallbacks =
        m_counters.inlineFallbacks.load(std::memory_order_relaxed);
    s.cancellationStops =
        m_counters.cancellationStops.load(std::memory_order_relaxed);
#endif
    // Worker-aggregated counters are always available because `WorkerState`
    // carries them unconditionally (they fire only on park/wake/steal events,
    // not on every dispatch).
    const std::size_t n = m_control.participants;
    for (std::size_t i = 0; i < n; ++i) {
      const auto *w = m_workers.get() + i;
      s.futexParks += w->parks.load(std::memory_order_relaxed);
      s.futexWakes += w->wakes.load(std::memory_order_relaxed);
      s.stealAttempts += w->stealAttempts.load(std::memory_order_relaxed);
      s.stealSuccesses += w->stealSuccesses.load(std::memory_order_relaxed);
    }
    return s;
  }

  /// Keep workers hot for a scoped latency-sensitive producer loop. Use around
  /// a burst of synchronous primitive calls when the caller prefers CPU burn
  /// over the futex wake round trip. The returned guard must outlive the
  /// dispatch burst.
  [[nodiscard]] LowLatencyGuard lowLatencyScope() noexcept {
    const auto &ctx = tlsContext();
    if (ctx.insidePoolWorker && ctx.pool == this) {
      return {};
    }
    return LowLatencyGuard(*this);
  }

  /// Number of CCD (or shared-L3) groups detected by the topology probe;
  /// always at least 1 on a host with one physical core.
  [[nodiscard]] std::uint32_t ccdCount() const noexcept {
    return m_topology.ccdCount;
  }

  /// Origin tag of this pool: `Standalone` (user-owned) or `Arena`
  /// (`PoolGroup`-owned).
  ///
  /// Read at the entry of every primitive's dispatch path: when the calling
  /// thread is a worker on a different `Arena` pool, the dispatch falls
  /// through to inline-on-caller execution to avoid the cross-arena deadlock
  /// that would result from a worker blocking on another arena's queue.
  [[nodiscard]] PoolKind kind() const noexcept { return m_kind; }

  /// Index of this pool inside its owning `PoolGroup`. Returns `0` for
  /// `Standalone` pools and for the first arena. Used as the participant token
  /// carried in `ThreadContext::arenaIndex` so the cross-arena guard can
  /// compare without holding a back-pointer to the `PoolGroup`.
  [[nodiscard]] std::uint32_t arenaIndex() const noexcept {
    return m_arenaIndex;
  }

  /// Slot index of the calling thread within any pool. Returns 0 when the
  /// calling thread is not currently inside a pool worker body.
  [[nodiscard]] static std::size_t workerIndex() noexcept {
    return tlsContext().slot;
  }

  /// True when the calling thread is currently inside a pool worker body.
  [[nodiscard]] static bool insidePoolWorker() noexcept {
    return tlsContext().insidePoolWorker;
  }

  /// Calling thread's arena participant token. Used by
  /// `PoolGroup::localArena()` to pick the arena the calling thread is
  /// currently a worker of. Returns `static_cast<std::uint32_t>(-1)` when the
  /// thread is not a worker on any `Arena` pool.
  [[nodiscard]] static std::uint32_t currentArenaIndexHint() noexcept {
    const auto &ctx = tlsContext();
    if (!ctx.insidePoolWorker || ctx.kind != PoolKind::Arena) {
      return static_cast<std::uint32_t>(-1);
    }
    return ctx.arenaIndex;
  }

  /// Run |fn| over `[first, last)` in parallel using the policy from |HintsT|.
  ///
  /// The body is invoked once per block as `fn(blockFirst, blockAfterLast)`.
  /// Synchronous: returns only after every block has completed (or after the
  /// first thrown exception is propagated).
  ///
  /// - `Balance::StaticUniform` runs the worker-strided block partition (no
  ///   atomics on the hot path); `Balance::DynamicChunked` races on the
  ///   relaxed `nextBlock` counter; the other two tiers fall back to dynamic.
  /// - `chunk == 0` derives a default from `(last - first) / participants`.
  /// - `n * estimatedItemNs * 1e-3 < minTaskUs * participants` runs the body
  ///   inline on the producer's thread; no worker is woken.
  /// The descriptor keeps the previous callable address between dispatches;
  /// it is overwritten before reuse and never dereferenced after the
  /// synchronous call returns. `tok` stays by value so owned tokens can move
  /// into the descriptor without a shared_ptr retain.
  /// NOLINTBEGIN(performance-unnecessary-value-param,
  /// clang-analyzer-core.StackAddressEscape)
  template <class HintsT, class F>
  [[gnu::hot, gnu::flatten]] void
  parallelFor(std::size_t first, std::size_t last, F &&fn,
              CancellationToken tok = CancellationToken{}) {
    if (last <= first) {
      return;
    }
    if constexpr (detail::kCancellationActive<HintsT>) {
      if (!tok.canStop()) {
        using NoCancellationHintsT = detail::NoCancellationHints<HintsT>;
        parallelFor<NoCancellationHintsT>(first, last, std::forward<F>(fn),
                                          CancellationToken{});
        return;
      }
    }
    if (shouldFallThroughCrossArena()) {
      const auto &ctx = tlsContext();
      // Nested same-pool path is safe for both throwing and noexcept
      // bodies. The forkJoinAll nested dispatch handles exception capture
      // through the standard ForkJoinState slot, and noexcept bodies just
      // bypass the catch frames entirely. Earlier code disabled noexcept
      // here as `!is_nothrow_invocable_v<F&, ...>` which silently
      // collapsed noexcept nested loops to single-threaded inline.
      constexpr bool kAllowNestedSamePool = true;
      if constexpr (kAllowNestedSamePool) {
        if (ctx.pool == this) {
          const std::size_t n = last - first;
          const std::size_t participants = m_control.participants;
          if (detail::shouldRunInlineHinted<HintsT>(n, participants)) {
            runInlineChunked(first, last, HintsT::chunk,
                             detail::kCancellationActive<HintsT>,
                             std::forward<F>(fn), tok);
            return;
          }

          constexpr bool kCancellationActiveForNestedToken =
              detail::kCancellationActive<HintsT>;

          std::size_t chunk = HintsT::chunk;
          if (chunk == 0) {
            chunk = ceilDiv(n, participants * 2U);
            if (chunk == 0) {
              chunk = 1;
            }
          }
          const std::size_t blockCount = ceilDiv(n, chunk);
          if (blockCount <= 1U) {
            runInline(first, last, std::forward<F>(fn), tok);
            return;
          }
          auto nestedBody = [&](std::size_t block) {
            if constexpr (kCancellationActiveForNestedToken) {
              if (tok.stop_requested()) {
                return;
              }
            }
            const std::size_t lo = first + (block * chunk);
            const std::size_t hi = std::min(lo + chunk, last);
            if (lo < hi) {
              fn(lo, hi);
            }
          };
          forkJoinAll<HintsT>(blockCount, nestedBody);
          return;
        }
      }
      runInlineChunked(first, last, HintsT::chunk,
                       detail::kCancellationActive<HintsT>, std::forward<F>(fn),
                       tok);
      return;
    }
    const std::size_t n = last - first;
    const std::size_t participants = m_control.participants;

    if (detail::shouldRunInlineHinted<HintsT>(n, participants)) {
      runInlineChunked(first, last, HintsT::chunk,
                       detail::kCancellationActive<HintsT>, std::forward<F>(fn),
                       tok);
      return;
    }

    constexpr bool kCancellationActiveForToken =
        detail::kCancellationActive<HintsT>;

    // Lazy producer-CPU pin for partition shapes that put real work on slot 0.
    // Auto-derived chunks (`chunk == 0`) signal "split the range across all
    // participants" -- the slot-0 block is the same size as a worker block, and
    // producer drift onto a worker-pinned CPU halves slot 0 via SMT/scheduler
    // contention.
    if constexpr (HintsT::chunk == 0) {
      ensureProducerPinnedForChunkZero();
    }

    // `static thread_local` lifts the descriptor out of the stack frame:
    // storage allocated once per thread, atomics keep their value across calls
    // (no per-dispatch zero-init storm), address stable so the descriptor body
    // line stays warm in producer L1 across back-to-back dispatches. Descriptor
    // write elision: keep desc cache line in MESI-Shared state across
    // back-to-back dispatches when fields are unchanged (steady-state bench
    // loop, where stack-allocated wrapper has stable address, hints are
    // constexpr, and range/chunk are loop-invariant). Each `if (desc.X != X)`
    // guard replaces an unconditional store with a read+predicted-branch; the
    // producer's L1 read keeps the line Shared instead of dirtying it to
    // Modified, so workers observe the descriptor body line without forcing a
    // Modified-to-Shared coherence transition.
    //
    // `dispatchOneStaticLockedBody` writes nullptr into firstException after
    // capturing and rethrowing any exception, so the slot is null at every call
    // entry.
    detail::JobDescriptor &desc = sharedParallelForDesc();
    auto *fnAddr =
        const_cast<void *>(static_cast<const void *>(std::addressof(fn)));

    // Piggyback same-command reuse detection on the existing write-elision
    // guards: each guard already compares; if every guard takes the "no-write"
    // branch the dispatch's parameters match the previous one and we can
    // publish with kReuseBit set so workers skip desc reads.
    bool keyMatches = true;
    if (desc.first != first) {
      desc.first = first;
      keyMatches = false;
    }
    if (desc.last != last) {
      desc.last = last;
      keyMatches = false;
    }
    if (desc.participants != participants) {
      desc.participants = static_cast<std::uint32_t>(participants);
      keyMatches = false;
    }
    // desc.balance is consulted only by runActiveJob's runtime balance switch
    // (used by the untyped worker fallback). Under StaticUniform with the typed
    // worker entry, both the worker and slot-0 paths are template-dispatched
    // and never read this field; elide the compare/store.
    if constexpr (HintsT::balance != Balance::StaticUniform) {
      if (desc.balance != HintsT::balance) {
        desc.balance = HintsT::balance;
        keyMatches = false;
      }
    }
    if (desc.priority != HintsT::priority) {
      desc.priority = HintsT::priority;
      keyMatches = false;
    }
    if (!desc.preWakeCompletionProbe) {
      desc.preWakeCompletionProbe = true;
      keyMatches = false;
    }
    // desc.body is unused under the typed StaticUniform / DynamicChunked
    // entries: workers read desc->fnPtr (typed runner) and slot-0 calls *slot0
    // directly. Elide FunctionRef construction and compare/store on those
    // paths. Fallback balances still need desc.body for runActiveJob.
    if constexpr (HintsT::balance != Balance::StaticUniform &&
                  HintsT::balance != Balance::DynamicChunked) {
      const FunctionRef<void(std::size_t, std::size_t)> body{fn};
      if (desc.body != body) {
        desc.body = body;
        keyMatches = false;
      }
    }
    // Cancellation-off compile-time elision: when HintsT::cancellationChecks is
    // false neither the worker runner nor slot-0 read desc.token, so the
    // producer's compare+move is dead.
    if constexpr (kCancellationActiveForToken) {
      if (desc.token != tok) {
        desc.token = std::move(tok);
        keyMatches = false;
      }
    } else {
      (void)tok;
    }

    if constexpr (HintsT::balance == Balance::StaticUniform) {
      if (desc.fnPtr != fnAddr) {
        desc.fnPtr = fnAddr;
        keyMatches = false;
      }
      auto *expectedEntry =
          &detail::typedStaticUniformWorkerEntry<HintsT,
                                                 std::remove_reference_t<F>>;
      if (desc.workerEntry != expectedEntry) {
        desc.workerEntry = expectedEntry;
        keyMatches = false;
      }
      // Producer cold-collapse opt-in: hand the worker entry the address of
      // this pool's `WorkerState` array so the CAS-claim path can resolve
      // `claimedAt` per rank. The pointer is pool-stable across this thread's
      // reuse window; the keyMatches gate elides the store on back-to-back
      // same-pool dispatches.
      auto *workersRaw = static_cast<void *>(m_workers.get());
      if (desc.workerStateBase != workersRaw) {
        desc.workerStateBase = workersRaw;
        keyMatches = false;
      }
    } else if constexpr (HintsT::balance == Balance::DynamicChunked) {
      if (desc.fnPtr != fnAddr) {
        desc.fnPtr = fnAddr;
        keyMatches = false;
      }
      auto *expectedEntry =
          &detail::typedDynamicChunkedWorkerEntry<HintsT,
                                                  std::remove_reference_t<F>>;
      if (desc.workerEntry != expectedEntry) {
        desc.workerEntry = expectedEntry;
        keyMatches = false;
      }
      // DynamicChunked opts into cold-collapse: the producer's join-wait can
      // stamp parked ranks directly via `tryClaimRank` after slot-0 drains
      // the shared atomic counter.
      auto *workersRaw = static_cast<void *>(m_workers.get());
      if (desc.workerStateBase != workersRaw) {
        desc.workerStateBase = workersRaw;
        keyMatches = false;
      }
    } else {
      if (desc.fnPtr != nullptr) {
        desc.fnPtr = nullptr;
        keyMatches = false;
      }
      if (desc.workerEntry != nullptr) {
        desc.workerEntry = nullptr;
        keyMatches = false;
      }
      // Steal balance does not engage cold-collapse (workers re-enter via
      // their local deque rather than a single counter).
      if (desc.workerStateBase != nullptr) {
        desc.workerStateBase = nullptr;
        keyMatches = false;
      }
    }
    if (!keyMatches || desc.chunk == 0U || desc.blockCount == 0U) {
      const std::size_t hintChunk = HintsT::chunk;
      // Static doesn't steal, so 1x oversubscription gives each rank a
      // single contiguous range. Dynamic needs 2x so a fast worker can
      // absorb a straggler's tail.
      constexpr bool kOversubscribe =
          HintsT::balance == Balance::DynamicChunked;
      fillBlockShape(desc, n, participants, hintChunk, kOversubscribe);
    }

    if constexpr (HintsT::balance == Balance::DynamicChunked) {
      // Phase B atomic tail starts at `participants`; Phase A's rank-strided
      // assignment owns `[0, participants)`. Defer the store until after
      // `fillBlockShape` so we can skip it when the dispatch will collapse
      // to inline (`blockCount <= 1`) or when there is no oversubscription
      // (`blockCount <= participants`, where Phase B is never read).
      if (desc.blockCount > participants) {
        desc.nextBlock.store(participants, std::memory_order_relaxed);
      }
    }

    if (desc.blockCount <= 1U) {
      if constexpr (kCancellationActiveForToken) {
        runInline(first, last, std::forward<F>(fn), desc.token);
      } else {
        runInline(first, last, std::forward<F>(fn), CancellationToken{});
      }
      return;
    }

    // Reuse decision: keyMatches is set above by piggybacking on the
    // write-elision guards. No extra compares: the guards above already do
    // field-by-field equality. Eligible only when the worker runner is
    // monomorphized (StaticUniform + lvalue F + nothrow body + no
    // cancellation), AND fillBlockShape didn't change chunk/blockCount this
    // call.
    constexpr bool kCancellationOff = !detail::kCancellationActive<HintsT>;
    constexpr bool kReuseEligible =
        (HintsT::balance == Balance::StaticUniform ||
         HintsT::balance == Balance::DynamicChunked) &&
        std::is_lvalue_reference_v<F &&> &&
        std::is_nothrow_invocable_v<std::remove_reference_t<F> &, std::size_t,
                                    std::size_t> &&
        kCancellationOff;

    if constexpr (HintsT::balance == Balance::StaticUniform) {
      const bool reuseHint = kReuseEligible && keyMatches;
      dispatchOneStaticTypedSlot0Hinted<HintsT>(desc, fn, reuseHint);
    } else if constexpr (HintsT::balance == Balance::DynamicChunked) {
      const bool reuseHint = kReuseEligible && keyMatches;
      dispatchOneDynamicTypedSlot0Hinted<HintsT>(desc, fn, reuseHint);
    } else {
      dispatchOneStatic<HintsT::balance>(desc);
    }
  }
  // NOLINTEND(performance-unnecessary-value-param,
  // clang-analyzer-core.StackAddressEscape)

  /// Runtime-hint sibling of `parallelFor<HintsT>` for benchmark / CLI
  /// consumers. Mirrors the member-template surface but accepts a runtime
  /// |hints| value; the dispatch decision is made from a runtime field rather
  /// than a compile-time constant.
  template <class F>
  void parallelForRuntime(std::size_t first, std::size_t last, F &&fn,
                          const Hints &hints,
                          CancellationToken tok = CancellationToken{}) {
    if (last <= first) {
      return;
    }
    // Honor `hints.cancellationChecks`: when false, clear the caller's
    // token in place so no downstream worker polls it. Single predicted
    // branch; assigning a default token is allocation-free and equivalent
    // to the compile-time `kCancellationActive<HintsT>` elision used by
    // `parallelFor<HintsT>`.
    if (!hints.cancellationChecks) {
      tok = CancellationToken{};
    }
    if (shouldFallThroughCrossArena()) {
      runInlineChunked(first, last, hints.chunk, hints.cancellationChecks,
                       std::forward<F>(fn), tok);
      return;
    }
    const std::size_t n = last - first;
    const std::size_t participants = m_control.participants;

    if (detail::shouldRunInline(n, participants, hints.estimatedItemNs,
                                hints.minTaskUs)) {
      runInlineChunked(first, last, hints.chunk, hints.cancellationChecks,
                       std::forward<F>(fn), tok);
      return;
    }

    auto wrapper = [&fn](std::size_t lo, std::size_t hi) { fn(lo, hi); };
    const FunctionRef<void(std::size_t, std::size_t)> body{wrapper};

    detail::JobDescriptor desc;
    desc.first = first;
    desc.last = last;
    desc.participants = static_cast<std::uint32_t>(participants);
    desc.balance = hints.balance;
    desc.priority = hints.priority;
    desc.preWakeCompletionProbe = true;
    desc.body = body;
    desc.token = std::move(tok);

    fillBlockShape(desc, n, participants, hints.chunk);

    if (desc.blockCount <= 1U) {
      runInlineChunked(first, last, hints.chunk, hints.cancellationChecks,
                       std::forward<F>(fn), desc.token);
      return;
    }

    // Skip the atomic store when `blockCount <= participants`: Phase B's
    // atomic tail is never read in that shape (BlockClaim<Dynamic>::next
    // returns kNoBlock immediately).
    if (hints.balance == Balance::DynamicChunked &&
        desc.blockCount > participants) {
      desc.nextBlock.store(participants, std::memory_order_relaxed);
    }

    dispatchOne(desc);
  }

  /// Deterministic reduction over `[first, last)` using the policy from
  /// |HintsT|.
  ///
  /// Each block produces a partial value via |map|, partials are stored in a
  /// stack-resident vector indexed by stable chunk id, and the producer
  /// combines them with |combine| via a pairwise reduction tree in chunk-id
  /// order (NOT completion order). Under `Determinism::KahanCompensated` the
  /// per-chunk accumulation and tree combine wrap the user partial in a
  /// `KahanPair` so FP cancellation is compensated through every interior node.
  ///
  /// Determinism contract:
  /// - Chunk size is a function of `(n, HintsT)` only, NOT of participant
  ///   count. The chunk count is stable across `j`, so the tree shape is
  ///   `n`-determined.
  /// - Workers write only to their own `partials[chunkId]` slots; the
  ///   producer's join-loop establishes happens-before via the acquire-load on
  ///   `doneEpoch` so reading partials post-join is race-free.
  /// - Combining in chunk-id pairwise tree order makes parallel FP reduction
  ///   bit-reproducible (Demmel-Nguyen TOMS 2014).
  ///
  /// Empty range returns |init| unchanged. Cancellation throws
  /// `cancelled_value_exception<T>` whose `partial_value` is the deterministic
  /// combine of all chunks that completed before the stop; if zero completed,
  /// `partial_value` is |init| unchanged (we never combine against a
  /// default-constructed `T{}` since `combine` is not assumed to satisfy
  /// `combine(x, T{}) == x`).
  ///
  /// Requirements on `T`: default-constructible, copyable or movable, and
  /// trivially copyable in the Kahan path (per-chunk value is converted to
  /// `double` before the tree combine).
  template <class HintsT, class T, class Map, class Combine>
  [[nodiscard]] T parallelReduce(std::size_t first, std::size_t last, T init,
                                 Map &&map, Combine &&combine,
                                 CancellationToken tok = CancellationToken{}) {
    if (last <= first) {
      return init;
    }
    if constexpr (detail::kCancellationActive<HintsT>) {
      if (!tok.canStop()) {
        using NoCancellationHintsT = detail::NoCancellationHints<HintsT>;
        return parallelReduce<NoCancellationHintsT>(
            first, last, std::move(init), std::forward<Map>(map),
            std::forward<Combine>(combine), CancellationToken{});
      }
    }
    if (shouldFallThroughCrossArena()) {
      return runReduceInline<HintsT>(first, last, std::move(init),
                                     std::forward<Map>(map),
                                     std::forward<Combine>(combine), tok);
    }
    const std::size_t n = last - first;
    const std::size_t participants = m_control.participants;

    if (detail::shouldRunInlineHinted<HintsT>(n, participants)) {
      return runReduceInline<HintsT>(first, last, std::move(init),
                                     std::forward<Map>(map),
                                     std::forward<Combine>(combine), tok);
    }

    // Single-chunk fast path: reduce of a tiny range collapses to one map call
    // + combine, skipping the dispatch round-trip and the per-chunk slot vector
    // allocation entirely.
    {
      const std::size_t chunk = reduceChunkSize(n, HintsT::chunk);
      if (ceilDiv(n, chunk) <= 1U) {
        return runReduceInline<HintsT>(first, last, std::move(init),
                                       std::forward<Map>(map),
                                       std::forward<Combine>(combine), tok);
      }
    }

    if constexpr (HintsT::chunk == 0) {
      ensureProducerPinnedForChunkZero();
    }

    return runReduceParallel<HintsT>(
        first, last, std::move(init), std::forward<Map>(map),
        std::forward<Combine>(combine), std::move(tok), participants);
  }

  /// Runtime-hint sibling of `parallelReduce<HintsT>` for benchmark / CLI
  /// consumers. The runtime branch on `hints.determinism` selects between the
  /// compensated and uncompensated reduction shapes via one tag-switch.
  template <class T, class Map, class Combine>
  [[nodiscard]] T
  parallelReduceRuntime(std::size_t first, std::size_t last, T init, Map &&map,
                        Combine &&combine, const Hints &hints,
                        CancellationToken tok = CancellationToken{}) {
    if (last <= first) {
      return init;
    }
    if (shouldFallThroughCrossArena()) {
      return runReduceInlineRuntime(first, last, std::move(init),
                                    std::forward<Map>(map),
                                    std::forward<Combine>(combine), hints, tok);
    }
    const std::size_t n = last - first;
    const std::size_t participants = m_control.participants;

    if (detail::shouldRunInline(n, participants, hints.estimatedItemNs,
                                hints.minTaskUs)) {
      return runReduceInlineRuntime(first, last, std::move(init),
                                    std::forward<Map>(map),
                                    std::forward<Combine>(combine), hints, tok);
    }
    {
      const std::size_t chunk = reduceChunkSize(n, hints.chunk);
      if (ceilDiv(n, chunk) <= 1U) {
        return runReduceInlineRuntime(
            first, last, std::move(init), std::forward<Map>(map),
            std::forward<Combine>(combine), hints, tok);
      }
    }

    return runReduceParallelRuntime(
        first, last, std::move(init), std::forward<Map>(map),
        std::forward<Combine>(combine), hints, std::move(tok), participants);
  }

  /// Run |phaseFn| for |nPhases| phases using the persistent-worker plex
  /// protocol.
  ///
  /// The producer participates as slot 0 and drives the phase epoch;
  /// background workers spin-wait in user space between phases (no futex
  /// round-trip per phase). Each phase invokes
  /// `phaseFn(phaseIdx, slot, lo, hi)` once per participant, where
  /// `(lo, hi)` is the slot's contiguous range over `[0, n)`.
  ///
  /// Optional pre-phase hook: when supplied, `prePhaseFn(phaseIdx)` runs
  /// serially on the producer BEFORE publishing the next phase, with
  /// happens-before to every worker's per-phase body. Use it to read the
  /// previous phase's per-slot results (synchronized by the prior join) and
  /// update shared state the upcoming phase reads.
  ///
  /// Determinism: phase epochs are produced in `[0, nPhases)` order and each
  /// `phaseFn(p, slot, ...)` call is invoked exactly once per `(p, slot)` pair.
  /// The producer's slot-0 work for phase `p` runs after publishing
  /// `currentPhase = p + 1` and before joining on every other slot's
  /// `done >= p + 1`.
  ///
  /// Cancellation: a stopped |tok| flips the plex's cancellation flag at the
  /// next phase boundary. Worker bodies observe the flag between phases and
  /// return cleanly; the call still rendezvous with every worker before
  /// returning.
  ///
  /// Exception handling: the first thrown `phaseFn` exception is captured and
  /// rethrown by the producer after join. Subsequent throws drop.
  template <class HintsT, class Phase, class PrePhase>
  void runPlex(std::size_t nPhases, std::size_t n, Phase &&phaseFn,
               PrePhase &&prePhaseFn,
               CancellationToken tok = CancellationToken{}) {
    if (nPhases == 0) {
      return;
    }
    const std::size_t participants = m_control.participants;

    if (participants <= 1 || shouldFallThroughCrossArena()) {
      runPlexInline(nPhases, n, std::forward<Phase>(phaseFn),
                    std::forward<PrePhase>(prePhaseFn), tok);
      return;
    }

    runPlexParallel(nPhases, n, std::forward<Phase>(phaseFn),
                    std::forward<PrePhase>(prePhaseFn), std::move(tok),
                    participants);
  }

  /// Plex form without a pre-phase hook. Equivalent to the four-argument
  /// overload with a no-op pre-phase callable. Use when the plex needs no
  /// inter-phase serial bookkeeping.
  template <class HintsT, class Phase>
  void runPlex(std::size_t nPhases, std::size_t n, Phase &&phaseFn,
               CancellationToken tok = CancellationToken{}) {
    auto noPrePhase = [](std::size_t /*phaseIdx*/) noexcept {};
    runPlex<HintsT>(nPhases, n, std::forward<Phase>(phaseFn), noPrePhase,
                    std::move(tok));
  }

  /// Runtime-hint sibling of `runPlex<HintsT>` for benchmark / CLI consumers.
  template <class Phase, class PrePhase>
  void runPlexRuntime(std::size_t nPhases, std::size_t n, Phase &&phaseFn,
                      PrePhase &&prePhaseFn, const Hints & /*hints*/,
                      CancellationToken tok = CancellationToken{}) {
    if (nPhases == 0) {
      return;
    }
    const std::size_t participants = m_control.participants;

    if (participants <= 1 || shouldFallThroughCrossArena()) {
      runPlexInline(nPhases, n, std::forward<Phase>(phaseFn),
                    std::forward<PrePhase>(prePhaseFn), tok);
      return;
    }

    runPlexParallel(nPhases, n, std::forward<Phase>(phaseFn),
                    std::forward<PrePhase>(prePhaseFn), std::move(tok),
                    participants);
  }

  /// Runtime-hint runPlex without a pre-phase hook.
  template <class Phase>
  void runPlexRuntime(std::size_t nPhases, std::size_t n, Phase &&phaseFn,
                      const Hints &hints,
                      CancellationToken tok = CancellationToken{}) {
    auto noPrePhase = [](std::size_t /*phaseIdx*/) noexcept {};
    runPlexRuntime(nPhases, n, std::forward<Phase>(phaseFn), noPrePhase, hints,
                   std::move(tok));
  }

  /// Run |fns|... in parallel as a recursive fork-join, joining once every
  /// task retires.
  ///
  /// Each task callable is wrapped into a stack-resident `detail::Task`
  /// descriptor and pushed onto a participating worker's Chase-Lev
  /// work-stealing deque. Workers pop from their own deque first; on empty
  /// they steal from another worker's deque, biased toward same-CCD victims
  /// when `HintsT::stealPolicy == StealPolicy::ClusterLocal`. The producer
  /// participates as slot 0 and joins on the outstanding-task counter reaching
  /// zero.
  ///
  /// Recursive children: tasks may call back into `forkJoin` from inside their
  /// bodies. The nested call detects it is running on one of this pool's
  /// workers, allocates its own outstanding-task counter, and pushes children
  /// onto the calling worker's own deque. The nested join condition is its
  /// own counter reaching zero.
  ///
  /// Cancellation: workers observe |tok| at task boundaries. A stopped token
  /// causes participating workers to short-circuit unstarted bodies
  /// (decrementing the counter without running the body) so the join still
  /// rendezvous.
  ///
  /// Exception handling: the first thrown task body's exception is captured
  /// and rethrown from the producer after every outstanding task has retired.
  /// Subsequent throws drop. The cancellation flag is set as part of the
  /// first-exception capture path so the join finishes promptly.
  template <class HintsT, class... TaskFns>
  // The `tok` parameter is moved into `runForkJoinOuter` / `runForkJoinNested`
  // on the non-empty task path; the no-task branch returns immediately. Tidy
  // sees only the no-task branch and suggests `const &`, but pass-by-value
  // makes the move possible on the live path.
  // NOLINTNEXTLINE(performance-unnecessary-value-param)
  void forkJoin(CancellationToken tok, TaskFns &&...fns) {
    forkJoinImpl<HintsT, /*HasToken=*/true>(std::move(tok),
                                            std::forward<TaskFns>(fns)...);
  }

  /// Internal implementation shared by `forkJoin` and the no-token
  /// overload. `HasToken` is a compile-time tag that elides token reads
  /// when the caller did not pass a token.
  template <class HintsT, bool HasToken, class... TaskFns>
  void forkJoinImpl(CancellationToken tok, TaskFns &&...fns) {
    constexpr std::size_t kNTasks = sizeof...(TaskFns);
    if constexpr (kNTasks == 0) {
      (void)tok;
      return;
    } else if constexpr (kNTasks == 1) {
      // Single-task fork-join is a degenerate case: there is no fan-out, no
      // deque traffic, no generation publish. Run it directly on the caller and
      // observe the token at entry, but only when the caller passed a real
      // token; the no-token overload bypasses the check entirely via
      // compile-time gate.
      if constexpr (HasToken) {
        if (tok.stop_requested()) {
          return;
        }
      } else {
        (void)tok;
      }
      std::tuple<std::decay_t<TaskFns>...> single{
          std::forward<TaskFns>(fns)...};
      std::get<0>(single)();
      return;
    } else {
      // Materialize each task callable in a tuple so the producer's stack owns
      // the storage that every `FunctionRef` points into. The tuple outlives
      // the descriptors by construction -- the synchronous call joins before
      // this scope exits.
      std::tuple<std::decay_t<TaskFns>...> closures{
          std::forward<TaskFns>(fns)...};

      // Detect a recursive call from inside this pool's worker drain loop and
      // take the in-place nested-recursion path. Outside the pool (or from a
      // different pool's worker), route through the dispatch path that wakes
      // every background worker.
      auto &ctx = tlsContext();
      if (ctx.pool == this && ctx.insidePoolWorker) {
        // Typed-tail fast path: build N-1 Task descriptors for the deferred
        // children and invoke the last child directly via its statically-typed
        // callable. Skips the FunctionRef thunk and the per-task runOneTask
        // wrapper that would otherwise mediate the inline call. The deque still
        // holds N-1 descriptors so peers steal them through the standard victim
        // probe.
        std::array<detail::Task, kNTasks - 1> deferred{};
        fillTaskBodies(closures, deferred,
                       std::make_index_sequence<kNTasks - 1>{});
        runForkJoinTypedTailNested<HasToken>(
            deferred.data(), kNTasks - 1, std::get<kNTasks - 1>(closures),
            std::move(tok), stealPolicyFromHints<HintsT>(),
            static_cast<std::uint32_t>(ctx.slot));
        return;
      }
      std::array<detail::Task, kNTasks> tasks{};
      fillTaskBodies(closures, tasks, std::index_sequence_for<TaskFns...>{});
      ensureProducerPinnedForChunkZero();
      runForkJoinOuter<HasToken>(tasks.data(), kNTasks, std::move(tok),
                                 stealPolicyFromHints<HintsT>());
    }
  }

  /// No-token convenience overload for `forkJoin`. Forwards to the four-arg
  /// overload with a default-constructed `CancellationToken` so call sites
  /// that do not need cancellation pay no syntactic cost.
  template <class HintsT, class... TaskFns>
  void forkJoin(TaskFns &&...fns) {
    // No token passed: route through the compile-time `HasToken=false` impl so
    // the cancel-gate's `state.token.stop_requested()` poll, the per-call
    // `state.token = ...` assignment, and the `kNTasks == 1` early-stop check
    // are all elided at compile time. Saves one `shared_ptr` deref per spawn on
    // the canonical (no-cancel) recursive path.
    forkJoinImpl<HintsT, /*HasToken=*/false>(CancellationToken{},
                                             std::forward<TaskFns>(fns)...);
  }

  /// Runtime-N fork-join: spawn |n| tasks indexed `[0, n)` and join.
  /// Generalizes the variadic `forkJoin<HintsT>(fns...)` to a count known only
  /// at runtime.
  template <class HintsT, class BodyFn>
  void forkJoinAll(std::size_t n, BodyFn body) {
    if (n == 0U) {
      return;
    }
    if (n == 1U) {
      // Single-task fork-join is a degenerate case: no fan-out, no deque
      // traffic, no state.
      body(0);
      return;
    }
    constexpr std::size_t kStackTaskBudget = 32U;
    using Body = std::decay_t<BodyFn>;
    Body &bodyRef = body;
    struct IndexedClosure {
      Body *bodyPtr;
      std::size_t index;
      void operator()() const { (*bodyPtr)(index); }
    };
    // Uninitialized aligned storage. Default-init `std::array<...>{}` would
    // zero 32 elements every call; we only ever touch slots [0, n). Skipping
    // the 32-element fill removes the memset_avx512 hot spot from the runtime-N
    // hot path.
    alignas(IndexedClosure)
        std::array<std::byte, sizeof(IndexedClosure) * kStackTaskBudget>
            stackClosureBuf;
    alignas(detail::Task)
        std::array<std::byte, sizeof(detail::Task) * kStackTaskBudget>
            stackTaskBuf;
    std::vector<IndexedClosure> heapClosures;
    std::vector<detail::Task> heapTasks;
    auto *closures = reinterpret_cast<IndexedClosure *>(stackClosureBuf.data());
    auto *tasks = reinterpret_cast<detail::Task *>(stackTaskBuf.data());
    if (n > kStackTaskBudget) {
      heapClosures.resize(n);
      heapTasks.resize(n);
      closures = heapClosures.data();
      tasks = heapTasks.data();
    }

    auto &ctx = tlsContext();
    if (ctx.pool == this && ctx.insidePoolWorker) {
      // Typed-tail nested fast path: push n-1 tasks for peers to steal, invoke
      // `body(n-1)` directly on this thread via its statically-typed callable.
      // Skips the FunctionRef thunk and one runOneTask wrapper at every
      // recursion frame. Mirrors the variadic forkJoin's typed-tail path for
      // the runtime-N spawn shape.
      const std::size_t deferred = n - 1U;
      for (std::size_t i = 0; i < deferred; ++i) {
        if (n > kStackTaskBudget) {
          closures[i] = IndexedClosure{.bodyPtr = &bodyRef, .index = i};
          tasks[i].body = FunctionRef<void()>(closures[i]);
          tasks[i].state = nullptr;
        } else {
          ::new (closures + i) IndexedClosure{.bodyPtr = &bodyRef, .index = i};
          ::new (tasks + i) detail::Task{};
          tasks[i].body = FunctionRef<void()>(closures[i]);
        }
      }
      runForkJoinTypedTailNested</*HasToken=*/false>(
          tasks, deferred, [&]() { bodyRef(n - 1U); }, CancellationToken{},
          stealPolicyFromHints<HintsT>(), static_cast<std::uint32_t>(ctx.slot));
      if (n <= kStackTaskBudget) {
        // Trivially-destructible types: no destructor calls needed.
        static_assert(std::is_trivially_destructible_v<IndexedClosure>);
        static_assert(std::is_trivially_destructible_v<detail::Task>);
      }
      return;
    }

    // Outer (top-level) path: full materialization, no inline tail.
    for (std::size_t i = 0; i < n; ++i) {
      if (n > kStackTaskBudget) {
        closures[i] = IndexedClosure{.bodyPtr = &bodyRef, .index = i};
        tasks[i].body = FunctionRef<void()>(closures[i]);
        tasks[i].state = nullptr;
      } else {
        ::new (closures + i) IndexedClosure{.bodyPtr = &bodyRef, .index = i};
        ::new (tasks + i) detail::Task{};
        tasks[i].body = FunctionRef<void()>(closures[i]);
      }
    }
    runForkJoinOuter</*HasToken=*/false>(tasks, n, CancellationToken{},
                                         stealPolicyFromHints<HintsT>());
  }

  // Friend overload routing the `citor::forkJoin` CPO to this pool. Both
  // surfaces (member call and CPO call) share the same engine and monomorphize
  // identically.
  template <class HintsT, class... TaskFns>
  friend void tag_invoke(ForkJoinTag /*cpo*/, ThreadPool &self,
                         CancellationToken tok, HintsT /*hints*/,
                         TaskFns &&...fns) {
    self.template forkJoin<HintsT>(std::move(tok),
                                   std::forward<TaskFns>(fns)...);
  }

  // Adapter so the CPO's `ForkJoinFn::operator()` can call our friend
  // overload. `ForkJoinFn::operator()` invokes `tag_invoke(*this, pool, tok,
  // HintsT{}, fns...)` passing the CPO's own type, not `ForkJoinTag`. The
  // adapter forwards from the CPO type to the canonical `ForkJoinTag`
  // overload above.
  template <class HintsT, class... TaskFns>
  friend void tag_invoke(const detail::ForkJoinFn & /*cpo*/, ThreadPool &self,
                         CancellationToken tok, HintsT /*hints*/,
                         TaskFns &&...fns) {
    self.template forkJoin<HintsT>(std::move(tok),
                                   std::forward<TaskFns>(fns)...);
  }

  /// Run |fn| over `[0, q)` query indices in parallel using the policy from
  /// |HintsT|.
  ///
  /// The body is invoked once per chunk as `fn(qFirst, qLast)`; the body must
  /// process every query index in `[qFirst, qLast)` in any order, writing into
  /// a per-query output slot keyed on the query index. Synchronous.
  ///
  /// Reuses the `parallelFor` engine; this is the named entry point for "many
  /// independent queries" workloads. Sites with skewed per-query cost should
  /// override `HintsT::balance` to `Balance::DynamicChunked` to amortize
  /// traversal-depth skew across workers.
  ///
  /// Output stability: per-query results are bit-identical regardless of
  /// dispatch order so long as the body keys its writes on the query index.
  template <class HintsT, class QueryFn>
  void bulkForQueries(std::size_t q, QueryFn &&fn,
                      CancellationToken tok = CancellationToken{}) {
    parallelFor<HintsT>(std::size_t{0}, q, std::forward<QueryFn>(fn),
                        std::move(tok));
  }

  /// Runtime-hint sibling of `bulkForQueries<HintsT>` for benchmark / CLI
  /// consumers. Forwards through `parallelForRuntime`.
  template <class QueryFn>
  void bulkForQueriesRuntime(std::size_t q, QueryFn &&fn, const Hints &hints,
                             CancellationToken tok = CancellationToken{}) {
    parallelForRuntime(std::size_t{0}, q, std::forward<QueryFn>(fn), hints,
                       std::move(tok));
  }

  // Friend overload routing the `citor::bulkForQueries` CPO to this pool. The
  // CPO surface monomorphizes identically to the member call.
  template <class HintsT, class QueryFn>
  friend void tag_invoke(BulkForQueriesTag /*cpo*/, ThreadPool &self,
                         std::size_t q, QueryFn &&fn, HintsT /*hints*/,
                         CancellationToken tok) {
    self.template bulkForQueries<HintsT>(q, std::forward<QueryFn>(fn),
                                         std::move(tok));
  }

  // Adapter so the CPO's `BulkForQueriesFn::operator()` can call our friend
  // overload. Forwards from the CPO type to the canonical `BulkForQueriesTag`
  // overload above.
  template <class HintsT, class QueryFn>
  friend void tag_invoke(const detail::BulkForQueriesFn & /*cpo*/,
                         ThreadPool &self, std::size_t q, QueryFn &&fn,
                         HintsT /*hints*/, CancellationToken tok) {
    self.template bulkForQueries<HintsT>(q, std::forward<QueryFn>(fn),
                                         std::move(tok));
  }

  // Friend overload routing the `citor::parallelFor` CPO to this pool. The CPO
  // surface monomorphizes identically to the member call.
  template <class HintsT, class F>
  friend void tag_invoke(ParallelForTag /*cpo*/, ThreadPool &self,
                         std::size_t first, std::size_t last, F &&fn,
                         HintsT /*hints*/, CancellationToken tok) {
    self.template parallelFor<HintsT>(first, last, std::forward<F>(fn),
                                      std::move(tok));
  }

  // Friend overload routing the `citor::parallelReduce` CPO to this pool.
  // Both surfaces monomorphize identically with `HintsT` baked in at compile
  // time.
  template <class HintsT, class T, class Map, class Combine>
  friend T tag_invoke(ParallelReduceTag /*cpo*/, ThreadPool &self,
                      std::size_t first, std::size_t last, T init, Map &&map,
                      Combine &&combine, HintsT /*hints*/,
                      CancellationToken tok) {
    return self.template parallelReduce<HintsT>(
        first, last, std::move(init), std::forward<Map>(map),
        std::forward<Combine>(combine), std::move(tok));
  }

  // Adapter so the `parallelReduce` CPO function-object can call our friend
  // overload. Forwards from the CPO type to the canonical `ParallelReduceTag`
  // overload above.
  template <class HintsT, class T, class Map, class Combine>
  friend T tag_invoke(const detail::ParallelReduceFn & /*cpo*/,
                      ThreadPool &self, std::size_t first, std::size_t last,
                      T init, Map &&map, Combine &&combine, HintsT /*hints*/,
                      CancellationToken tok) {
    return self.template parallelReduce<HintsT>(
        first, last, std::move(init), std::forward<Map>(map),
        std::forward<Combine>(combine), std::move(tok));
  }

  // Adapter so the CPO's `ParallelForFn::operator()` can call our friend
  // overload. Forwards from the CPO type to the canonical `ParallelForTag`
  // overload above.
  template <class HintsT, class F>
  friend void tag_invoke(const detail::ParallelForFn & /*cpo*/,
                         ThreadPool &self, std::size_t first, std::size_t last,
                         F &&fn, HintsT /*hints*/, CancellationToken tok) {
    self.template parallelFor<HintsT>(first, last, std::forward<F>(fn),
                                      std::move(tok));
  }

  // Friend overload routing the `citor::runPlex` CPO to this pool. The CPO
  // surface monomorphizes identically to the member call.
  template <class HintsT, class Phase>
  friend void tag_invoke(RunPlexTag /*cpo*/, ThreadPool &self,
                         std::size_t nPhases, std::size_t n, Phase &&phaseFn,
                         HintsT /*hints*/, CancellationToken tok) {
    self.template runPlex<HintsT>(nPhases, n, std::forward<Phase>(phaseFn),
                                  std::move(tok));
  }

  // Adapter so the CPO's `RunPlexFn::operator()` can call our friend overload.
  // Forwards from the CPO type to the canonical `RunPlexTag` overload above.
  template <class HintsT, class Phase>
  friend void tag_invoke(const detail::RunPlexFn & /*cpo*/, ThreadPool &self,
                         std::size_t nPhases, std::size_t n, Phase &&phaseFn,
                         HintsT /*hints*/, CancellationToken tok) {
    self.template runPlex<HintsT>(nPhases, n, std::forward<Phase>(phaseFn),
                                  std::move(tok));
  }

  /// Run an ordered sequence of stages with the declared barrier between
  /// consecutive stages.
  ///
  /// Each stage is a `Stage` value carrying a callable plus a compile-time
  /// `BarrierKind`. The producer participates as slot 0 across every stage;
  /// background workers run their slice of each stage in parallel. The
  /// post-stage barrier controls how the next stage begins:
  ///
  /// - `None`: each worker proceeds without waiting on others.
  /// - `Global` / `DeterministicReduce`: every worker waits until every slot
  ///   has finished the prior stage before any worker begins the next.
  /// - `ProducerSerial`: only slot 0 runs the next stage's body; other workers
  ///   spin on slot 0's completion before proceeding.
  ///
  /// Determinism: stages run in submission order. In the default same-chunk
  /// mode, each `(stage, slot)` pair is invoked exactly once per call (or
  /// zero times for non-producer slots after a `ProducerSerial` barrier).
  /// Per-slot ranges are stable functions of `(n, slot, participants)`.
  /// Dynamic-chain hints can opt all-global stage packs into per-stage chunk
  /// claiming.
  ///
  /// Cancellation: the implicit cancellation token bound through the CPO
  /// surface is observed at stage boundaries. A stopped token causes every
  /// slot to skip the remaining stages cleanly so the producer's join still
  /// rendezvous.
  ///
  /// Exception handling: the first thrown stage body's exception is captured,
  /// the chain's cancellation flag is set, and the producer rethrows once
  /// every slot has retired. Slots that observe the cancellation flag stamp
  /// their `done` epoch to the final stage so other slots' barrier waits
  /// complete.
  ///
  /// Empty stage pack is a no-op; empty range (`n == 0`) still runs each
  /// stage with `slotLo == slotHi`.
  template <class ChainHintsT, class... Stages>
  void parallelChain(std::size_t n, Stages &&...stages) {
    parallelChainWithToken<ChainHintsT>(n, CancellationToken{},
                                        std::forward<Stages>(stages)...);
  }

  /// `parallelChain` overload that accepts an explicit cancellation token.
  /// Named differently from the no-token form because variadic packs absorb
  /// the leading token argument when the two overloads share a name.
  template <class ChainHintsT, class... Stages>
  void parallelChainWithToken(std::size_t n, const CancellationToken &tok,
                              Stages &&...stages) {
    parallelChainImpl<ChainHintsT>(n, tok, std::forward<Stages>(stages)...);
  }

  /// Runtime-hint sibling of `parallelChain<ChainHintsT>` for benchmark / CLI
  /// consumers. The runtime branch on stage barriers is compile-time because
  /// each stage's `BarrierKind` is encoded in its type. |hints.priority| is
  /// honored on the dispatch lease; other hint fields (balance, chunk,
  /// pipelineSameChunk, cancellationChecks) currently fall back to the
  /// engine defaults; the per-stage runner uses `ChainHintsT` as a type
  /// marker for `if constexpr` selection, so threading them runtime would
  /// require structural changes to the per-stage dispatcher.
  template <class... Stages>
  void parallelChainRuntime(std::size_t n, const ChainHints &hints,
                            const CancellationToken &tok, Stages &&...stages) {
    parallelChainImplWithRuntimeHints<ChainHints>(
        n, &hints, tok, std::forward<Stages>(stages)...);
  }

  // Friend overload routing the `citor::parallelChain` CPO to this pool. The
  // CPO surface monomorphizes identically to the member call.
  template <class ChainHintsT, class... Stages>
  friend void tag_invoke(ParallelChainTag /*cpo*/, ThreadPool &self,
                         std::size_t n, [[maybe_unused]] ChainHintsT hints,
                         const CancellationToken &tok, Stages &&...stages) {
    self.template parallelChainWithToken<ChainHintsT>(
        n, tok, std::forward<Stages>(stages)...);
  }

  // Adapter so the CPO's `ParallelChainFn::operator()` can call our friend
  // overload. Forwards from the CPO type to the canonical `ParallelChainTag`
  // overload above.
  template <class ChainHintsT, class... Stages>
  friend void tag_invoke(const detail::ParallelChainFn & /*cpo*/,
                         ThreadPool &self, std::size_t n,
                         [[maybe_unused]] ChainHintsT hints,
                         const CancellationToken &tok, Stages &&...stages) {
    self.template parallelChainWithToken<ChainHintsT>(
        n, tok, std::forward<Stages>(stages)...);
  }

  /// Blelloch two-pass parallel prefix scan over `[0, n)` using the policy
  /// from |HintsT|.
  ///
  /// Two-pass dispatch shape:
  ///   1. Pass 1: each slot computes its chunk's partial sum by invoking
  ///      `body(slot, slotLo, slotHi, identity, nullptr)`. The body returns
  ///      the chunk's partial; the worker stamps `partials[slot]` and stamps
  ///      its done epoch to `1` (release).
  ///   2. Sequential reduce: the producer waits until every chunk's
  ///      `done >= 1`, then computes
  ///      `prefix[c] = combine(prefix[c-1], partials[c-1])` in chunk-id order
  ///      with `prefix[0] = identity`. It overwrites `partials[c]` with
  ///      `prefix[c]` and publishes `prefixesPublished = 1` (release).
  ///   3. Pass 2: each slot acquire-loads `prefixesPublished`, then invokes
  ///      `body(slot, slotLo, slotHi, partials[slot], nullptr)`; the body
  ///      writes the per-element scan into a buffer it captured itself and
  ///      returns the same partial sum (ignored). Each slot stamps `done = 2`.
  ///   4. The producer joins on every slot's `done >= 2` and returns the
  ///      inclusive accumulator at the right edge:
  ///      `combine(prefix[participants - 1], partial[participants - 1])`.
  ///
  /// Body contract for distinguishing passes: `out` is `nullptr` in BOTH
  /// passes; the body must distinguish them itself. Recommended pattern:
  ///
  ///   std::atomic<int> callIdx{0};
  ///   auto body = [&](...) {
  ///     const int idx = callIdx.fetch_add(1, std::memory_order_acq_rel);
  ///     if (idx < pool.participants()) { /* Pass 1 */ }
  ///     else                            { /* Pass 2 */ }
  ///   };
  ///
  /// The `std::atomic<int>` captured by reference is the only race-free way
  /// to thread a pass index through the body, since multiple workers run
  /// each pass concurrently. A plain `int` counter captured by reference is
  /// a data race; use the atomic shape above. See `parallel_scan_test.cpp`
  /// for a working example.
  ///
  /// Alternative, brittle: branch on `initial != identity`. Slot 0's Pass 2
  /// receives `initial = identity` (its exclusive prefix IS identity), so
  /// this form loses the distinction for slot 0; prefer the atomic counter.
  ///
  /// Determinism: chunk identity is a stable function of
  /// `(n, slot, participants)`; the producer's sequential reduce visits
  /// chunks in `[0, participants)` order so output is bit-stable across worker
  /// counts only when |prefix| is associative AND the body's per-element fold
  /// is left-to-right within a chunk.
  ///
  /// Empty range returns |identity| without dispatching. Single participant
  /// walks inline: the body is invoked once with `initial = identity` and the
  /// call returns the body's return value combined with |identity| through
  /// |prefix|.
  ///
  /// Cancellation: stop-requests at pass boundaries cause every slot to stamp
  /// `done = 2` and exit cleanly; the producer's join still rendezvous. The
  /// return value reflects whatever completed chunks computed.
  ///
  /// Exception handling: the first thrown body exception is captured and
  /// rethrown by the producer after every slot has retired. Subsequent throws
  /// drop.
  template <class HintsT, class T, class BodyFn, class PrefixFn>
  [[nodiscard]] T parallelScan(std::size_t n, T identity, BodyFn &&body,
                               PrefixFn &&prefix,
                               CancellationToken tok = CancellationToken{}) {
    if (n == 0) {
      return identity;
    }
    const std::size_t participants = m_control.participants;

    // Inline threshold: a two-pass scan needs two body invocations per slot, a
    // sequential reduce, and two joins. For tiny `n`, the dispatch round-trip
    // dominates the actual scan work, so collapse to the inline branch.
    // Threshold matches the chunk default used by `parallelFor`'s inline gate
    // and keeps the contract observable (single body call with `initial =
    // identity`).
    static constexpr std::size_t kScanInlineThreshold = 32U;
    if (participants <= 1 || n <= kScanInlineThreshold ||
        shouldFallThroughCrossArena()) {
      // Inline fallback: one chunk, body invoked once with `initial =
      // identity`. The body's return value is the chunk's partial sum; the
      // inclusive accumulator at the right edge is `combine(identity, partial)`
      // which equals `combine(prefix[0], partial[0])` in the multi-participant
      // shape. The cross-arena guard takes the same branch: a worker on a
      // different arena must not block on this arena's queue. A pre-cancelled
      // token short- circuits before the body runs, returning the identity
      // unchanged.
      if (tok.stop_requested()) {
        return identity;
      }
      T partial =
          std::forward<BodyFn>(body)(std::size_t{0}, std::size_t{0}, n,
                                     identity, static_cast<T *>(nullptr));
      return prefix(std::move(identity), std::move(partial));
    }

    if constexpr (HintsT::chunk == 0) {
      ensureProducerPinnedForChunkZero();
    }

    return runScanParallel<HintsT>(
        n, std::move(identity), std::forward<BodyFn>(body),
        std::forward<PrefixFn>(prefix), std::move(tok), participants);
  }

  // Friend overload routing the `citor::parallelScan` CPO to this pool. The
  // CPO surface monomorphizes identically to the member call.
  template <class HintsT, class T, class BodyFn, class PrefixFn>
  friend T tag_invoke(ParallelScanTag /*cpo*/, ThreadPool &self, std::size_t n,
                      T identity, BodyFn &&body, PrefixFn &&prefix,
                      HintsT /*hints*/, CancellationToken tok) {
    return self.template parallelScan<HintsT>(
        n, std::move(identity), std::forward<BodyFn>(body),
        std::forward<PrefixFn>(prefix), std::move(tok));
  }

  // Adapter so the CPO's `ParallelScanFn::operator()` can call our friend
  // overload. Forwards from the CPO type to the canonical `ParallelScanTag`
  // overload above.
  template <class HintsT, class T, class BodyFn, class PrefixFn>
  friend T tag_invoke(const detail::ParallelScanFn & /*cpo*/, ThreadPool &self,
                      std::size_t n, T identity, BodyFn &&body,
                      PrefixFn &&prefix, HintsT /*hints*/,
                      CancellationToken tok) {
    return self.template parallelScan<HintsT>(
        n, std::move(identity), std::forward<BodyFn>(body),
        std::forward<PrefixFn>(prefix), std::move(tok));
  }

  /// Buffer-to-buffer inclusive prefix scan. Engine owns the inner loop
  /// (no user body), so it can use the most aggressive memory-traffic
  /// shape -- decoupled-lookback single-pass with `PREFETCHW` ahead of
  /// the writes, per-cluster lookback chains on multi-CCD parts -- to
  /// hit the hardware bandwidth floor.
  ///
  /// `in` and `out` are caller-owned spans of equal length; aliasing
  /// (in.data() == out.data()) is safe because the engine reads `in[i]`
  /// before writing `out[i]` for every `i`. `identity` is the monoid
  /// identity (e.g. 0 for plus). `prefix` is the associative combiner
  /// (must be associative; need not be commutative).
  ///
  /// Returns the inclusive total at the right edge:
  /// `prefix(prefix(... prefix(identity, in[0]) ...), in[n-1])`.
  template <class HintsT, class T, class PrefixFn>
  [[nodiscard]] T inclusiveScan(std::span<const T> in, std::span<T> out,
                                T identity, PrefixFn &&prefix,
                                CancellationToken tok = CancellationToken{}) {
    return runInclusiveScanLookback<HintsT, T>(in, out, std::move(identity),
                                               std::forward<PrefixFn>(prefix),
                                               std::move(tok));
  }

  // Friend overload routing the `citor::inclusiveScan` CPO to this pool.
  template <class HintsT, class T, class PrefixFn>
  friend T tag_invoke(InclusiveScanTag /*cpo*/, ThreadPool &self,
                      std::span<const T> in, std::span<T> out, T identity,
                      PrefixFn &&prefix, HintsT /*hints*/,
                      CancellationToken tok) {
    return self.template inclusiveScan<HintsT>(in, out, std::move(identity),
                                               std::forward<PrefixFn>(prefix),
                                               std::move(tok));
  }

  template <class HintsT, class T, class PrefixFn>
  friend T tag_invoke(const detail::InclusiveScanFn & /*cpo*/, ThreadPool &self,
                      std::span<const T> in, std::span<T> out, T identity,
                      PrefixFn &&prefix, HintsT /*hints*/,
                      CancellationToken tok) {
    return self.template inclusiveScan<HintsT>(in, out, std::move(identity),
                                               std::forward<PrefixFn>(prefix),
                                               std::move(tok));
  }

  /// Submit |fn| for fire-and-forget execution; returns without joining.
  ///
  /// The pool tracks every in-flight detached task via a counter. The
  /// destructor blocks until the counter drops to zero, so a body that
  /// outruns its caller is guaranteed to finish before the pool is destroyed.
  /// The body runs on a dedicated `std::thread` spawned per call; the pool's
  /// persistent worker fleet is reserved for the synchronous primitives.
  ///
  /// Cancellation: a pre-cancelled |tok| short-circuits the body before any
  /// user code runs. The body observes the token cooperatively; the pool does
  /// not preempt.
  ///
  /// Exception handling: an exception escaping |fn| is captured into a
  /// per-pool slot via `std::current_exception` and surfaced by
  /// `lastDetachedException`. The slot latches on the first throw; subsequent
  /// throws drop. A detached throw never propagates into the destructor.
  template <class HintsT, class TaskFn>
  void submitDetached(TaskFn fn, CancellationToken tok = CancellationToken{}) {
    // Token already stopped at submit time: the trampoline would observe
    // `stop_requested()` before invoking the body and decrement the in-flight
    // counter without doing useful work. Skip the heap allocation, the
    // std::thread spawn, and the counter round-trip; the user's contract is
    // "body runs unless cancellation observed", which is satisfied trivially
    // here.
    if (tok.stop_requested()) [[unlikely]] {
      return;
    }
    // Heap-allocate the closure owner on the cold submit path under a
    // try/catch; the body and token are owned by the spawned thread and
    // released after the body returns. A per-pool slab would shave the
    // allocation but is unnecessary while detached submission stays cold.
    auto owner = std::make_unique<DetachedTask>();
    owner->body = std::function<void()>(std::move(fn));
    owner->token = std::move(tok);

    m_detachedInFlight.fetch_add(1, std::memory_order_acq_rel);
    try {
      // The thread takes ownership of the heap closure via the unique_ptr
      // release below. The `runDetached` trampoline observes cancellation, runs
      // the body, captures any thrown exception, and decrements the in-flight
      // counter on the way out.
      DetachedTask *raw = owner.release();
      try {
        std::thread t(&ThreadPool::runDetached, this, raw);
        // Detach on the std::thread side so the OS thread cleans up on its own;
        // the join-on-destroy contract is satisfied by the in-flight counter,
        // not by holding the std::thread.
        t.detach();
      } catch (...) {
        // Spawn failed after release: reclaim ownership so the closure is
        // destroyed and rethrow.
        owner.reset(raw);
        throw;
      }
    } catch (...) {
      // Failed to spawn: roll back the counter so the destructor does not wait
      // forever, and re-throw so the caller sees the system error.
      m_detachedInFlight.fetch_sub(1, std::memory_order_acq_rel);
      {
        const std::scoped_lock<std::mutex> lock(m_detachedMutex);
        m_detachedDone.notify_all();
      }
      throw;
    }
  }

  /// Snapshot of the most recently captured exception thrown by a detached
  /// task. The slot latches on the first detached throw; subsequent throws do
  /// not overwrite. Returns `nullptr` if no detached task has thrown.
  [[nodiscard]] std::exception_ptr lastDetachedException() const noexcept {
    const std::scoped_lock<std::mutex> lock(m_detachedMutex);
    return m_detachedException;
  }

  // Friend overload routing the `citor::submitDetached` CPO to this pool.
  // Both surfaces share the same engine and monomorphize identically.
  template <class HintsT, class TaskFn>
  friend void tag_invoke(SubmitDetachedTag /*cpo*/, ThreadPool &self,
                         HintsT /*hints*/, TaskFn &&fn, CancellationToken tok) {
    self.template submitDetached<HintsT>(std::forward<TaskFn>(fn),
                                         std::move(tok));
  }

  // Adapter so the CPO's `SubmitDetachedFn::operator()` can call our friend
  // overload. Forwards from the CPO type to the canonical `SubmitDetachedTag`
  // overload above.
  template <class HintsT, class TaskFn>
  friend void tag_invoke(const detail::SubmitDetachedFn & /*cpo*/,
                         ThreadPool &self, HintsT /*hints*/, TaskFn &&fn,
                         CancellationToken tok) {
    self.template submitDetached<HintsT>(std::forward<TaskFn>(fn),
                                         std::move(tok));
  }

private:
  /// Per-thread context tracking pool participation; consulted by
  /// `workerIndex` and friends.
  struct ThreadContext {
    /// Slot index of the calling thread (0 outside any pool).
    std::size_t slot = 0;

    /// Whether the calling thread is currently inside a pool worker body.
    bool insidePoolWorker = false;

    /// Pool the calling thread is currently a worker of, or `nullptr` outside
    /// any pool. Used by `forkJoin` to detect a recursive call from inside the
    /// pool's own worker drain loop and take the in-place nested-recursion
    /// path instead of dispatching a fresh generation.
    ThreadPool *pool = nullptr;

    /// Origin tag of the pool this thread is participating in. Mirrors
    /// `ThreadPool::kind()`; set only while `insidePoolWorker` is true.
    PoolKind kind = PoolKind::Standalone;

    /// Arena index of the owning `PoolGroup` arena; meaningful only when
    /// `kind == PoolKind::Arena`. The cross-arena guard reads this token to
    /// fall through to inline-on-caller execution when a worker tries to call
    /// into a different arena's queue.
    std::uint32_t arenaIndex = 0;
  };

  /// Access (and lazily zero-initialize) the calling thread's pool context.
  static ThreadContext &tlsContext() noexcept {
    thread_local ThreadContext ctx;
    return ctx;
  }

  /// Detect a synchronous dispatch that would deadlock and force the
  /// inline-on-caller path. Returns `true` in three scenarios:
  ///
  /// 1. Cross-arena call. The caller is a worker on a different `Arena` pool.
  ///    Submitting here would block the caller's own arena worker on a queue
  ///    it does not service.
  /// 2. Same-arena reentrancy. The caller is already a worker on this `Arena`
  ///    pool. A primitive without a nested same-pool path would block on its
  ///    own join condition.
  /// 3. Same-pool reentrancy on a `Standalone` pool. The producer or a worker
  ///    already inside a synchronous primitive of THIS pool would re-enter
  ///    `dispatchOneStatic` and try to re-acquire the non-recursive
  ///    `m_dispatchMutex`, deadlocking against the outer dispatch.
  ///
  /// Primitives with a same-pool nested path (`forkJoin`, compile-time
  /// `parallelFor`) detect that case at their entry point and bypass this
  /// guard.
  [[nodiscard]] bool
  shouldFallThroughCrossArena(const ThreadContext &ctx) const noexcept {
    if (!ctx.insidePoolWorker) {
      return false;
    }
    // Same-pool reentrancy: a primitive that reaches this shared guard has no
    // nested same-pool path, so dispatching would block on m_dispatchMutex
    // while the outer dispatch holds it. Inline on the caller instead.
    if (ctx.pool == this) {
      return true;
    }
    // Cross-pool: a worker on any other pool (Arena or Standalone) calling
    // synchronously here would block its own pool's worker on this pool's
    // queue. Inline on the caller. Without this fall-through, two
    // standalone pools whose workers synchronously call each other deadlock
    // (worker A waits for B, worker B waits for A).
    return true;
  }

  /// Convenience overload that reads the calling thread's `ThreadContext`
  /// from TLS.
  [[nodiscard]] bool shouldFallThroughCrossArena() const noexcept {
    return shouldFallThroughCrossArena(tlsContext());
  }

  /// Argument bundle handed to `workerEntry`; lives in `m_workerSpawnArgs` for
  /// the pool's lifetime.
  struct WorkerSpawnArg {
    /// Pool owning this worker; non-owning pointer.
    ThreadPool *pool = nullptr;
    /// Slot index assigned to this worker (`>= 1`; producer is slot 0).
    std::size_t workerIndex = 0;
  };

  /// Heap-allocated owner for a single fire-and-forget detached task. The pool
  /// spawns one `std::thread` per task that takes ownership of the closure
  /// and the cancellation token; the owner is destroyed after the body
  /// returns (or short-circuits on a pre-cancelled token).
  struct DetachedTask {
    /// Closure run by the spawned thread. Empty when the body has been
    /// moved away or never assigned.
    std::function<void()> body;
    /// Cancellation token consulted before the body runs and again at
    /// any cooperation points the body chooses to insert.
    CancellationToken token;
  };

  /// Trampoline executed on the spawned detached-task thread. Observes
  /// cancellation, runs the body, captures any thrown exception into the
  /// pool's slot, and decrements the in-flight counter under the destructor's
  /// wait condition.
  static void runDetached(ThreadPool *self, DetachedTask *raw) noexcept {
    std::unique_ptr<DetachedTask> owner(raw);
    if (!owner->token.stop_requested()) {
      try {
        if (owner->body) {
          owner->body();
        }
      } catch (...) {
        const std::scoped_lock<std::mutex> lock(self->m_detachedMutex);
        if (!self->m_detachedException) {
          self->m_detachedException = std::current_exception();
        }
      }
    }
    // Decrement the in-flight count BEFORE destroying the closure. If the
    // closure owns the last `shared_ptr<ThreadPool>`, destroying it triggers
    // `~ThreadPool()` synchronously on this detached thread. The destructor
    // calls `waitForDetachedDrain()`, which would self-deadlock if the count
    // were still 1.
    //
    // Hold `m_detachedMutex` across the fetch_sub + notify so a waiter cannot
    // miss the wake-up between its predicate check and its `wait()`. Once the
    // lock is released the closure may destroy the pool object on this same
    // thread; do NOT touch `self` after `owner.reset()`.
    {
      const std::scoped_lock<std::mutex> lock(self->m_detachedMutex);
      self->m_detachedInFlight.fetch_sub(1, std::memory_order_acq_rel);
      self->m_detachedDone.notify_all();
    }
    owner.reset();
  }

  /// Block until every in-flight detached task has decremented the counter.
  /// Idempotent; called from the destructor before the synchronous-worker
  /// shutdown path runs so detached bodies never race the pool's teardown.
  void waitForDetachedDrain() noexcept {
    std::unique_lock<std::mutex> lock(m_detachedMutex);
    m_detachedDone.wait(lock, [this]() noexcept {
      return m_detachedInFlight.load(std::memory_order_acquire) == 0U;
    });
  }

  /// pthread entry trampoline that pins the worker, registers TLS, and runs
  /// the main loop.
  static void *workerEntry(void *raw) noexcept {
    auto *arg = static_cast<WorkerSpawnArg *>(raw);
    auto &self = *(arg->pool->m_workers.get() + arg->workerIndex);
    if (self.cpuId != UINT32_MAX) {
      detail::bindAffinityOnce(self.cpuId);
    }
    auto &ctx = tlsContext();
    ctx.slot = arg->workerIndex;
    ctx.insidePoolWorker = true;
    ctx.pool = arg->pool;
    ctx.kind = arg->pool->m_kind;
    ctx.arenaIndex = arg->pool->m_arenaIndex;
    arg->pool->m_workersReady.fetch_add(1, std::memory_order_release);
    detail::workerMainLoop(self, arg->pool->m_control);
    ctx.slot = 0;
    ctx.insidePoolWorker = false;
    ctx.pool = nullptr;
    ctx.kind = PoolKind::Standalone;
    ctx.arenaIndex = 0;
    return nullptr;
  }

#ifndef __linux__
  /// `std::thread` entry used by the non-Linux fallback path; mirrors
  /// `workerEntry`. Pins the worker to its slot's reserved CPU when one is
  /// recorded (Windows path resolves real CPU ids via `detectTopology`).
  static void workerEntryStdThread(ThreadPool *self,
                                   std::size_t workerIndex) noexcept {
    auto &state = *(self->m_workers.get() + workerIndex);
    if (state.cpuId != UINT32_MAX) {
      detail::bindAffinityOnce(state.cpuId);
    }
    auto &ctx = tlsContext();
    ctx.slot = workerIndex;
    ctx.insidePoolWorker = true;
    ctx.pool = self;
    ctx.kind = self->m_kind;
    ctx.arenaIndex = self->m_arenaIndex;
    self->m_workersReady.fetch_add(1, std::memory_order_release);
    detail::workerMainLoop(state, self->m_control);
    ctx.slot = 0;
    ctx.insidePoolWorker = false;
    ctx.pool = nullptr;
    ctx.kind = PoolKind::Standalone;
    ctx.arenaIndex = 0;
  }
#endif

  /// Inline fallback: run the body over `[first, last)` directly on the
  /// producer thread. Used when the pool has only one participant or when the
  /// inline-fallback gate (`shouldRunInline`) fires. A pre-cancelled |tok|
  /// short-circuits the body. Producer TLS context is left untouched so
  /// nested-call detection still reports the outer worker's slot.
  ///
  /// Single-call flavor: invokes `fn(first, last)` once. Used when the user
  /// did not request an explicit chunk size.
  template <class F>
  void runInline(std::size_t first, std::size_t last, F &&fn,
                 const CancellationToken &tok) {
    CITOR_COUNTERS_INC(inlineFallbacks);
    if (tok.stop_requested()) {
      CITOR_COUNTERS_INC(cancellationStops);
      return;
    }
    fn(first, last);
  }

  /// Chunked inline fallback: invokes `fn(lo, hi)` for each chunk-sized span
  /// in `[first, last)`. Used by call sites that propagate a non-zero
  /// `HintsT::chunk` so the inline path observes the same chunk boundaries
  /// the parallel path would have produced. Token is polled between chunks
  /// when `cancelChecks` is true so a long range can still be cancelled
  /// mid-flight.
  template <class F>
  void runInlineChunked(std::size_t first, std::size_t last, std::size_t chunk,
                        bool cancelChecks, F &&fn,
                        const CancellationToken &tok) {
    CITOR_COUNTERS_INC(inlineFallbacks);
    if (tok.stop_requested()) {
      CITOR_COUNTERS_INC(cancellationStops);
      return;
    }
    if (chunk == 0U || chunk >= (last - first)) {
      fn(first, last);
      return;
    }
    for (std::size_t lo = first; lo < last; lo += chunk) {
      if (cancelChecks && tok.stop_requested()) {
        CITOR_COUNTERS_INC(cancellationStops);
        return;
      }
      const std::size_t hi = std::min(lo + chunk, last);
      fn(lo, hi);
    }
  }

  /// Overflow-safe ceiling division: returns `ceil(a / b)` without computing
  /// `a + b - 1`. Used wherever the chunk-shape math could otherwise wrap
  /// when |a| is near `SIZE_MAX`. |b| must be non-zero.
  static constexpr std::size_t ceilDiv(std::size_t a, std::size_t b) noexcept {
    return (a / b) + ((a % b) != 0U ? 1U : 0U);
  }

  /// Computes and stores the block shape (`chunk`, `blockCount`) on
  /// `|desc|`. The hint's `chunk` overrides when non-zero; otherwise
  /// defaults to `ceil(n / participants)` so each worker handles one
  /// contiguous block. `|oversubscribe|` selects 2x oversubscription
  /// (DynamicChunked) versus 1x (StaticUniform).
  [[gnu::always_inline]] static void
  fillBlockShape(detail::JobDescriptor &desc, std::size_t n,
                 std::size_t participants, std::size_t hintChunk,
                 bool oversubscribe = true) noexcept {
    // Callers gate `n == 0` upstream (parallelFor's `last <= first` early
    // return) and `participants` is constructor-initialised to >= 1, so
    // `ceilDiv(n, P*2)` / `ceilDiv(n, P)` and the subsequent
    // `ceilDiv(n, chunk)` are both always >= 1; the post-divide
    // `chunk == 0` / `blockCount == 0` guards the previous version carried
    // are dead.
    std::size_t chunk = hintChunk;
    if (chunk == 0) {
      // Auto-derive `chunk`. DynamicChunked benefits from 2x oversubscription
      // (the second block is the unit a fast worker absorbs when a peer
      // straggles); StaticUniform doesn't steal so 1x is enough and gives
      // every rank a single contiguous range, saving one FunctionRef call
      // per rank on cheap-body workloads.
      const std::size_t denom =
          oversubscribe ? participants * 2U : participants;
      chunk = ceilDiv(n, denom);
    }
    const std::size_t blockCount = ceilDiv(n, chunk);
    // Check-before-write keeps the desc cache line MESI-Shared across
    // iterations when the shape is loop-invariant (steady-state bench).
    if (desc.chunk != chunk) {
      desc.chunk = chunk;
    }
    if (desc.blockCount != blockCount) {
      desc.blockCount = blockCount;
    }
  }

  /// Maximum number of chunks the default reduce-chunk derivation produces.
  /// Must remain independent of the participant count so cross-`nJobs`
  /// bit-identity holds. Sized roughly twice the largest `participants` value
  /// the pool can hold so each worker gets at least two chunks for partition
  /// stability.
  static constexpr std::size_t kReduceMaxChunks = 64;
  /// Reserved stack-scratch budget for the reduce partials buffer. The
  /// reducer falls back to heap allocation when `kReduceMaxChunks *
  /// sizeof(T)` exceeds this byte budget.
  static constexpr std::size_t kReduceStackScratchBytes =
      std::size_t{32U} * 1024U;

  /// Derive a deterministic chunk size for `parallelReduce` calls. Purely a
  /// function of |n| and the optional hint chunk override and is independent of
  /// participant count. This invariant makes the chunk-id pairwise tree shape
  /// stable across worker counts (Demmel-Nguyen TOMS 2014). Never zero when
  /// `n > 0`.
  static std::size_t reduceChunkSize(std::size_t n,
                                     std::size_t hintChunk) noexcept {
    if (hintChunk != 0) {
      return hintChunk;
    }
    if (n <= kReduceMaxChunks) {
      return 1;
    }
    return ceilDiv(n, kReduceMaxChunks);
  }

  /// Inline-fallback reducer used when the pool gate routes the call to the
  /// producer. Walks the range using the same chunk schedule as the parallel
  /// path (chunk size is `f(n, HintsT)` only) so FixedBlockOrder /
  /// KahanCompensated reductions remain bit-identical to the worker-fanned-out
  /// path.
  /// Inline-fallback reducer used when the pool's participant count is one
  /// (cold path; the parallel engine takes the multi-participant path). |chunk|
  /// and |determinism| may come from a compile-time `HintsT` (typed entry) or
  /// a runtime `Hints` instance (untyped entry); the body is the same loop +
  /// pairwise-tree combine in either case.
  template <class T, class Map, class Combine>
  T runReduceInlineImpl(std::size_t first, std::size_t last, T init, Map &&map,
                        Combine &&combine, std::size_t chunkHint,
                        Determinism determinism, const CancellationToken &tok) {
    if (tok.stop_requested()) {
      throw cancelled_value_exception<T>(std::move(init));
    }
    const std::size_t n = last - first;
    const std::size_t chunk = reduceChunkSize(n, chunkHint);
    const std::size_t nChunks = ceilDiv(n, chunk);

    if (determinism == Determinism::KahanCompensated) {
      std::vector<detail::KahanPair> partials(nChunks);
      for (std::size_t b = 0; b < nChunks; ++b) {
        const std::size_t lo = first + (b * chunk);
        const std::size_t hi = std::min(lo + chunk, last);
        const T mapped = map(lo, hi);
        partials[b] =
            detail::kahanAdd(detail::KahanPair{}, static_cast<double>(mapped));
      }
      const detail::KahanPair treeResult = detail::pairwiseTreeCombine(
          partials, [](detail::KahanPair a, detail::KahanPair b) {
            return detail::kahanCombine(a, b);
          });
      const auto treeT = static_cast<T>(treeResult.sum);
      return combine(std::move(init), treeT);
    }
    std::vector<T> partials;
    partials.reserve(nChunks);
    for (std::size_t b = 0; b < nChunks; ++b) {
      const std::size_t lo = first + (b * chunk);
      const std::size_t hi = std::min(lo + chunk, last);
      partials.push_back(map(lo, hi));
    }
    auto wrappedCombine = [&combine](T a, T b) {
      return combine(std::move(a), std::move(b));
    };
    T treeResult = detail::pairwiseTreeCombine(partials, wrappedCombine);
    return combine(std::move(init), std::move(treeResult));
  }

  /// Compile-time-hinted wrapper over `runReduceInlineImpl` that pulls
  /// `chunk` and `determinism` from `HintsT`.
  template <class HintsT, class T, class Map, class Combine>
  T runReduceInline(std::size_t first, std::size_t last, T init, Map &&map,
                    Combine &&combine, const CancellationToken &tok) {
    return runReduceInlineImpl(first, last, std::move(init),
                               std::forward<Map>(map),
                               std::forward<Combine>(combine), HintsT::chunk,
                               HintsT::determinism, tok);
  }

  /// Runtime-hint wrapper over `runReduceInlineImpl` that forwards the
  /// runtime `|hints|` chunk and determinism instead of a compile-time
  /// `HintsT`.
  template <class T, class Map, class Combine>
  T runReduceInlineRuntime(std::size_t first, std::size_t last, T init,
                           Map &&map, Combine &&combine, const Hints &hints,
                           const CancellationToken &tok) {
    return runReduceInlineImpl(
        first, last, std::move(init), std::forward<Map>(map),
        std::forward<Combine>(combine), hints.chunk, hints.determinism, tok);
  }

  /// Parallel reducer engine for a compile-time `HintsT`. Allocates a
  /// `std::vector<PartialSlot>` (per-element cache-line padded to avoid
  /// false-sharing across adjacent chunk slots) sized to `nChunks`, dispatches
  /// a `void(lo, hi)` worker body that computes the partial via |map| and
  /// stores into `partials[blockId]`, then combines via the chunk-id pairwise
  /// tree. The Kahan path wraps `T` in `KahanPair`.
  template <class HintsT, class T, class Map, class Combine>
  T runReduceParallel(std::size_t first, std::size_t last, T init, Map &&map,
                      Combine &&combine, CancellationToken tok,
                      std::size_t participants) {
    const std::size_t n = last - first;
    const std::size_t chunk = reduceChunkSize(n, HintsT::chunk);
    const std::size_t nChunks = ceilDiv(n, chunk);
    constexpr bool kCancellationActive = detail::kCancellationActive<HintsT>;

    if constexpr (HintsT::determinism == Determinism::KahanCompensated) {
      if constexpr (!kCancellationActive) {
        auto runWithPartials = [&](auto &partials,
                                   std::size_t partialCount) -> T {
          auto bodyWrapper = [&](std::size_t blockId, std::size_t lo,
                                 std::size_t hi) {
            const T mapped = map(lo, hi);
            partials[blockId] = detail::kahanAdd(detail::KahanPair{},
                                                 static_cast<double>(mapped));
          };
          dispatchReduceJobTyped<HintsT>(first, last, bodyWrapper,
                                         std::move(tok), participants, chunk,
                                         nChunks);

          std::size_t step = 1;
          while (step < partialCount) {
            const std::size_t stride = step * 2U;
            for (std::size_t i = 0; i + step < partialCount; i += stride) {
              partials[i] =
                  detail::kahanCombine(partials[i], partials[i + step]);
            }
            step = stride;
          }
          const auto treeT = static_cast<T>(partials.front().sum);
          return combine(std::move(init), treeT);
        };

        if constexpr (sizeof(detail::KahanPair) * kReduceMaxChunks <=
                      kReduceStackScratchBytes) {
          if (nChunks <= kReduceMaxChunks) {
            std::array<detail::KahanPair, kReduceMaxChunks> partials;
            return runWithPartials(partials, nChunks);
          }
        }
        std::vector<detail::KahanPair> partials(nChunks);
        return runWithPartials(partials, nChunks);
      }

      // Kahan path: accumulate per chunk into a KahanPair then
      // compensate-combine across chunks. The user's combine is treated as `+`;
      // for non-additive Kahan use cases the caller picks FixedBlockOrder
      // instead. The completion flag lives on the same padded line as the
      // partial so workers writing different chunks never share a cache line.
      struct alignas(kCacheLine) KahanSlot {
        detail::KahanPair value{};
        std::uint8_t done = 0;
      };

      auto runWithPartials = [&](auto &partials,
                                 std::size_t partialCount) -> T {
        auto bodyWrapper = [&](std::size_t blockId, std::size_t lo,
                               std::size_t hi) {
          const T mapped = map(lo, hi);
          partials[blockId].value = detail::kahanAdd(
              detail::KahanPair{}, static_cast<double>(mapped));
          partials[blockId].done = 1;
        };
        dispatchReduceJobTyped<HintsT>(first, last, bodyWrapper, std::move(tok),
                                       participants, chunk, nChunks);

        if constexpr (!kCancellationActive) {
          std::size_t step = 1;
          while (step < partialCount) {
            const std::size_t stride = step * 2U;
            for (std::size_t i = 0; i + step < partialCount; i += stride) {
              partials[i].value = detail::kahanCombine(
                  partials[i].value, partials[i + step].value);
            }
            step = stride;
          }
          const auto treeT = static_cast<T>(partials.front().value.sum);
          return combine(std::move(init), treeT);
        }

        // Single pass over `partials[]`: collect completed chunks into `flat[]`
        // and remember whether any chunk was missed in one cache-line walk
        // instead of two.
        std::vector<detail::KahanPair> flat;
        flat.reserve(partialCount);
        bool anyMissed = false;
        for (std::size_t i = 0; i < partialCount; ++i) {
          if (partials[i].done != 0U) {
            flat.push_back(partials[i].value);
          } else {
            anyMissed = true;
          }
        }
        // Empty partials (every chunk cancelled before completing): return
        // |init| unchanged via the cancelled-value exception. We do NOT
        // combine `init` with `T{}` because the user |combine| is not
        // required to satisfy `combine(x, T{}) == x`.
        if (flat.empty()) {
          throw cancelled_value_exception<T>(std::move(init));
        }
        const detail::KahanPair treeResult = detail::pairwiseTreeCombine(
            flat, [](detail::KahanPair a, detail::KahanPair b) {
              return detail::kahanCombine(a, b);
            });
        const auto treeT = static_cast<T>(treeResult.sum);
        T combined = combine(std::move(init), treeT);
        if (anyMissed) {
          throw cancelled_value_exception<T>(std::move(combined));
        }
        return combined;
      };

      if constexpr (sizeof(KahanSlot) * kReduceMaxChunks <=
                    kReduceStackScratchBytes) {
        if (nChunks <= kReduceMaxChunks) {
          std::array<KahanSlot, kReduceMaxChunks> partials;
          return runWithPartials(partials, nChunks);
        }
      }
      std::vector<KahanSlot> partials(nChunks);
      return runWithPartials(partials, nChunks);
    } else {
      if constexpr (!kCancellationActive) {
        auto runWithPartials = [&](auto &partials,
                                   std::size_t partialCount) -> T {
          auto bodyWrapper = [&](std::size_t blockId, std::size_t lo,
                                 std::size_t hi) {
            partials[blockId] = map(lo, hi);
          };
          dispatchReduceJobTyped<HintsT>(first, last, bodyWrapper,
                                         std::move(tok), participants, chunk,
                                         nChunks);

          auto wrappedCombine = [&combine](T a, T b) {
            return combine(std::move(a), std::move(b));
          };
          std::size_t step = 1;
          while (step < partialCount) {
            const std::size_t stride = step * 2U;
            for (std::size_t i = 0; i + step < partialCount; i += stride) {
              partials[i] = wrappedCombine(std::move(partials[i]),
                                           std::move(partials[i + step]));
            }
            step = stride;
          }
          return combine(std::move(init), std::move(partials.front()));
        };

        if constexpr (sizeof(T) * kReduceMaxChunks <=
                      kReduceStackScratchBytes) {
          if (nChunks <= kReduceMaxChunks) {
            std::array<T, kReduceMaxChunks> partials;
            return runWithPartials(partials, nChunks);
          }
        }
        std::vector<T> partials(nChunks);
        return runWithPartials(partials, nChunks);
      }

      // FixedBlockOrder / OrderTolerant share the same dispatch shape; the only
      // difference is whether the caller's combine is
      // bit-reproducible-friendly. We still use the chunk-id pairwise tree so
      // FixedBlockOrder is bit-identical across worker counts.
      struct alignas(kCacheLine) Slot {
        T value;
        std::uint8_t done = 0;
      };

      auto runWithPartials = [&](auto &partials,
                                 std::size_t partialCount) -> T {
        auto bodyWrapper = [&](std::size_t blockId, std::size_t lo,
                               std::size_t hi) {
          partials[blockId].value = map(lo, hi);
          partials[blockId].done = 1;
        };
        dispatchReduceJobTyped<HintsT>(first, last, bodyWrapper, std::move(tok),
                                       participants, chunk, nChunks);

        if constexpr (!kCancellationActive) {
          auto wrappedCombine = [&combine](T a, T b) {
            return combine(std::move(a), std::move(b));
          };
          std::size_t step = 1;
          while (step < partialCount) {
            const std::size_t stride = step * 2U;
            for (std::size_t i = 0; i + step < partialCount; i += stride) {
              partials[i].value =
                  wrappedCombine(std::move(partials[i].value),
                                 std::move(partials[i + step].value));
            }
            step = stride;
          }
          return combine(std::move(init), std::move(partials.front().value));
        }

        // Single pass over `partials[]`: collect completed chunks into `flat[]`
        // and remember whether any chunk was missed in one cache-line walk
        // instead of two.
        std::vector<T> flat;
        flat.reserve(partialCount);
        bool anyMissed = false;
        for (std::size_t i = 0; i < partialCount; ++i) {
          if (partials[i].done != 0U) {
            flat.push_back(std::move(partials[i].value));
          } else {
            anyMissed = true;
          }
        }
        // Empty partials (every chunk cancelled): return |init| unchanged. See
        // the Kahan branch above for why combining with `T{}` is unsafe for
        // arbitrary user combiners.
        if (flat.empty()) {
          throw cancelled_value_exception<T>(std::move(init));
        }
        auto wrappedCombine = [&combine](T a, T b) {
          return combine(std::move(a), std::move(b));
        };
        T treeResult = detail::pairwiseTreeCombine(flat, wrappedCombine);
        T combined = combine(std::move(init), std::move(treeResult));
        if (anyMissed) {
          throw cancelled_value_exception<T>(std::move(combined));
        }
        return combined;
      };

      if constexpr (sizeof(Slot) * kReduceMaxChunks <=
                    kReduceStackScratchBytes) {
        if (nChunks <= kReduceMaxChunks) {
          std::array<Slot, kReduceMaxChunks> partials;
          return runWithPartials(partials, nChunks);
        }
      }
      // Default-construct via `resize`: `Slot::value` is read only when
      // `done != 0`, so leaving `value` uninitialized for unused slots saves
      // one fill-copy per chunk and drops the copy-constructible requirement
      // on `T` for slot construction.
      std::vector<Slot> partials;
      partials.resize(nChunks);
      return runWithPartials(partials, nChunks);
    }
  }

  /// Runtime-hint parallel reducer; selects the determinism path via a runtime
  /// branch.
  template <class T, class Map, class Combine>
  T runReduceParallelRuntime(std::size_t first, std::size_t last, T init,
                             Map &&map, Combine &&combine, const Hints &hints,
                             CancellationToken tok, std::size_t participants) {
    const std::size_t n = last - first;
    const std::size_t chunk = reduceChunkSize(n, hints.chunk);
    const std::size_t nChunks = ceilDiv(n, chunk);

    if (hints.determinism == Determinism::KahanCompensated) {
      struct alignas(kCacheLine) KahanSlot {
        detail::KahanPair value{};
        std::uint8_t done = 0;
      };
      std::vector<KahanSlot> partials(nChunks);

      auto bodyWrapper = [&](std::size_t lo, std::size_t hi) {
        const std::size_t blockId = (lo - first) / chunk;
        const T mapped = map(lo, hi);
        partials[blockId].value =
            detail::kahanAdd(detail::KahanPair{}, static_cast<double>(mapped));
        partials[blockId].done = 1;
      };
      const FunctionRef<void(std::size_t, std::size_t)> body{bodyWrapper};
      dispatchReduceJob(first, last, body, std::move(tok), participants, chunk,
                        nChunks);

      std::vector<detail::KahanPair> flat;
      flat.reserve(nChunks);
      bool anyMissed = false;
      for (std::size_t i = 0; i < nChunks; ++i) {
        if (partials[i].done != 0U) {
          flat.push_back(partials[i].value);
        } else {
          anyMissed = true;
        }
      }
      if (flat.empty()) {
        throw cancelled_value_exception<T>(std::move(init));
      }
      const detail::KahanPair treeResult = detail::pairwiseTreeCombine(
          flat, [](detail::KahanPair a, detail::KahanPair b) {
            return detail::kahanCombine(a, b);
          });
      const auto treeT = static_cast<T>(treeResult.sum);
      T combined = combine(std::move(init), treeT);
      if (anyMissed) {
        throw cancelled_value_exception<T>(std::move(combined));
      }
      return combined;
    }

    struct alignas(kCacheLine) Slot {
      T value;
      std::uint8_t done = 0;
    };
    std::vector<Slot> partials;
    partials.resize(nChunks);

    auto bodyWrapper = [&](std::size_t lo, std::size_t hi) {
      const std::size_t blockId = (lo - first) / chunk;
      partials[blockId].value = map(lo, hi);
      partials[blockId].done = 1;
    };
    const FunctionRef<void(std::size_t, std::size_t)> body{bodyWrapper};
    dispatchReduceJob(first, last, body, std::move(tok), participants, chunk,
                      nChunks);

    std::vector<T> flat;
    flat.reserve(nChunks);
    bool anyMissed = false;
    for (std::size_t i = 0; i < nChunks; ++i) {
      if (partials[i].done != 0U) {
        flat.push_back(std::move(partials[i].value));
      } else {
        anyMissed = true;
      }
    }
    if (flat.empty()) {
      throw cancelled_value_exception<T>(std::move(init));
    }
    auto wrappedCombine = [&combine](T a, T b) {
      return combine(std::move(a), std::move(b));
    };
    T treeResult = detail::pairwiseTreeCombine(flat, wrappedCombine);
    T combined = combine(std::move(init), std::move(treeResult));
    if (anyMissed) {
      throw cancelled_value_exception<T>(std::move(combined));
    }
    return combined;
  }

  /// Single-thread fallback: run every phase of the plex on the producer
  /// thread. Triggered when the pool has at most one participant. Slot 0 owns
  /// the entire range `[0, n)`. `prePhaseFn(phaseIdx)` is invoked before each
  /// phase; cancellation is observed before each phase and the call returns
  /// cleanly when the token is stopped.
  template <class Phase, class PrePhase>
  void runPlexInline(std::size_t nPhases, std::size_t n, Phase &&phaseFn,
                     PrePhase &&prePhaseFn, const CancellationToken &tok) {
    for (std::size_t p = 0; p < nPhases; ++p) {
      if (tok.stop_requested()) {
        return;
      }
      prePhaseFn(p);
      phaseFn(p, std::uint32_t{0}, std::size_t{0}, n);
    }
  }

  /// Persistent-worker plex driver: publish one dispatch and drive `nPhases`
  /// phases.
  ///
  /// Allocates a stack-resident `PlexState` with per-worker `done[]` slots on
  /// dedicated lines. The wrapper body, invoked with `(slot, slot+1)`, runs
  /// the per-slot phase loop: spin until `currentPhase >= localPhase`, run
  /// `phaseFn`, signal `done[slot]`. The dispatch publishes a single job with
  /// `blockCount = participants` and `chunk = 1` so every worker receives
  /// exactly one body invocation; `runStaticPartition` matches block id to
  /// slot id. The producer participates as slot 0; its body drives the phase
  /// epoch (publish next phase, run slot 0's `phaseFn`, then spin-wait on
  /// every other slot's `done`). The descriptor's join after dispatch returns
  /// establishes happens-before for every worker's writes; any captured
  /// exception is rethrown by `dispatchOne`.
  ///
  /// Wait for every non-producer slot to reach |pStamp| on its `done`
  /// counter. Used by `runPlexParallel` to close a phase: the producer joins
  /// on every other slot's `done >= pStamp` before publishing the next phase
  /// (or before returning on a cancellation path).
  [[gnu::always_inline]] static void
  waitPlexSlotsReachStamp(detail::PlexState &state,
                          std::uint64_t pStamp) noexcept {
    for (std::uint32_t s = 1; s < state.participants; ++s) {
      while (true) {
        const std::uint64_t obs =
            state.doneSlot(s).done.load(std::memory_order_acquire);
        if (obs >= pStamp) {
          break;
        }
        detail::cpuRelax();
      }
    }
  }

  /// Pre-phase hook: the producer invokes `prePhaseFn(p - 1)` before each
  /// release-store on `currentPhase`. The hook synchronises with workers'
  /// acquire-spin via the same release-store, so any state the hook writes is
  /// visible to every worker that observes the new phase.
  template <class Phase, class PrePhase>
  void runPlexParallel(std::size_t nPhases, std::size_t n, Phase &&phaseFn,
                       PrePhase &&prePhaseFn, CancellationToken tok,
                       std::size_t participants) {
    // Acquire the dispatch gate BEFORE touching pool-owned scratch
    // (`m_plexDoneSlots`, `m_plexEpochBase`). Concurrent producers would
    // otherwise race on the epoch advance.
    const DispatchLease lease(*this, Priority::Throughput);

    detail::PlexState state;
    state.nPhases = nPhases;
    state.n = n;
    state.participants = static_cast<std::uint32_t>(participants);
    state.doneSlots = m_plexDoneSlots.get();
    // Reserve a fresh epoch interval in `m_plexEpochBase`. Workers stamp
    // absolute targets
    // (`epochBase + p`); the next dispatch starts at `epochBase + nPhases + 1`,
    // so prior-call stamps cannot satisfy this call's waits. The wrap guard
    // zero-resets and rebases when the counter approaches `UINT64_MAX`; with
    // typical advance rates the branch is unreachable.
    if (m_plexEpochBase > UINT64_MAX - 1024U) [[unlikely]] {
      auto *slotsBase = m_plexDoneSlots.get();
      for (std::size_t i = 0; i < participants; ++i) {
        slotsBase[i].done.store(0, std::memory_order_relaxed);
      }
      m_plexEpochBase = 0;
    }
    state.epochBase = m_plexEpochBase;
    m_plexEpochBase += static_cast<std::uint64_t>(nPhases) + 1U;

    // The plex's cancellation token lives on the producer's stack; we DO NOT
    // move it into the descriptor's token field because `runStaticPartition`
    // consults `desc.token.stop_requested` before admitting each block. With
    // `blockCount = participants` and `chunk = 1`, every slot has exactly one
    // block; a stopped token would cause workers to skip their slot body
    // entirely, deadlocking the producer's slot-0 join. The wrapper observes
    // the token directly at phase boundaries instead.
    CancellationToken plexTok = std::move(tok);

    // The wrapper body is invoked once per slot (chunkBlock == 1). Each
    // invocation runs the full phase loop for that slot. Slot 0 (the producer)
    // drives the phase epoch and joins on every other slot's `done` between
    // phases; non-zero slots spin-wait on `currentPhase` and signal `done`
    // after each phase's work.
    auto bodyWrapper = [&state, &phaseFn, &prePhaseFn,
                        &plexTok](std::size_t lo, std::size_t /*hi*/) {
      const auto slot = static_cast<std::uint32_t>(lo);
      const std::size_t totalPhases = state.nPhases;
      const std::uint64_t epochBase = state.epochBase;
      const std::uint64_t finalStamp =
          epochBase + static_cast<std::uint64_t>(totalPhases);
      const auto [slotLo, slotHi] = state.slotRange(slot);

      if (slot == 0U) {
        // Producer drives the phase epoch.
        for (std::size_t p = 1; p <= totalPhases; ++p) {
          const std::uint64_t pStamp =
              epochBase + static_cast<std::uint64_t>(p);
          // Cancellation check before publishing the next phase. Stop on either
          // an external stop request OR a peer-observed throw / cancellation,
          // so the producer does not run prePhaseFn or slot-0's phaseFn for
          // phases beyond the failure.
          if (plexTok.stop_requested() ||
              state.phaseCancelled.load(std::memory_order_acquire) != 0U) {
            state.phaseCancelled.store(1U, std::memory_order_release);
            // Publish the cancelled epoch so workers leave their spin loops.
            state.currentPhase.store(p, std::memory_order_release);
            // Wait for every worker to observe and exit the phase loop. They
            // will set `done[slot]` to the final epoch stamp so we know they
            // have left.
            waitPlexSlotsReachStamp(state, pStamp);
            return;
          }
          // Pre-phase hook: write any shared state the upcoming phase reads.
          // The release-store on `currentPhase` below publishes these writes to
          // every worker.
          try {
            prePhaseFn(p - 1);
          } catch (...) {
            captureFirstException(state);
            state.phaseCancelled.store(1U, std::memory_order_release);
            state.currentPhase.store(p, std::memory_order_release);
            state.doneSlot(0).done.store(pStamp, std::memory_order_release);
            waitPlexSlotsReachStamp(state, pStamp);
            return;
          }
          // Publish the next phase via release: workers' acquire-spin observes
          // the new value and proceeds to run their slice for phase index `p -
          // 1`.
          state.currentPhase.store(p, std::memory_order_release);
          // Producer runs slot 0's slice for this phase.
          try {
            phaseFn(p - 1, std::uint32_t{0}, slotLo, slotHi);
          } catch (...) {
            captureFirstException(state);
            // Mark cancelled so workers exit their loops, then close the phase.
            state.phaseCancelled.store(1U, std::memory_order_release);
            state.doneSlot(0).done.store(pStamp, std::memory_order_release);
            waitPlexSlotsReachStamp(state, pStamp);
            return;
          }
          state.doneSlot(0).done.store(pStamp, std::memory_order_release);
          // Producer joins on every other slot's `done >= pStamp` before
          // publishing the next phase.
          waitPlexSlotsReachStamp(state, pStamp);
        }
      } else {
        // Background worker: spin on `currentPhase` and run our slice once per
        // phase.
        for (std::size_t p = 1; p <= totalPhases; ++p) {
          const std::uint64_t pStamp =
              epochBase + static_cast<std::uint64_t>(p);
          // Acquire-spin on `currentPhase` until the producer publishes our
          // next phase OR cancellation is observed. Both `currentPhase` advance
          // and `phaseCancelled` flip are release-stores by the producer; the
          // worker's acquire-loads pair with them so any state the producer
          // wrote is visible here.
          while (true) {
            const std::uint64_t observed =
                state.currentPhase.load(std::memory_order_acquire);
            if (observed >= p) {
              break;
            }
            if (state.phaseCancelled.load(std::memory_order_acquire) != 0U) {
              // Producer threw or cancelled before publishing this phase. Stamp
              // the final phase epoch on `done` so any future producer-side
              // join (none expected here, but the contract is the descriptor's
              // join sees a deterministic epoch) advances.
              state.doneSlot(slot).done.store(finalStamp,
                                              std::memory_order_release);
              return;
            }
            detail::cpuRelax();
          }
          if (state.phaseCancelled.load(std::memory_order_acquire) != 0U) {
            // Producer signalled cancellation while publishing this phase.
            // Signal we have observed the current phase so the producer's exit
            // join advances, then drain the remaining epochs.
            state.doneSlot(slot).done.store(pStamp, std::memory_order_release);
            if (p < totalPhases) {
              state.doneSlot(slot).done.store(finalStamp,
                                              std::memory_order_release);
            }
            return;
          }
          try {
            phaseFn(p - 1, slot, slotLo, slotHi);
          } catch (...) {
            captureFirstException(state);
            state.phaseCancelled.store(1U, std::memory_order_release);
            // Stamp the final epoch so the producer's exit join completes.
            state.doneSlot(slot).done.store(finalStamp,
                                            std::memory_order_release);
            return;
          }
          // Release so the producer's acquire-load observes our writes.
          state.doneSlot(slot).done.store(pStamp, std::memory_order_release);
        }
      }
    };

    const FunctionRef<void(std::size_t, std::size_t)> body{bodyWrapper};

    detail::JobDescriptor desc;
    desc.first = 0;
    desc.last = participants;
    desc.participants = static_cast<std::uint32_t>(participants);
    desc.balance = Balance::StaticUniform;
    desc.priority = Priority::Throughput;
    desc.body = body;
    // Leave `desc.token` default-constructed: the descriptor's stop-check would
    // gate `runStaticPartition` from admitting per-slot blocks, deadlocking the
    // producer's join. The wrapper observes `plexTok` at phase boundaries
    // instead.
    desc.chunk = 1;
    desc.blockCount = participants;

    // The dispatch gate is already held via `lease` above (so the epoch
    // reservation is serialized against concurrent producers); use the locked
    // variant which skips the redundant gate acquisition.
    dispatchOneStaticLocked<Balance::StaticUniform>(desc);

    // `dispatchOneStaticLocked` rethrows exceptions captured by
    // `desc.firstException`; the body wrapper routes plex-thrown exceptions
    // through `state.firstException` instead so the descriptor's capture slot
    // stays clean. Rethrow here.
    rethrowIfCaptured(state);
  }

  /// First-exception capture shared by every primitive (plex, chain, scan,
  /// forkJoin, dispatch). Stored on the protocol's state so the descriptor's
  /// own slot remains untouched (workers may also throw from the wrapper body
  /// itself, which the dispatch engine captures separately). The first thread
  /// to win the CAS owns the exception pointer; subsequent throws delete their
  /// own copy. |State| must expose a `firstException` atomic of
  /// `exception_ptr*`.
  template <class State>
  [[gnu::always_inline]] static void
  captureFirstException(State &state) noexcept {
    auto *eptr =
        new (std::nothrow) std::exception_ptr(std::current_exception());
    if (eptr == nullptr) {
      std::terminate();
    }
    std::exception_ptr *expected = nullptr; // NOLINT(misc-const-correctness)
    if (!state.firstException.compare_exchange_strong(
            expected, eptr, std::memory_order_release,
            std::memory_order_acquire)) {
      delete eptr;
    }
  }

  /// `BarrierKind::Global` rendezvous: each slot scans every peer's `done`
  /// epoch until all have reached |target|. Used by every slot (producer and
  /// non-producers alike) for `BarrierKind::Global` and
  /// `BarrierKind::DeterministicReduce`. Each slot stamps its own `done` to
  /// |target| then scans every peer's `done` line until all have reached
  /// |target|.
  ///
  /// Coherence model: each slot's `done` line is on its own cache line (no
  /// false sharing). After a slot's release-store the line transitions
  /// Modified on the writing core; readers fetch it via Modified -> Shared
  /// transitions. Because every slot scans in parallel, the wall-clock cost
  /// of the rendezvous is bounded by `(slowest_writer_finish_time) + (one
  /// cross-core coherence pull per peer)`, not by a producer's serial
  /// scan-then-publish chain.
  ///
  /// Cancellation handshake: a slot bailing on cancellation stamps
  /// `done = nStages` instead of |target|. Because `nStages > target` for
  /// every active stage, the wait condition `done >= target` is satisfied
  /// unconditionally, so waiters exit cleanly without an explicit
  /// `chainCancelled` poll inside the spin loop. The slot's own `done` line
  /// is skipped via the `s != self` guard so the loop does not pay an extra
  /// L1-load for self-traffic.
  static void waitAllSlotsDone(detail::ChainState &state, std::uint32_t self,
                               std::uint64_t target) noexcept {
    const std::uint64_t absoluteTarget = state.epochBase + target;
    const std::uint32_t participants = state.participants;
    // Phase-shift the peer scan: each slot starts from `(self + 1) mod
    // participants` and walks forward, so the first lines pulled by every slot
    // are different. The naive `for (s = 0; s < participants; ++s)` order has
    // every slot probing slot 0 first, which spikes contention on slot 0's
    // cache line at every barrier; the shifted order distributes the initial
    // pulls across all peer lines.
    for (std::uint32_t step = 1; step < participants; ++step) {
      const std::uint32_t s = (self + step < participants)
                                  ? self + step
                                  : self + step - participants;
      while (state.doneSlot(s).done.load(std::memory_order_acquire) <
             absoluteTarget) {
        detail::cpuRelax();
      }
    }
  }

  /// Wait for slot 0's `done` epoch to reach |target| before returning. Used
  /// for `BarrierKind::ProducerSerial`: workers other than slot 0 spin on the
  /// producer's release-store before either skipping the next stage's body or
  /// proceeding. Cancellation handshake mirrors `waitAllSlotsDone`: a
  /// cancelled producer stamps `done[0] = nStages` so the wait condition is
  /// satisfied without an explicit `chainCancelled` poll inside the spin loop.
  static void waitProducerSerialBarrier(detail::ChainState &state,
                                        std::uint64_t target) noexcept {
    const std::uint64_t absoluteTarget = state.epochBase + target;
    while (state.doneSlot(0).done.load(std::memory_order_acquire) <
           absoluteTarget) {
      detail::cpuRelax();
    }
  }

  /// Recursive helper: run stage |I| (and every later stage) for one slot.
  /// Uses a compile-time `if constexpr` ladder over `BarrierKind` so each
  /// stage's barrier becomes a branchless inlined call. The recursion
  /// terminates when `I == sizeof...(Stages)`.
  ///
  /// Pre-stage entry (driven by stage `I-1`'s barrier):
  /// - `None`: no wait. Each slot pipelines forward independently.
  /// - `Global` / `DeterministicReduce`: every slot scans every peer's
  ///   `done >= I` independently; no participant acts as a serial gate.
  ///   Coherence traffic per stage is bounded by the slowest writer's
  ///   release-store plus one cross-core pull per peer per slot, executed
  ///   in parallel across slots.
  /// - `ProducerSerial`: non-producers wait on slot 0's `done >= I`;
  ///   producer skips the wait.
  ///
  /// Stage execution:
  /// - `ProducerSerial`: only slot 0 invokes the body; other slots stamp
  ///   their `done` and skip.
  /// - Otherwise: every slot invokes the body for its `(slotLo, slotHi)`.
  ///
  /// Cancellation: between every stage the slot checks `chainCancelled`;
  /// when set, the slot stamps `done` to the final stage epoch and returns.
  template <std::size_t I, class StagesTuple>
  void runChainStageForSlot(std::uint32_t slot, std::size_t slotLo,
                            std::size_t slotHi, detail::ChainState &state,
                            StagesTuple &stages,
                            const CancellationToken &tok) noexcept {
    if constexpr (I < std::tuple_size_v<StagesTuple>) {
      // Apply pre-stage barrier driven by stage `I-1`'s post-stage barrier
      // kind. `None` means "no cross-slot wait" -- the chain keeps each slot on
      // its own contiguous chunk across stages, so the slot's prior
      // release-store satisfies any implicit dependency on its own upstream
      // output.
      const bool slotRunsBody = [&]() noexcept {
        if constexpr (I > 0) {
          using PrevStage = std::tuple_element_t<I - 1, StagesTuple>;
          constexpr BarrierKind kPrevBarrier = PrevStage::barrier;
          if constexpr (kPrevBarrier == BarrierKind::Global ||
                        kPrevBarrier == BarrierKind::DeterministicReduce) {
            // Decentralized Global rendezvous: every slot scans every peer's
            // `done` line in parallel. Each slot pays one cross-core pull per
            // peer, but the pulls happen in parallel across slots so the wall
            // time is bounded by the slowest writer plus one peer-scan latency
            // rather than by a producer's serial scan-then-publish chain.
            waitAllSlotsDone(state, slot, static_cast<std::uint64_t>(I));
            return true;
          } else if constexpr (kPrevBarrier == BarrierKind::ProducerSerial) {
            if (slot != 0U) {
              // Wait for slot 0 to finish stage `I` itself (its post-stage
              // stamp is `I + 1`), not just the previous stage. Otherwise a
              // non-producer skipping stage `I` could race past slot 0's
              // still-running serial body and enter stage `I + 1`.
              waitProducerSerialBarrier(state,
                                        static_cast<std::uint64_t>(I + 1));
              return false;
            }
            return true;
          } else {
            return true;
          }
        } else {
          return true;
        }
      }();

      // Cancellation observed at the stage boundary: stamp the final epoch and
      // bail.
      if (state.chainCancelled.load(std::memory_order_acquire) != 0U ||
          tok.stop_requested()) {
        if (tok.stop_requested()) {
          state.chainCancelled.store(1U, std::memory_order_release);
        }
        state.doneSlot(slot).done.store(
            state.epochBase + static_cast<std::uint64_t>(state.nStages),
            std::memory_order_release);
        return;
      }

      if (slotRunsBody) {
        try {
          std::get<I>(stages).fn(I, slot, slotLo, slotHi);
        } catch (...) {
          captureFirstException(state);
          state.chainCancelled.store(1U, std::memory_order_release);
          state.doneSlot(slot).done.store(
              state.epochBase + static_cast<std::uint64_t>(state.nStages),
              std::memory_order_release);
          return;
        }
      }

      // Stamp this slot's `done` for stage `I` only when a downstream consumer
      // might wait on it. Two readers exist:
      //   - The next stage's pre-barrier scan when THIS stage's barrier is
      //   Global /
      //     DeterministicReduce (reads `done[*] >= I + 1`).
      //   - Slot 0 in particular: when the PREV stage's barrier was
      //   ProducerSerial, every
      //     non-producer slot spins on `done[0] >= I + 1` to know slot 0
      //     finished THIS (the producer-serial) stage. Only slot 0 needs to
      //     stamp in that case.
      //   - When THIS stage's barrier is ProducerSerial, the next stage's
      //   pre-barrier wait is
      //     the ProducerSerial wait above (handled by THE NEXT stage via
      //     prev-was-PS), so the stamp is still required for slot 0 here too.
      // Other combinations (None post-stage with non-PS pre-stage) carry no
      // observable consumer and the release-store is dead weight.
      using ThisStage = std::tuple_element_t<I, StagesTuple>;
      constexpr BarrierKind kThisBarrier = ThisStage::barrier;
      constexpr bool kThisStampNeeded =
          (kThisBarrier == BarrierKind::Global ||
           kThisBarrier == BarrierKind::DeterministicReduce ||
           kThisBarrier == BarrierKind::ProducerSerial);
      constexpr bool kPrevWasProducerSerial = []() noexcept {
        if constexpr (I > 0) {
          using PrevStage = std::tuple_element_t<I - 1, StagesTuple>;
          return PrevStage::barrier == BarrierKind::ProducerSerial;
        } else {
          return false;
        }
      }();

      if constexpr (kThisStampNeeded) {
        state.doneSlot(slot).done.store(state.epochBase +
                                            static_cast<std::uint64_t>(I + 1),
                                        std::memory_order_release);
      } else if constexpr (kPrevWasProducerSerial) {
        if (slot == 0U) {
          state.doneSlot(0).done.store(state.epochBase +
                                           static_cast<std::uint64_t>(I + 1),
                                       std::memory_order_release);
        }
      }

      runChainStageForSlot<I + 1>(slot, slotLo, slotHi, state, stages, tok);
    }
  }

  /// True when `StageT`'s barrier permits the chain to use the
  /// dynamic-rebalance scheduler (`Global` or `DeterministicReduce`).
  template <class StageT>
  static consteval bool chainStageAllowsDynamicRebalance() noexcept {
    return StageT::barrier == BarrierKind::Global ||
           StageT::barrier == BarrierKind::DeterministicReduce;
  }

  /// Index-pack helper for `allChainStagesAllowDynamicRebalance`. Folds
  /// `chainStageAllowsDynamicRebalance` across every stage in the tuple.
  template <class StagesTuple, std::size_t... Is>
  static consteval bool allChainStagesAllowDynamicRebalanceImpl(
      std::index_sequence<Is...> /*unused*/) noexcept {
    return (... && chainStageAllowsDynamicRebalance<
                       std::tuple_element_t<Is, StagesTuple>>());
  }

  /// True when every stage in the chain permits dynamic rebalance. Used
  /// at the top of the chain dispatch to pick the scheduler.
  template <class StagesTuple>
  static consteval bool allChainStagesAllowDynamicRebalance() noexcept {
    return allChainStagesAllowDynamicRebalanceImpl<StagesTuple>(
        std::make_index_sequence<std::tuple_size_v<StagesTuple>>{});
  }

  /// Returns the per-stage block size for the dynamic chain scheduler.
  /// Honours `|hintChunk|` when non-zero; otherwise picks
  /// `ceil(n / (participants * 2))` so the scheduler has 2x
  /// oversubscription per slot for steal-driven rebalance.
  static constexpr std::size_t
  chainDynamicChunkSize(std::size_t n, std::size_t participants,
                        std::size_t hintChunk) noexcept {
    if (hintChunk != 0U) {
      return hintChunk;
    }
    return ceilDiv(n, participants * 2U);
  }

  /// Slot's main loop for the dynamic chain scheduler. Per stage, waits
  /// for the previous stage's barrier, then drains a per-stage atomic
  /// counter for blocks until the chain completes or is cancelled.
  template <std::size_t I, class StagesTuple>
  void runDynamicChainStageForSlot(std::uint32_t slot,
                                   detail::ChainState &state,
                                   StagesTuple &stages,
                                   const CancellationToken &tok) noexcept {
    if constexpr (I < std::tuple_size_v<StagesTuple>) {
      if constexpr (I > 0) {
        waitAllSlotsDone(state, slot, static_cast<std::uint64_t>(I));
      }

      if (state.chainCancelled.load(std::memory_order_acquire) != 0U ||
          tok.stop_requested()) {
        if (tok.stop_requested()) {
          state.chainCancelled.store(1U, std::memory_order_release);
        }
        state.doneSlot(slot).done.store(
            state.epochBase + static_cast<std::uint64_t>(state.nStages),
            std::memory_order_release);
        return;
      }

      auto &stageCounter = state.dynamicStageCounter(I);
      while (state.chainCancelled.load(std::memory_order_acquire) == 0U) {
        const std::size_t block =
            stageCounter.next.fetch_add(1, std::memory_order_relaxed);
        if (block >= state.dynamicBlockCount) {
          break;
        }
        const auto [lo, hi] = state.dynamicBlockRange(block);
        try {
          std::get<I>(stages).fn(I, slot, lo, hi);
        } catch (...) {
          captureFirstException(state);
          state.chainCancelled.store(1U, std::memory_order_release);
          state.doneSlot(slot).done.store(
              state.epochBase + static_cast<std::uint64_t>(state.nStages),
              std::memory_order_release);
          return;
        }
      }

      state.doneSlot(slot).done.store(state.epochBase +
                                          static_cast<std::uint64_t>(I + 1),
                                      std::memory_order_release);
      runDynamicChainStageForSlot<I + 1>(slot, state, stages, tok);
    }
  }

  /// Single-thread fallback: run every stage on the producer thread.
  /// Triggered when the pool has at most one participant or the stage pack is
  /// empty. Each stage runs with `slot = 0`, `lo = 0`, `hi = n`. Cancellation
  /// aborts the loop cleanly between stages. Stage exceptions propagate to
  /// the caller unchanged because the inline path has no shared state.
  template <class StagesTuple>
  void runChainInline(std::size_t n, const CancellationToken &tok,
                      StagesTuple &stages) {
    runChainInlineRec<0>(n, tok, stages);
  }

  /// Recursive worker for `runChainInline`. Walks every stage in order
  /// on the producer thread, observing cancellation between stages.
  template <std::size_t I, class StagesTuple>
  void runChainInlineRec(std::size_t n, const CancellationToken &tok,
                         StagesTuple &stages) {
    if constexpr (I < std::tuple_size_v<StagesTuple>) {
      if (tok.stop_requested()) {
        return;
      }
      std::get<I>(stages).fn(I, std::uint32_t{0}, std::size_t{0}, n);
      runChainInlineRec<I + 1>(n, tok, stages);
    }
  }

  /// Variadic chain dispatcher: publish one job, run every stage, rethrow on
  /// first throw. An empty stage pack returns immediately. With
  /// `participants <= 1` every stage runs inline on the producer. Otherwise
  /// a stack-resident `ChainState` is allocated, a wrapper body invokes
  /// `runChainStageForSlot<0>` for the slot's id, the call dispatches a
  /// static-uniform job with `chunk == 1` and `blockCount == participants`,
  /// and the chain's first captured exception (if any) is rethrown after the
  /// join.
  template <class ChainHintsT, class... Stages>
  void parallelChainImpl(std::size_t n, const CancellationToken &tok,
                         Stages &&...stages) {
    parallelChainImplWithRuntimeHints<ChainHintsT>(
        n, /*runtimeHints=*/nullptr, tok, std::forward<Stages>(stages)...);
  }

  /// Internal entry shared by the compile-time-hinted and runtime-hinted
  /// `parallelChain` overloads. `|runtimeHints|` is non-null when the
  /// runtime overload is in use; the compile-time overload passes
  /// `nullptr` and the implementation reads `ChainHintsT` instead.
  template <class ChainHintsT, class... Stages>
  void parallelChainImplWithRuntimeHints(std::size_t n,
                                         const ChainHints *runtimeHints,
                                         const CancellationToken &tok,
                                         Stages &&...stages) {
    constexpr std::size_t kStageCount = sizeof...(Stages);
    if constexpr (kStageCount == 0) {
      return;
    } else {
      auto stagePack =
          std::tuple<std::decay_t<Stages>...>(std::forward<Stages>(stages)...);

      const std::size_t participants = m_control.participants;
      if (participants <= 1 || shouldFallThroughCrossArena()) {
        runChainInline(n, tok, stagePack);
        return;
      }

      // Acquire the dispatch gate BEFORE touching pool-owned scratch
      // (`m_chainDoneSlots`, `m_chainEpochBase`). Concurrent producers would
      // otherwise race on the epoch advance and on the residual stamps left by
      // a peer's previous dispatch. Runtime path threads
      // `runtimeHints->priority` through; compile-time path uses
      // `ChainHintsT::priority`.
      constexpr bool kIsRuntimeHints = std::is_same_v<ChainHintsT, ChainHints>;
      Priority leasePriority = Priority::Throughput;
      if constexpr (kIsRuntimeHints) {
        if (runtimeHints != nullptr) {
          leasePriority = runtimeHints->priority;
        }
      } else {
        leasePriority = ChainHintsT::priority;
      }
      const DispatchLease lease(*this, leasePriority);

      // Reserve a fresh epoch interval in the pool's monotonic counter. The
      // interval covers stamps `[epochBase + 1, epochBase + nStages]`; the next
      // dispatch starts at `epochBase + nStages + 1` so prior-call stamps are
      // strictly below this call's targets. The wrap guard zero-resets and
      // rebases when the counter approaches `UINT64_MAX`; with typical advance
      // rates that branch is unreachable in practice.
      detail::ChainDoneSlot *const slotsBase = m_chainDoneSlots.get();
      const std::uint64_t epochAdvance =
          static_cast<std::uint64_t>(kStageCount) + 1U;
      if (m_chainEpochBase > UINT64_MAX - 1024U) [[unlikely]] {
        for (std::size_t i = 0; i < participants; ++i) {
          slotsBase[i].done.store(0, std::memory_order_relaxed);
        }
        m_chainEpochBase = 0;
      }
      const std::uint64_t epochBase = m_chainEpochBase;
      m_chainEpochBase += epochAdvance;

      detail::ChainState state;
      state.nStages = kStageCount;
      state.n = n;
      state.participants = static_cast<std::uint32_t>(participants);
      state.doneSlots = slotsBase;
      state.epochBase = epochBase;

      using StagePackT = decltype(stagePack);
      constexpr bool kRuntimeHints = std::is_same_v<ChainHintsT, ChainHints>;
      constexpr bool kDynamicRequested = []() consteval {
        if constexpr (kRuntimeHints) {
          return false;
        } else {
          return ChainHintsT::balance == Balance::DynamicChunked &&
                 !ChainHintsT::pipelineSameChunk;
        }
      }();
      constexpr bool kEveryStageCanBeRebalanced =
          allChainStagesAllowDynamicRebalance<StagePackT>();
      constexpr bool kUseDynamicRebalance =
          kDynamicRequested && kEveryStageCanBeRebalanced;

      auto dispatchSlotBody = [&](auto &bodyWrapper) {
        const FunctionRef<void(std::size_t, std::size_t)> body{bodyWrapper};

        detail::JobDescriptor desc;
        desc.first = 0;
        desc.last = participants;
        desc.participants = static_cast<std::uint32_t>(participants);
        desc.balance = Balance::StaticUniform;
        if constexpr (kRuntimeHints) {
          desc.priority = Priority::Throughput;
        } else {
          desc.priority = ChainHintsT::priority;
        }
        desc.body = body;
        // Leave `desc.token` default-constructed: the descriptor's stop-check
        // would gate `runStaticPartition` from admitting per-slot blocks,
        // deadlocking the chain rendezvous. The wrapper observes the
        // user-supplied token at stage boundaries instead.
        desc.chunk = 1;
        desc.blockCount = participants;

        dispatchOneStaticLocked<Balance::StaticUniform>(desc);
      };

      if constexpr (kUseDynamicRebalance) {
        if (n > 0U) {
          std::array<detail::ChainDynamicStageCounter, kStageCount>
              dynamicStageCounters;
          state.dynamicStageCounters = dynamicStageCounters.data();
          std::size_t hintChunk = 0;
          if constexpr (!kRuntimeHints) {
            hintChunk = ChainHintsT::chunk;
          }
          state.dynamicChunk =
              chainDynamicChunkSize(n, participants, hintChunk);
          state.dynamicBlockCount = ceilDiv(n, state.dynamicChunk);
          for (auto &counter : dynamicStageCounters) {
            counter.next.store(0, std::memory_order_relaxed);
          }

          auto bodyWrapper = [&state, &stagePack, &tok,
                              this](std::size_t lo, std::size_t /*hi*/) {
            const auto slot = static_cast<std::uint32_t>(lo);
            runDynamicChainStageForSlot<0>(slot, state, stagePack, tok);
          };
          dispatchSlotBody(bodyWrapper);
        } else {
          auto bodyWrapper = [&state, &stagePack, &tok,
                              this](std::size_t lo, std::size_t /*hi*/) {
            const auto slot = static_cast<std::uint32_t>(lo);
            const auto [slotLo, slotHi] = state.slotRange(slot);
            runChainStageForSlot<0>(slot, slotLo, slotHi, state, stagePack,
                                    tok);
          };
          dispatchSlotBody(bodyWrapper);
        }
      } else {
        auto bodyWrapper = [&state, &stagePack, &tok,
                            this](std::size_t lo, std::size_t /*hi*/) {
          const auto slot = static_cast<std::uint32_t>(lo);
          const auto [slotLo, slotHi] = state.slotRange(slot);
          runChainStageForSlot<0>(slot, slotLo, slotHi, state, stagePack, tok);
        };
        dispatchSlotBody(bodyWrapper);
      }

      // `dispatchOneStatic` rethrows exceptions captured by
      // `desc.firstException`; the body wrapper routes chain-thrown exceptions
      // through `state.firstException` instead.
      rethrowIfCaptured(state);
    }
  }

  /// Rethrow any exception captured into |state|'s `firstException` slot.
  /// The slot is reset to `nullptr` after the rethrow so the caller's stack
  /// frame can unwind without leaking the heap-allocated exception_ptr.
  template <class State>
  [[gnu::always_inline]] static void rethrowIfCaptured(State &state) {
    auto *eptr = state.firstException.load(std::memory_order_acquire);
    if (eptr != nullptr) [[unlikely]] {
      const std::exception_ptr captured = *eptr;
      delete eptr;
      state.firstException.store(nullptr, std::memory_order_release);
      std::rethrow_exception(captured);
    }
  }

  /// Parallel Blelloch scan engine: publish one job, run two passes, return
  /// the inclusive total. The pool-owned `done` slot block is reused (each
  /// dispatch reserves a disjoint epoch interval), and a per-chunk `partials`
  /// vector with cache-line padding keeps adjacent chunks' writes off the
  /// same line. The wrapper body invoked with `(slot, slot+1)` runs the
  /// per-slot two-pass protocol: slot 0 (the producer) drives the sequential
  /// reduce between passes; non-zero slots acquire-spin on
  /// `prefixesPublished` between Pass 1 and Pass 2. The dispatch publishes a
  /// static-uniform job with `blockCount = participants` and `chunk = 1` so
  /// every slot receives exactly one body invocation. Slot 0's body runs
  /// Pass 1, performs the sequential reduce, publishes prefixes, runs
  /// Pass 2, then joins on every other slot's `done >= 2`. Any captured
  /// exception is rethrown after dispatch returns.
  ///
  /// The per-chunk slot type is cache-line aligned so adjacent partials live
  /// on separate lines (false-sharing avoidance over byte-tight packing).
  template <class HintsT, class T, class BodyFn, class PrefixFn>
  T runScanParallel(std::size_t n, T identity, BodyFn &&body, PrefixFn &&prefix,
                    CancellationToken tok, std::size_t participants) {
    // NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
    struct alignas(kCacheLine) ScanSlot {
      T value;
    };
    std::vector<ScanSlot> partials;
    partials.resize(participants);

    // Hierarchical-scan setup. The slot-to-cluster mapping, contiguity
    // check, and `useHierarchical` flag were precomputed once at pool ctor
    // by `initScanScratch` from pool-immutable inputs. Reading the cached
    // fields takes the per-call topology resolution off the hot path.
    const bool useHierarchical = m_scanUseHierarchical;
    const std::uint32_t numClusters = m_scanNumClusters;
    // Cluster scratch. Numbers are tiny (`numClusters <= participants` and
    // typically <= 8) so false sharing across cluster entries is not worth
    // padding away: the cluster-total line bounce IS the cross-cluster
    // reduce traffic, so a few line transfers per call are intentional.
    std::vector<T> clusterTotals;
    std::vector<T> clusterPrefixes;
    // `std::atomic` is non-copyable and non-movable, so
    // `std::vector<std::atomic<T>>` will not compile and `std::array` needs a
    // compile-time size. Hence the unique_ptr-owned C array.
    // NOLINTNEXTLINE(modernize-avoid-c-arrays)
    std::unique_ptr<std::atomic<std::uint64_t>[]> clusterDoneStamps;
    if (useHierarchical) {
      clusterTotals.assign(numClusters, identity);
      clusterPrefixes.assign(numClusters, identity);
      clusterDoneStamps =
          // NOLINTNEXTLINE(modernize-avoid-c-arrays)
          std::make_unique<std::atomic<std::uint64_t>[]>(numClusters);
      for (std::uint32_t k = 0; k < numClusters; ++k) {
        clusterDoneStamps[k].store(0U, std::memory_order_relaxed);
      }
    }

    // Acquire the dispatch gate BEFORE touching pool-owned scratch
    // (`m_chainDoneSlots`, `m_chainEpochBase`). Concurrent producers would
    // otherwise race on the epoch advance and on the residual stamps left by a
    // peer's previous dispatch.
    const DispatchLease lease(*this, Priority::Throughput);

    // Reserve a fresh epoch interval: scan stamps `done = epochBase + 1` after
    // Pass 1 and `done = epochBase + 2` after Pass 2, so the next dispatch
    // starts at `epochBase + 3`. The wrap guard zero-resets and rebases when
    // the counter approaches `UINT64_MAX`.
    detail::ChainDoneSlot *const slotsBase = m_chainDoneSlots.get();
    if (m_chainEpochBase > UINT64_MAX - 1024U) [[unlikely]] {
      for (std::size_t i = 0; i < participants; ++i) {
        slotsBase[i].done.store(0, std::memory_order_relaxed);
      }
      m_chainEpochBase = 0;
    }
    const std::uint64_t epochBase = m_chainEpochBase;
    m_chainEpochBase += 3U;

    detail::ScanState<T> state;
    state.participants = static_cast<std::uint32_t>(participants);
    state.n = n;
    state.doneSlots = slotsBase;
    state.epochBase = epochBase;
    if (useHierarchical) {
      state.clusterIdOfSlot = m_scanClusterIdOfSlot.data();
      state.numClusters = numClusters;
      state.clusterFirstSlot = m_scanClusterFirstSlot.data();
      state.clusterSlotCount = m_scanClusterSlotCount.data();
      state.clusterTotals = clusterTotals.data();
      state.clusterPrefixes = clusterPrefixes.data();
    }

    // CCD-aware asymmetric chunk partitioning. The slot-to-CCD layout, the
    // producer-CCD slot count, and the asymmetric numerator (out of 16)
    // were precomputed once at pool ctor by `initScanScratch` from the
    // pool-immutable inputs (`m_ccdOfSlot`, the coherence probe's
    // `maxCrossOverIntraRatio`). The hot path reads the cached fields
    // directly; this saves the per-call O(participants) sweep over
    // `m_ccdOfSlot` plus the floating-point bias derivation.
    if (m_scanSlotsOnProducerCcd > 0U &&
        m_scanSlotsOnProducerCcd < participants) {
      state.ccdOfSlot = m_ccdOfSlot.data();
      state.producerCcd = m_scanProducerCcd;
      state.slotsOnProducerCcd = m_scanSlotsOnProducerCcd;
      state.asymmetricNum = m_scanAsymmetricNum;
    }

    CancellationToken scanTok = std::move(tok);

    // The inclusive accumulator at the right edge of the scan is computed by
    // slot 0's sequential reduce. Slot 0 IS the producer thread, so writing to
    // a stack-local captured by reference is race-free: no other thread reads
    // `inclusiveTotal` until `dispatchOne` joins.
    T inclusiveTotal{};

    std::atomic<std::uint64_t> *const clusterStampsPtr =
        clusterDoneStamps.get();
    auto bodyWrapper = [&state, &body, &prefix, &identity, &scanTok, &partials,
                        &inclusiveTotal, useHierarchical,
                        clusterStampsPtr](std::size_t lo, std::size_t /*hi*/) {
      const auto slot = static_cast<std::uint32_t>(lo);
      const auto [slotLo, slotHi] = state.slotRange(slot);
      const std::uint64_t pass1Stamp = state.epochBase + 1U;
      const std::uint64_t pass2Stamp = state.epochBase + 2U;
      // Worker-side failure capture: stamp the first exception slot,
      // then arm the cancellation flag so peers short-circuit at the
      // next pass-2 gate. Both stores are release; readers acquire-load
      // matching atomics elsewhere in the body wrapper.
      auto recordFailure = [&state] {
        captureFirstException(state);
        state.scanCancelled.store(1U, std::memory_order_release);
      };

      // ----- Pass 1: compute this chunk's partial sum with `initial =
      // identity`. Output buffer is `nullptr` to signal "Pass 1,
      // do not write" so the body can branch on it when it makes the
      // partial-sum computation cheaper than a full scan write.
      try {
        T partial = body(static_cast<std::size_t>(slot), slotLo, slotHi,
                         identity, static_cast<T *>(nullptr));
        partials[slot].value = std::move(partial);
      } catch (...) {
        recordFailure();
        // Stamp the final epoch so the producer's exit join completes
        // regardless of pass.
        state.doneSlot(slot).done.store(pass2Stamp, std::memory_order_release);
        if (slot == 0U) {
          // Producer must still wait for every other slot before returning to
          // keep dispatchOne's join contract intact. Publish prefixes so any
          // peer spinning on `prefixesPublished` exits its loop.
          state.prefixesPublished.store(1U, std::memory_order_release);
          for (std::uint32_t s = 1; s < state.participants; ++s) {
            while (state.doneSlot(s).done.load(std::memory_order_acquire) <
                   pass2Stamp) {
              detail::cpuRelax();
            }
          }
        }
        return;
      }
      // Release-store: pairs with the producer's acquire-load before computing
      // prefixes.
      state.doneSlot(slot).done.store(pass1Stamp, std::memory_order_release);

      // Hierarchical per-cluster reduce path. Each cluster's leader does
      // its own local seq-reduce over its cluster's slots; cluster
      // leaders then exchange one cluster-total cache line each across
      // the fabric, bounding inter-cluster line transit to one transfer
      // per cluster pair. The producer combines the per-cluster totals
      // into per-cluster exclusive prefixes and publishes them; each
      // cluster leader folds its prefix into its slots' partials before
      // Pass 2 reads them. Triggered when the pool's coherence probe
      // found two or more contiguous clusters and the producer is in
      // cluster 0.
      if (useHierarchical) {
        const std::uint32_t cluster = state.clusterIdOfSlot[slot];
        const std::uint32_t firstSlot = state.clusterFirstSlot[cluster];
        const std::uint32_t slotCount = state.clusterSlotCount[cluster];
        const bool isLeader = (slot == firstSlot);

        if (isLeader) {
          // Wait for cluster's slots' Pass 1.
          for (std::uint32_t s = firstSlot; s < firstSlot + slotCount; ++s) {
            if (s == slot) {
              continue;
            }
            while (state.doneSlot(s).done.load(std::memory_order_acquire) <
                   pass1Stamp) {
              detail::cpuRelax();
            }
          }
          // Local seq-reduce: write per-slot intra-cluster exclusive
          // prefixes back into `partials`, accumulate into `localAcc`.
          T localAcc = identity;
          try {
            for (std::uint32_t s = firstSlot; s < firstSlot + slotCount; ++s) {
              T t = std::move(partials[s].value);
              partials[s].value = localAcc;
              localAcc = prefix(std::move(localAcc), std::move(t));
            }
          } catch (...) {
            recordFailure();
          }
          // Publish cluster total. The store on `clusterTotals[cluster]`
          // happens-before the release on `clusterStampsPtr[cluster]`, so
          // the producer's acquire on the same atomic synchronises the
          // cluster total read.
          state.clusterTotals[cluster] = std::move(localAcc);
          clusterStampsPtr[cluster].store(1U, std::memory_order_release);

          if (slot == 0U) {
            // Producer (cluster 0 leader) waits for every other cluster
            // total, then computes the cross-cluster exclusive prefixes.
            for (std::uint32_t k = 1; k < state.numClusters; ++k) {
              while (clusterStampsPtr[k].load(std::memory_order_acquire) < 1U) {
                detail::cpuRelax();
              }
            }
            T globalAcc = identity;
            try {
              for (std::uint32_t k = 0; k < state.numClusters; ++k) {
                state.clusterPrefixes[k] = globalAcc;
                globalAcc =
                    prefix(std::move(globalAcc), state.clusterTotals[k]);
              }
            } catch (...) {
              recordFailure();
            }
            inclusiveTotal = std::move(globalAcc);
            // Publish prefixes globally so non-producer leaders proceed.
            state.prefixesPublished.store(1U, std::memory_order_release);
          } else {
            // Non-producer cluster leader: wait for global prefix
            // publication (the producer's release-store).
            while (state.prefixesPublished.load(std::memory_order_acquire) ==
                   0U) {
              detail::cpuRelax();
            }
          }

          // Cluster leader (every leader, including producer) combines
          // its cluster's exclusive prefix with each slot's intra-cluster
          // exclusive prefix to produce the per-slot global exclusive
          // prefix that Pass 2's body will read.
          const T cPrefix = state.clusterPrefixes[cluster];
          try {
            for (std::uint32_t s = firstSlot; s < firstSlot + slotCount; ++s) {
              partials[s].value = prefix(cPrefix, std::move(partials[s].value));
            }
          } catch (...) {
            recordFailure();
          }
          // Final per-cluster signal: cluster's slots can read their
          // partials and run Pass 2.
          clusterStampsPtr[cluster].store(2U, std::memory_order_release);
        } else {
          // Non-leader slot in this cluster: wait for the cluster's
          // leader to combine the global prefix into our `partials[slot]`.
          while (clusterStampsPtr[cluster].load(std::memory_order_acquire) <
                 2U) {
            detail::cpuRelax();
          }
        }

        // ----- Pass 2: every slot reads `partials[slot]` (now the
        // global exclusive prefix for this slot's chunk) and runs the
        // body with that prefix as `initial`.
        const bool cancelObserved =
            state.scanCancelled.load(std::memory_order_acquire) != 0U ||
            scanTok.stop_requested();
        if (!cancelObserved) {
          try {
            T myPrefix = partials[slot].value;
            (void)body(static_cast<std::size_t>(slot), slotLo, slotHi,
                       std::move(myPrefix), static_cast<T *>(nullptr));
          } catch (...) {
            recordFailure();
          }
        } else if (scanTok.stop_requested()) {
          state.scanCancelled.store(1U, std::memory_order_release);
        }
        state.doneSlot(slot).done.store(pass2Stamp, std::memory_order_release);
        return;
      }

      if (slot == 0U) {
        // Producer waits for every chunk's Pass-1 partial. A peer that threw
        // stamps the Pass-2 epoch as part of its catch; that satisfies `done >=
        // pass1Stamp` too, so the reduce proceeds against the partial slot's
        // last value (which the post-join rethrow then suppresses).
        for (std::uint32_t s = 1; s < state.participants; ++s) {
          while (state.doneSlot(s).done.load(std::memory_order_acquire) <
                 pass1Stamp) {
            detail::cpuRelax();
          }
        }

        // Skip the seq-reduce when a peer's Pass-1 captured an EXCEPTION: the
        // partials of the failing slot were never written, so feeding them to
        // the user's `prefix` would invoke it on a default-constructed (or
        // moved-from) T. Cancellation alone is NOT sufficient to skip -- Pass 1
        // ran on every slot before any cancellation gate fires (the gates are
        // in Pass 2), so the partials are valid and the producer's
        // `inclusiveTotal` contract still holds. Mark the prefix flag published
        // either way so any worker still spinning on it can short-circuit.
        const bool pass1Failed =
            state.firstException.load(std::memory_order_acquire) != nullptr;
        T acc = identity;
        if (!pass1Failed) {
          // ----- Sequential reduce on the producer: build exclusive prefixes
          // in chunk-id order, stash them back into `partials[c]` so each
          // slot's Pass-2 invocation reads its own seed. A throwing user
          // `prefix` MUST publish the prefixes flag and stamp Pass-2 done
          // before re-raising, otherwise background workers stuck on
          // `prefixesPublished` deadlock the producer's outer join.
          try {
            for (std::uint32_t c = 0; c < state.participants; ++c) {
              T t = std::move(partials[c].value);
              partials[c].value = acc;
              acc = prefix(std::move(acc), std::move(t));
            }
          } catch (...) {
            recordFailure();
          }
        }
        // Publish the prefixes via release: workers' acquire-load on
        // `prefixesPublished` synchronizes with the writes we just performed to
        // `partials[*]` (when the loop ran) or releases peers stuck on the flag
        // (when we short-circuited).
        state.prefixesPublished.store(1U, std::memory_order_release);

        // ----- Pass 2: run slot 0's body with `initial = partials[0]`. Skip
        // the body entirely if we already captured an exception or saw a stop
        // request, but still stamp our `done` and join on the others.
        const bool cancelObserved =
            state.scanCancelled.load(std::memory_order_acquire) != 0U ||
            scanTok.stop_requested();
        if (!cancelObserved) {
          try {
            T myPrefix = partials[0U].value;
            (void)body(std::size_t{0}, slotLo, slotHi, std::move(myPrefix),
                       static_cast<T *>(nullptr));
          } catch (...) {
            recordFailure();
          }
        } else if (scanTok.stop_requested()) {
          state.scanCancelled.store(1U, std::memory_order_release);
        }
        state.doneSlot(0).done.store(pass2Stamp, std::memory_order_release);

        // Stash the inclusive accumulator now, before returning. It is
        // captured by reference in this lambda; the caller reads it after
        // `dispatchOneStaticLocked` returns, which already waits for every
        // worker's mailbox to stamp the dispatch's done sentinel. That
        // mailbox-join provides the per-slot Pass-2 rendezvous, so the
        // producer does not need a second scan over `done` slots here.
        inclusiveTotal = std::move(acc);
      } else {
        // Background worker: spin on `prefixesPublished` until the producer
        // finishes the reduce.
        while (true) {
          if (state.prefixesPublished.load(std::memory_order_acquire) != 0U) {
            break;
          }
          detail::cpuRelax();
        }
        if (state.scanCancelled.load(std::memory_order_acquire) != 0U ||
            scanTok.stop_requested()) {
          if (scanTok.stop_requested()) {
            state.scanCancelled.store(1U, std::memory_order_release);
          }
          state.doneSlot(slot).done.store(pass2Stamp,
                                          std::memory_order_release);
          return;
        }
        // Pass 2: invoke the body with `initial = partials[slot]`. Output
        // buffer is `nullptr` matching Pass 1; the body branches on `initial !=
        // identity` to know it should write.
        try {
          T myPrefix = partials[slot].value;
          (void)body(static_cast<std::size_t>(slot), slotLo, slotHi,
                     std::move(myPrefix), static_cast<T *>(nullptr));
        } catch (...) {
          recordFailure();
          state.doneSlot(slot).done.store(pass2Stamp,
                                          std::memory_order_release);
          return;
        }
        state.doneSlot(slot).done.store(pass2Stamp, std::memory_order_release);
      }
    };

    const FunctionRef<void(std::size_t, std::size_t)> bodyRef{bodyWrapper};

    detail::JobDescriptor desc;
    desc.first = 0;
    desc.last = participants;
    desc.participants = static_cast<std::uint32_t>(participants);
    desc.balance = Balance::StaticUniform;
    desc.priority = Priority::Throughput;
    desc.body = bodyRef;
    // Leave `desc.token` default-constructed: the descriptor's stop-check would
    // gate the static-partition admit from running every slot's body, and slot
    // 0 IS the participant that joins on the others, so a stopped token would
    // deadlock the producer's join. The wrapper observes `scanTok` at pass
    // boundaries instead.
    desc.chunk = 1;
    desc.blockCount = participants;

    dispatchOneStaticLocked<Balance::StaticUniform>(desc);

    // `dispatchOneStaticLocked` rethrows exceptions captured by
    // `desc.firstException`; the wrapper routes scan-thrown exceptions through
    // `state.firstException` instead.
    rethrowIfCaptured(state);

    return inclusiveTotal;
  }

  /// Decoupled-lookback inclusive prefix-sum engine. Single-pass: each
  /// tile reads its slice of `in` once and writes its slice of `out`
  /// once, so the bandwidth floor is 2n bytes (read input + write
  /// output) instead of two-pass Blelloch's 3n.
  ///
  /// Per-tile state machine (Merrill-Garland 2016): every tile cycles
  /// through `Initialized -> AggregateAvailable -> PrefixAvailable`.
  /// Tile T's worker computes its local total, publishes it as
  /// `aggregate`, walks predecessors backward summing aggregates until
  /// it finds a predecessor in `PrefixAvailable`, computes its own
  /// prefix, publishes `prefix`, then runs the inclusive scan into
  /// `out[T_lo..T_hi]` with `prefix` as the seed.
  ///
  /// Cross-CCD adaptation: each tile is owned by a single worker (slot)
  /// and per-tile state lines live with the owner; the lookback chain
  /// sweeps backward across tiles, so workers on cluster N reading a
  /// predecessor tile owned by cluster M pay the cross-cluster
  /// coherence cost. With tiles sized to the runtime-probed L2/2 the
  /// chain typically terminates within a couple of hops because
  /// immediate predecessors finish their aggregate before the
  /// successor's body returns.
  ///
  /// Output prefetch: each tile issues `PREFETCHW` over its own
  /// `out[T_lo..T_hi]` slice immediately after publishing its
  /// aggregate, so the cross-cluster RFO traffic for the writes runs
  /// concurrently with the lookback walk and the local scan, hiding
  /// the inter-die fabric round-trip behind per-tile compute.
  ///
  /// Tile size: `tileBytes = max(64 KiB, l2KibPerCore * 1024 / 2)` --
  /// half the runtime-probed L2 leaves room for both the input read
  /// and the output write of a tile to be L2-resident. Falls back to
  /// 256 KiB when sysfs is absent.
  ///
  /// Returns the inclusive total at the right edge.
  template <class HintsT, class T, class PrefixFn>
  T runInclusiveScanLookback(std::span<const T> in, std::span<T> out,
                             T identity, PrefixFn &&prefix,
                             const CancellationToken &tok) {
    if (in.size() != out.size()) {
      throw std::invalid_argument(
          "inclusiveScan: in.size() must equal out.size()");
    }
    const std::size_t n = in.size();
    if (n == 0U) {
      return identity;
    }
    const std::size_t participants = m_control.participants;

    // Choose tile bytes from the runtime-probed L2-per-core. The tile
    // size balances: (a) tile-local working set should fit in L2 so
    // Pass-1's chunk-local scan stays cache-resident through the
    // lookback wait, (b) tile count >= participants so every worker
    // has work and the lookback chain pipelines (more tiles than
    // workers means a slow tile doesn't stall the whole chain), (c)
    // tile compute time should be a few microseconds so the lookback
    // chain hops overlap with adjacent tiles' Pass-1 work.
    //
    // Heuristic: tile_bytes = min(l2_per_core / 4, n / participants).
    // The L2/4 cap leaves headroom for d.in + d.out + a small cushion
    // of locked stack frames in cache; the n/participants cap ensures
    // there are at least `participants` tiles. Hardware-agnostic:
    // works on any CPU that exposes index2/size in sysfs; falls back
    // to 64 KiB when sysfs is absent.
    // Tile sizing -- balance two competing constraints:
    //   (a) Chain parallelism: in a single coherence cluster, the
    //       lookback chain serializes the workers (tile T waits on
    //       T-1's prefix). If `numTiles > participants`, some workers
    //       grab a second tile and must then wait for the chain to
    //       progress past the first wave, defeating parallelism. So
    //       prefer `numTiles ~= participants_per_cluster` so each
    //       cluster's chain length matches its worker count.
    //   (b) L2 residency: Pass-1 writes d.out chunk-local-scan (one
    //       cache pass) and Pass-2 in-place adds the global prefix
    //       (second pass over the same lines). Each worker's per-tile
    //       working set is `2 * tileBytes` (the tile's d.out lines
    //       are read+written twice). If this exceeds L2, Pass-2's
    //       reads spill to L3, costing bandwidth.
    //
    // Resolve: pick `tileBytes = clamp(perParticipantBytes, kMinTileBytes,
    // l2KibPerCore*1024)`. The L2 cap ensures the tile's working set is
    // L2-resident; the per-participant ratio ensures the chain length
    // equals participant count when `n` is small enough; the
    // `kMinTileBytes` floor avoids pathological 4-tile dispatches with
    // huge per-tile overhead.
    constexpr std::size_t kFallbackL2Bytes = std::size_t{512U} * 1024U;
    constexpr std::size_t kMinTileBytes = std::size_t{64U} * 1024U;
    const std::size_t l2Bytes =
        m_topology.l2KibPerCore == 0U
            ? kFallbackL2Bytes
            : static_cast<std::size_t>(m_topology.l2KibPerCore) * 1024U;
    const std::size_t nBytes = n * sizeof(T);
    const std::size_t perParticipantBytes =
        (nBytes + participants - 1U) / participants;
    std::size_t tileBytes = perParticipantBytes;
    if (tileBytes > l2Bytes) {
      tileBytes = l2Bytes;
    }
    if (tileBytes < kMinTileBytes) {
      tileBytes = kMinTileBytes;
    }
    const std::size_t tileElems = tileBytes / sizeof(T);
    if (tileElems == 0U) {
      // T larger than the tile budget; fall through to a serial scan.
      T running = identity;
      for (std::size_t i = 0; i < n; ++i) {
        running = prefix(std::move(running), in[i]);
        out[i] = running;
      }
      return running;
    }
    const std::size_t numTiles = (n + tileElems - 1U) / tileElems;
    if (participants <= 1U || numTiles == 1U || shouldFallThroughCrossArena()) {
      // Serial path: a single thread does the whole scan.
      if (tok.stop_requested()) {
        return identity;
      }
      T running = identity;
      for (std::size_t i = 0; i < n; ++i) {
        running = prefix(std::move(running), in[i]);
        out[i] = running;
      }
      return running;
    }

    using Tile = detail::LookbackTile<T>;
    std::vector<Tile> tiles(numTiles);
    std::atomic<std::exception_ptr *> firstException{nullptr};

    // Per-cluster tile claim counters. Each cluster's workers atomically
    // claim tiles from their cluster's contiguous tile range. The
    // partition is `tilesPerCluster_k = round(numTiles * slotsInCluster_k
    // / participants)`, with the residual tile (if `numTiles %
    // participants != 0`) assigned to the last cluster. The lookback
    // chain runs intra-cluster except for the single hop at the
    // cluster-0 / cluster-1 boundary, so cross-cluster cache-line
    // transit on the chain is bounded to one line per scan.
    const bool multiCluster = m_scanUseHierarchical && m_scanNumClusters >= 2U;
    const std::uint32_t numClustersUsed = multiCluster ? m_scanNumClusters : 1U;
    std::vector<std::uint64_t> clusterFirstTile(numClustersUsed, 0U);
    std::vector<std::uint64_t> clusterLastTile(numClustersUsed, numTiles);
    if (multiCluster) {
      std::uint64_t cursor = 0;
      for (std::uint32_t k = 0; k < numClustersUsed; ++k) {
        clusterFirstTile[k] = cursor;
        const std::uint64_t kSlots = m_scanClusterSlotCount[k];
        std::uint64_t kTiles =
            (numTiles * kSlots + participants - 1ULL) / participants;
        if (k + 1U == numClustersUsed || cursor + kTiles > numTiles) {
          kTiles = numTiles - cursor;
        }
        cursor += kTiles;
        clusterLastTile[k] = cursor;
      }
    }
    std::vector<std::atomic<std::uint64_t>> clusterNextTile(numClustersUsed);
    for (std::uint32_t k = 0; k < numClustersUsed; ++k) {
      clusterNextTile[k].store(clusterFirstTile[k], std::memory_order_relaxed);
    }

    auto claimAndRunTile = [&](std::uint32_t slot) noexcept(noexcept(
                               prefix(std::declval<T>(), std::declval<T>()))) {
      const std::uint32_t cluster =
          multiCluster ? m_scanClusterIdOfSlot[slot] : 0U;
      const std::uint64_t myLast = clusterLastTile[cluster];
      while (true) {
        const std::uint64_t tIdx =
            clusterNextTile[cluster].fetch_add(1U, std::memory_order_acq_rel);
        if (tIdx >= myLast) {
          return;
        }
        if (firstException.load(std::memory_order_acquire) != nullptr) {
          // A peer threw; mark our tile's prefix as identity and
          // publish so successors don't deadlock on lookback.
          tiles[tIdx].prefix = identity;
          tiles[tIdx].flag.store(
              static_cast<std::uint64_t>(Tile::Flag::PrefixAvailable),
              std::memory_order_release);
          continue;
        }
        try {
          const std::size_t lo = tIdx * tileElems;
          const std::size_t hi = (lo + tileElems > n) ? n : (lo + tileElems);

          // Pass 1: read `in[lo..hi]` once, compute the chunk-local
          // inclusive scan with prefix=identity, write directly to
          // `out[lo..hi]`. The chunk total (= scan's last element)
          // is also published as the tile's `aggregate`. By
          // reusing `out` as the scratch buffer for the
          // local-scan output we avoid a separate temp buffer's
          // allocation and keep the data path L2-resident: Pass 2
          // immediately re-reads the same lines from L2 and
          // overwrites them with the global-prefix-corrected
          // values, so total memory traffic through the cache
          // hierarchy is `n bytes read in + n bytes written out`,
          // matching the 2n bandwidth floor.
          T localTotal = identity;
          for (std::size_t i = lo; i < hi; ++i) {
            localTotal = prefix(std::move(localTotal), in[i]);
            out[i] = localTotal;
          }

          // Publish aggregate. Release-store on the flag pairs with
          // a successor tile's acquire-load in `lookbackWalk`.
          tiles[tIdx].aggregate = localTotal;
          tiles[tIdx].flag.store(
              static_cast<std::uint64_t>(Tile::Flag::AggregateAvailable),
              std::memory_order_release);

          // Compute exclusive prefix for this tile.
          T myPrefix;
          if (tIdx == 0U) {
            myPrefix = identity;
          } else {
            myPrefix = detail::lookbackWalk<T>(tiles.data(),
                                               static_cast<std::uint32_t>(tIdx),
                                               identity, prefix);
          }
          tiles[tIdx].prefix = myPrefix;
          tiles[tIdx].flag.store(
              static_cast<std::uint64_t>(Tile::Flag::PrefixAvailable),
              std::memory_order_release);

          // Pass 2: in-place adjust the chunk-local inclusive scan
          // we wrote in Pass 1 by adding the global prefix. The
          // lines are warm in L1/L2 from Pass 1's write so this is
          // an L2-bandwidth-bound read+write pass with no
          // additional input read from `in`.
          if (tIdx > 0U) {
            for (std::size_t i = lo; i < hi; ++i) {
              out[i] = prefix(myPrefix, std::move(out[i]));
            }
          }
        } catch (...) {
          auto *eptr = new std::exception_ptr(std::current_exception());
          std::exception_ptr *expected = nullptr;
          if (!firstException.compare_exchange_strong(
                  expected, eptr, std::memory_order_release,
                  std::memory_order_acquire)) {
            delete eptr;
          }
          tiles[tIdx].prefix = identity;
          tiles[tIdx].flag.store(
              static_cast<std::uint64_t>(Tile::Flag::PrefixAvailable),
              std::memory_order_release);
        }
      }
    };

    // Dispatch: every slot pulls tiles from the shared atomic counter.
    // Producer participates as slot 0. The dispatch's existing
    // mailbox-join handles the rendezvous after every tile is
    // consumed.
    auto bodyWrapper = [&claimAndRunTile](std::size_t slot,
                                          std::size_t /*hi*/) {
      claimAndRunTile(static_cast<std::uint32_t>(slot));
    };
    const FunctionRef<void(std::size_t, std::size_t)> bodyRef{bodyWrapper};

    const DispatchLease lease(*this, Priority::Throughput);
    detail::JobDescriptor desc;
    desc.first = 0;
    desc.last = participants;
    desc.participants = static_cast<std::uint32_t>(participants);
    desc.balance = Balance::StaticUniform;
    desc.priority = Priority::Throughput;
    desc.body = bodyRef;
    desc.chunk = 1;
    desc.blockCount = participants;
    dispatchOneStaticLocked<Balance::StaticUniform>(desc);

    auto *eptr = firstException.load(std::memory_order_acquire);
    if (eptr != nullptr) [[unlikely]] {
      const std::exception_ptr captured = *eptr;
      delete eptr;
      firstException.store(nullptr, std::memory_order_release);
      std::rethrow_exception(captured);
    }
    if (tok.stop_requested()) {
      return identity;
    }
    // Inclusive total at the right edge: combine the last tile's
    // exclusive prefix with its own aggregate.
    return prefix(tiles[numTiles - 1U].prefix, tiles[numTiles - 1U].aggregate);
  }

  /// Resolve the steal-policy field of |HintsT|, defaulting to
  /// `StealPolicy::ClusterLocal` when the hint type does not declare one.
  /// The detection idiom keeps the `forkJoin` entry compatible with hint
  /// presets that omit the field. The fallback matches `HintsDefaults` so
  /// an absent field behaves identically to the default-constructed hint.
  template <class HintsT>
  static constexpr StealPolicy stealPolicyFromHints() noexcept {
    if constexpr (requires { HintsT::stealPolicy; }) {
      return HintsT::stealPolicy;
    } else {
      return StealPolicy::ClusterLocal;
    }
  }

  /// Wrap each task closure in a `FunctionRef<void()>` and store it in a
  /// stack-resident `detail::Task` descriptor. Used by both `forkJoin` entry
  /// overloads.
  template <class ClosureTuple, std::size_t N, std::size_t... Is>
  static void fillTaskBodies(ClosureTuple &closures,
                             std::array<detail::Task, N> &tasks,
                             std::index_sequence<Is...> /*indices*/) noexcept {
    ((tasks[Is].body = FunctionRef<void()>(std::get<Is>(closures))), ...);
  }

  /// Run |nTasks| fork-join tasks from the producer thread, dispatching to
  /// background workers via the standard generation-publish protocol. Each
  /// task is pushed onto slot 0's deque, the descriptor is published so
  /// background workers wake and enter the drain loop, the producer runs its
  /// own drain loop as slot 0, and the call joins when every worker's
  /// `doneEpoch` reaches the new generation. The first captured exception
  /// (if any) is rethrown after the join.
  template <bool HasToken = true>
  void runForkJoinOuter(detail::Task *tasks, std::size_t nTasks,
                        CancellationToken tok, StealPolicy stealPolicy) {
    const std::size_t participants = m_control.participants;

    // Construct shared state on the producer's stack.
    detail::ForkJoinState state;
    state.participants = static_cast<std::uint32_t>(participants);
    state.ccdOfSlot = m_ccdOfSlot.empty() ? nullptr : m_ccdOfSlot.data();
    state.preferSameCcd = (stealPolicy == StealPolicy::ClusterLocal);
    state.token = std::move(tok);
    state.pendingTasks.store(static_cast<std::int64_t>(nTasks),
                             std::memory_order_relaxed);

    // Single-participant fast path: run every task inline on the producer; no
    // fan-out, no deque traffic, no generation publish. Cheaper than the
    // dispatch round-trip for the canonical n_init = 1 case. The cross-arena
    // guard takes the same path: a worker on a different arena must not block
    // on this arena's queue, so it runs the tasks itself.
    if (participants <= 1 || shouldFallThroughCrossArena()) {
      runForkJoinInline(tasks, nTasks, state);
      rethrowIfCaptured(state);
      return;
    }

    // Acquire the dispatch gate BEFORE writing per-task state and pushing onto
    // the worker deques: Chase-Lev push is single-owner and concurrent producer
    // pushes corrupt the deque, and the descriptor pointer races with workers
    // servicing a peer producer's forkJoin. The lease serializes producers
    // around `m_workerDeques` mutation.
    const DispatchLease lease(*this, Priority::Throughput);

    // Route every root task onto slot 0's (the producer's) deque. The earlier
    // round-robin fan-out across all `participants` deques pessimized fib-class
    // workloads: with only two root tasks, round-robin splits work across a
    // small subset of deques while the first recursive levels are still serial
    // on their owners. Pushing onto slot 0 instead lets stealers converge on a
    // single hot deque on the first probe; recursive forkJoin from inside a
    // worker still goes onto the calling worker's own deque (see
    // runForkJoinNested), so the steal radius shrinks as recursion
    // deepens. This matches TBB's "submit to caller's task pool" shape on the
    // outer call.
    //
    // Pre-grow once when |nTasks| would force multiple geometric grows on
    // the slot-0 deque (initial capacity is 64). Folds up to log2(nTasks)
    // allocations into one for large fan-outs.
    if (nTasks > 64U) {
      m_workerDeques[0]->reserveOwner(nTasks);
    }
    for (std::size_t i = 0; i < nTasks; ++i) {
      tasks[i].state = &state;
      m_workerDeques[0]->push(&tasks[i]);
    }
    (void)participants;

    // Body wrapper that workers run from `runActiveJob`. The job descriptor's
    // `lo` is the worker slot; we ignore `hi` (= lo+1, by construction). The
    // wrapper drains the worker's own deque, then steals from victims, until
    // every task in this state has retired.
    detail::ForkJoinState *statePtr = &state;
    auto bodyWrapper = [this, statePtr](std::size_t lo, std::size_t /*hi*/) {
      this->template runForkJoinDrain<HasToken>(static_cast<std::uint32_t>(lo),
                                                *statePtr);
    };
    const FunctionRef<void(std::size_t, std::size_t)> bodyRef{bodyWrapper};

    detail::JobDescriptor desc;
    desc.first = 0;
    desc.last = participants;
    desc.participants = static_cast<std::uint32_t>(participants);
    desc.balance = Balance::StaticUniform;
    desc.priority = Priority::Throughput;
    desc.body = bodyRef;
    // The descriptor's own `token` is left default-constructed: the
    // cancellation logic flows through `state.token`, observed by
    // `runForkJoinDrain` at task boundaries. A stopped descriptor token would
    // gate the static-partition admit and prevent slot 0's drain from running,
    // which would deadlock the join.
    desc.chunk = 1;
    desc.blockCount = participants;

    // The lease covers both the deque pushes above and the
    // publish/participate/join below. `dispatchOneStaticLocked` installs slot-0
    // TLS context across the producer's inline body so a recursive `forkJoin`
    // call from inside a task body can take the in-place nested-recursion path;
    // nothing additional is needed here.
    dispatchOneStaticLocked<Balance::StaticUniform>(desc);

    rethrowIfCaptured(state);
  }

  /// Recursive `forkJoin` fast path that calls the last child inline by
  /// static type. Pushes the first |deferredCount| `Task` descriptors onto
  /// the calling worker's deque (so peers can steal via the standard victim
  /// probe) and invokes |inlineLast| directly, bypassing the `FunctionRef`
  /// thunk and the per-task `runOneTask` wrapper. Cancellation gating,
  /// exception capture, and the `pendingTasks` rendezvous match
  /// `runOneTask`'s contract; only the indirect call disappears.
  ///
  /// Invariant: `pendingTasks` is initialized to |deferredCount| (the
  /// deque-pushed children only). The inline child runs on the producer
  /// thread and does not contribute to the join counter, since it always
  /// completes before drain begins. For `participants <= 1` the path falls
  /// back to the single-participant inline executor after rebuilding a full
  /// Task array.
  template <bool HasToken = true, class InlineFn>
  void runForkJoinTypedTailNested(
      detail::Task *deferred, std::size_t deferredCount, InlineFn &&inlineLast,
      CancellationToken tok, // NOLINT(performance-unnecessary-value-param)
      StealPolicy stealPolicy, std::uint32_t callerSlot) {
    const std::size_t participants = m_control.participants;

    detail::ForkJoinState state;
    state.participants = static_cast<std::uint32_t>(participants);
    state.ccdOfSlot = m_ccdOfSlot.empty() ? nullptr : m_ccdOfSlot.data();
    state.preferSameCcd = (stealPolicy == StealPolicy::ClusterLocal);
    if constexpr (HasToken) {
      if (tok != CancellationToken{}) {
        state.token = std::move(tok);
      }
    } else {
      (void)tok;
    }
    // Only the deque-pushed children are tracked by the join counter; the
    // inline tail runs synchronously on this thread before drain begins.
    state.pendingTasks.store(static_cast<std::int64_t>(deferredCount),
                             std::memory_order_relaxed);

    if (participants <= 1) {
      // Single-participant fallback: run the deferred tasks serially via the
      // standard wrapper, then call the typed tail directly, with the same
      // execution order as `runForkJoinInline`, no array rebuild required. The
      // tail's cancel gate / exception capture / fetch_sub equivalent is the
      // same as the post-loop tail below.
      state.pendingTasks.store(static_cast<std::int64_t>(deferredCount + 1),
                               std::memory_order_relaxed);
      for (std::size_t i = 0; i < deferredCount; ++i) {
        deferred[i].state = &state;
        runOneTaskImpl<HasToken>(deferred[i], 0);
      }
      if (state.forkJoinCancelled.load(std::memory_order_acquire) == 0U &&
          (!HasToken || !state.token.stop_requested())) {
        try {
          std::forward<InlineFn>(inlineLast)();
        } catch (...) {
          captureFirstException(state);
          state.forkJoinCancelled.store(1U, std::memory_order_release);
        }
      }
      state.pendingTasks.fetch_sub(1, std::memory_order_release);
      rethrowIfCaptured(state);
      return;
    }

    for (std::size_t i = 0; i < deferredCount; ++i) {
      deferred[i].state = &state;
      m_workerDeques[callerSlot]->push(&deferred[i]);
    }

    // Inline the typed tail directly. Replicates `runOneTask`'s cancellation
    // gate and exception capture verbatim, but skips the FunctionRef indirect
    // call and the `pendingTasks.fetch_sub` (the tail is not in the join
    // counter).
    if (state.forkJoinCancelled.load(std::memory_order_acquire) == 0U &&
        (!HasToken || !state.token.stop_requested())) {
      try {
        std::forward<InlineFn>(inlineLast)();
      } catch (...) {
        captureFirstException(state);
        state.forkJoinCancelled.store(1U, std::memory_order_release);
      }
    }

    // Producer-local own-pop counter. Every task popped off our own deque whose
    // `state` matches this scope retires here (single-threaded against this
    // counter), so we elide the atomic `pendingTasks.fetch_sub` per own-pop.
    // The drain combines the atomic (peer-stolen decrements) with this local
    // counter to detect the join condition.
    std::int64_t ownPending = 0;
    this->template runForkJoinDrain<HasToken>(callerSlot, state, &ownPending);

    rethrowIfCaptured(state);
  }

  /// Run |nTasks| fork-join tasks recursively from inside a worker's drain
  /// loop. Called when `forkJoin` is invoked re-entrantly from a thread that
  /// is already a worker of this pool. Pushes the children onto the calling
  /// worker's own deque so they are visible to the standard victim-selection
  /// probe and runs the calling worker's drain loop with a fresh
  /// `detail::ForkJoinState` until the inner pending-task counter reaches
  /// zero. The outer state's drain loop is unaffected; the inner call
  /// borrows the worker for the duration of the recursive frame.
  void runForkJoinNested(detail::Task *tasks, std::size_t nTasks,
                         CancellationToken tok, StealPolicy stealPolicy,
                         std::uint32_t callerSlot) {
    const std::size_t participants = m_control.participants;

    // Per-recursion ForkJoinState init: skip the token move-assign when
    // |tok| is the default-constructed sentinel (m_state.get() == nullptr).
    // The default ForkJoinState::token is also a sentinel by construction,
    // so skipping the assign produces identical observable behavior
    // (stop_requested() returns false on both). Saves one shared_ptr copy +
    // potential refcount RMW per nested call.
    detail::ForkJoinState state;
    state.participants = static_cast<std::uint32_t>(participants);
    state.ccdOfSlot = m_ccdOfSlot.empty() ? nullptr : m_ccdOfSlot.data();
    state.preferSameCcd = (stealPolicy == StealPolicy::ClusterLocal);
    if (tok != CancellationToken{}) {
      state.token = std::move(tok);
    }
    state.pendingTasks.store(static_cast<std::int64_t>(nTasks),
                             std::memory_order_relaxed);

    if (participants <= 1) {
      runForkJoinInline(tasks, nTasks, state);
      rethrowIfCaptured(state);
      return;
    }

    // Cilk-5 "spawn parent" trick: push every task EXCEPT the last so peers can
    // steal them, then run the last task inline before entering the drain. The
    // drain subsequently pops the un-stolen pushed tasks (LIFO order), or
    // steals replacements from peers.
    //
    // Why this is correct: the pushed tasks are visible to peers via
    // Chase-Lev's release-store on bottom; the inline last task runs on this
    // worker concurrently with peer execution of the stolen tasks. The join
    // condition (pendingTasks == 0) is invariant to which slot executes which
    // task, so identity-permuting which one runs inline does not change
    // observed behaviour. The captured-exception path inside `runOneTask` is
    // shared across pushed and inline executions.
    //
    // Why this is faster: a push + LIFO pop pair on the same deque costs two
    // atomic ops on `bottom` (release on push, acquire on pop) plus a
    // memory_order_seq_cst CAS in the last-item race resolution. Inlining the
    // body skips both. Recursive workloads (fib, UTS, cilksort) call this path
    // on every level of recursion; saving the per-call deque traffic compounds
    // with depth. Eliminating one push+pop per recursive call moves the
    // producer's hot loop measurably.
    for (std::size_t i = 0; i + 1 < nTasks; ++i) {
      tasks[i].state = &state;
      m_workerDeques[callerSlot]->push(&tasks[i]);
    }
    tasks[nTasks - 1].state = &state;
    runOneTask(tasks[nTasks - 1], callerSlot);

    runForkJoinDrain(callerSlot, state);

    rethrowIfCaptured(state);
  }

  /// Inline fallback: run every task serially on the calling thread. Used for
  /// the single-participant case (no fan-out is possible) and also as the
  /// slow-path tail of `runForkJoinDrain` when nothing is left to steal but
  /// tasks are still pending. Each body's exception is caught and recorded
  /// into the call's `firstException` slot.
  static void runForkJoinInline(detail::Task *tasks, std::size_t nTasks,
                                detail::ForkJoinState &state) noexcept {
    for (std::size_t i = 0; i < nTasks; ++i) {
      tasks[i].state = &state;
      runOneTask(tasks[i], 0);
    }
  }

  /// Worker-side drain loop used by the producer (slot 0) and every
  /// background participant. Repeatedly pop a task from the worker's own
  /// deque; if empty, steal from another worker; run the popped/stolen task;
  /// loop until |state|'s `pendingTasks` counter reaches zero. The
  /// cancellation flag short-circuits the body and decrements the counter
  /// without running the payload so the join finishes promptly under
  /// cancellation or after a captured exception.
  template <bool HasToken = true>
  void runForkJoinDrain(std::uint32_t slot, detail::ForkJoinState &state,
                        std::int64_t *ownPending = nullptr) noexcept {
    const std::size_t participants = state.participants;
    auto &ownDeque = *m_workerDeques[slot];
    std::uint64_t rng = xorshiftSeed(slot);
    std::uint32_t spinCount = 0;
    // Hot-victim hint: remember the slot we last stole from. When a worker is
    // generating a deep recursion (UTS / cilksort interior nodes), the same
    // victim is likely to publish more children before its sub-tree is fully
    // consumed. Probing the last-good victim first turns a 1/(participants-1)
    // random hit into a near-100 % cache-warm hit; failed probes fall through
    // to the random path so a stale hint costs at most one extra cache-line
    // read.
    auto lastVictim =
        static_cast<std::uint32_t>(participants); // sentinel: invalid

    while (true) {
      const std::int64_t pending =
          state.pendingTasks.load(std::memory_order_acquire);
      // When `ownPending` is provided, the caller (producer) is tracking
      // own-pop retirements in a non-atomic stack-local counter; the join
      // condition combines the atomic (peer decrements) with the local (own
      // decrements). This eliminates the LOCK XADD on every own-pop in the hot
      // recursive fib-class path, where same-thread retirements dominate.
      const std::int64_t effective =
          (ownPending != nullptr) ? (pending - *ownPending) : pending;
      if (effective <= 0) {
        return;
      }

      // 1. Try to pop from the owner's own deque first. The Le 2013 last-item
      // race is settled
      //    by the deque's internal seq_cst CAS.
      if (auto popped = ownDeque.pop()) {
        // Own-state task on the producer's drain: skip the atomic decrement;
        // bump the stack-local `ownPending` counter instead. Foreign-state
        // tasks (nested recursion surfacing in the same deque) still use the
        // atomic path so their parent drain's join condition observes the
        // decrement.
        if (ownPending != nullptr && (*popped)->state == &state) {
          runOneTaskOwn<HasToken>(**popped);
          ++(*ownPending);
        } else {
          runOneTaskImpl<HasToken>(**popped, slot);
        }
        spinCount = 0;
        continue;
      }

      // 2. Hot-victim hint: probe the last successful victim before going
      // random. Cheap when it
      //    fits the recursion pattern; one extra atomic load when stale.
      if (lastVictim < participants && lastVictim != slot) {
        if (auto stolen = m_workerDeques[lastVictim]->steal()) {
          runOneTaskImpl<HasToken>(**stolen, slot);
          spinCount = 0;
          continue;
        }
        // Invalidate the stale hint so we don't keep probing the same
        // dry victim. Random probing below picks a fresh target; on the
        // next successful steal we re-arm `lastVictim`.
        lastVictim = static_cast<std::uint32_t>(participants);
      }

      // 3. Random victim probe. Same-CCD bias when CCD-local affinity is
      // requested.
      auto victimOut =
          static_cast<std::uint32_t>(participants); // unused if nullptr
      detail::Task *stolen = trySteal(slot, state, rng, victimOut);
      if (stolen != nullptr) {
        lastVictim = victimOut;
        runOneTaskImpl<HasToken>(*stolen, slot);
        spinCount = 0;
        continue;
      }

      // 3. Nothing to do right now. Yield in user space for a bounded time
      // before re-checking
      //    the pending counter; the rest of the drain stays off the kernel
      //    scheduler. We do not park here because the join condition is the
      //    pending counter, not a new generation.
      detail::cpuRelax();
      ++spinCount;
      // Cap the busy-spin so the worker eventually backs off the steal probes.
      // The cap is the smallest budget that keeps the steal-success-rate test
      // happy.
      if (spinCount >= 4096U) {
        spinCount = 0;
        // Yield once per cycle to let other ready threads make progress on
        // co-tenant cores.
#if defined(__x86_64__) || defined(_M_X64)
        for (std::uint32_t k = 0; k < 64U; ++k) {
          detail::cpuRelax();
        }
#else
        (void)participants;
#endif
      }
      (void)participants;
    }
  }

  /// Pick a victim slot and call its deque's `steal` entry. Uses an xorshift
  /// RNG seeded from the caller's slot id; when |state| requests CCD-local
  /// affinity, the candidate set is restricted to same-CCD victims first,
  /// falling back to any victim if no same-CCD steal succeeded after a full
  /// rotation. Returns the stolen `Task` pointer or `nullptr` on
  /// contention / empty.
  detail::Task *trySteal(std::uint32_t self, detail::ForkJoinState &state,
                         std::uint64_t &rng,
                         std::uint32_t &victimOut) noexcept {
    const std::uint32_t participants = state.participants;
    if (participants <= 1) {
      return nullptr;
    }

    // Probe ONE random victim per call regardless of CCD preference. The drain
    // loop's outer spinCount budget provides the inter-attempt backoff. Probing
    // all same-CCD victims in a tight inner loop here burned most CPU on the
    // steal path for fib-class workloads: idle workers chasing a small root set
    // repeatedly loaded many empty deques before yielding.
    //
    // Same-CCD bias is preserved as a probability bias rather than an
    // unconditional sweep: 7/8 of probes target a same-CCD victim, 1/8 reach
    // for a cross-CCD victim. The bias keeps CCD-local locality without burning
    // cross-CCD coherence traffic on empty-deque sweeps, and the cross-CCD
    // escape valve still finds work when same-CCD is starved (otherwise workers
    // on a CCD with no live tasks would never steal at all).
    //
    // `victimOut` is filled with the probed slot so the caller can use it as a
    // hot-victim hint on subsequent calls. Written even on failure so the
    // caller can avoid re-probing the same empty deque on the very next
    // iteration.
    if (state.preferSameCcd) {
      const auto roll = static_cast<std::uint32_t>(xorshiftNext(rng) & 0x7U);
      const bool tryCross = (roll == 0U);
      if (!tryCross && !m_sameCcdVictims[self].empty()) {
        const auto &same = m_sameCcdVictims[self];
        const auto sameSize = static_cast<std::uint32_t>(same.size());
        const auto cursor = fastRange32(xorshiftNext(rng), sameSize);
        const auto victim = same[cursor];
        victimOut = victim;
        if (auto stolen = m_workerDeques[victim]->steal()) {
          return *stolen;
        }
        return nullptr;
      }
      const auto &cross = m_crossCcdVictims[self];
      const auto crossSize = static_cast<std::uint32_t>(cross.size());
      if (crossSize > 0U) {
        const auto cursor = fastRange32(xorshiftNext(rng), crossSize);
        const auto victim = cross[cursor];
        victimOut = victim;
        if (auto stolen = m_workerDeques[victim]->steal()) {
          return *stolen;
        }
      }
      return nullptr;
    }

    // No CCD preference: probe one random victim per call.
    {
      const auto &row = m_allVictims[self];
      const auto rowSize = static_cast<std::uint32_t>(row.size());
      const auto cursor = fastRange32(xorshiftNext(rng), rowSize);
      const auto victim = row[cursor];
      victimOut = victim;
      if (auto stolen = m_workerDeques[victim]->steal()) {
        return *stolen;
      }
    }
    return nullptr;
  }

  /// Execute one popped/stolen `detail::Task`. Runs the body in a try-block;
  /// on success decrements the call's `pendingTasks` counter via release so
  /// the producer's spin-loop establishes happens-before with whatever the
  /// body wrote. On exception, records the first throw into the call's
  /// `firstException` slot, sets the cancellation flag, and decrements
  /// unconditionally so the join finishes.
  ///
  /// Producer-local variant: skips `state.pendingTasks.fetch_sub` because
  /// the caller is tracking the decrement in a non-atomic stack-local
  /// counter. Used by the producer's own drain when the popped task belongs
  /// to the producer's own scope; peer-stolen tasks still use
  /// `runOneTaskImpl<HasToken>` to keep the cross-thread atomic.
  template <bool HasToken>
  [[gnu::always_inline]] static void
  runOneTaskOwn(detail::Task &task) noexcept {
    detail::ForkJoinState &state = *task.state;
    if (state.forkJoinCancelled.load(std::memory_order_acquire) != 0U ||
        (HasToken && state.token.stop_requested())) [[unlikely]] {
      return;
    }
    try {
      task.body();
    } catch (...) {
      captureFirstException(state);
      state.forkJoinCancelled.store(1U, std::memory_order_release);
    }
  }

  /// Cross-thread variant of the per-task runner. Decrements
  /// `state.pendingTasks` after the body returns or after a cancellation
  /// short-circuit so the producer's join condition can observe
  /// completion. `HasToken` elides the token poll when the caller routed
  /// through the no-token entry.
  template <bool HasToken>
  static void runOneTaskImpl(detail::Task &task,
                             std::uint32_t /*slot*/) noexcept {
    detail::ForkJoinState &state = *task.state;

    // Honour cancellation by decrementing the counter without running the body
    // so the join can observe `pendingTasks == 0`. The
    // `state.token.stop_requested()` poll is gated at compile time: when the
    // call site routed through the no-token overload the check folds away and
    // the worker pays only the `forkJoinCancelled` acquire-load on the hot
    // path.
    if (state.forkJoinCancelled.load(std::memory_order_acquire) != 0U ||
        (HasToken && state.token.stop_requested())) [[unlikely]] {
      state.pendingTasks.fetch_sub(1, std::memory_order_release);
      return;
    }

    try {
      task.body();
    } catch (...) {
      captureFirstException(state);
      state.forkJoinCancelled.store(1U, std::memory_order_release);
    }
    state.pendingTasks.fetch_sub(1, std::memory_order_release);
  }

  /// Default-token specialisation of `runOneTaskImpl<true>`. Used by the
  /// peer-stolen task path that does not statically know whether the
  /// caller passed a token.
  static void runOneTask(detail::Task &task, std::uint32_t slot) noexcept {
    runOneTaskImpl<true>(task, slot);
  }

  /// Seed an xorshift64 RNG from |slot|. The RNG drives `forkJoin`'s
  /// victim-selection probe order. Seeding from the slot id keeps each
  /// worker's first probe distinct so workers do not pile on the same
  /// victim.
  std::uint64_t xorshiftSeed(std::uint32_t slot) const noexcept {
    // Mix in `this` so two arenas with the same slot id produce different
    // RNG sequences. Without the `this` mix, slot 1 in arena A and slot 1
    // in arena B started their victim probes in lock-step, doubling
    // cache-line contention on the same victims.
    std::uint64_t s =
        0x9E3779B97F4A7C15ULL ^
        (static_cast<std::uint64_t>(slot) * 0xBF58476D1CE4E5B9ULL) ^
        reinterpret_cast<std::uintptr_t>(this);
    if (s == 0) {
      s = 1;
    }
    return s;
  }

  /// Advance an xorshift64 RNG. Marsaglia's xorshift64 sequence: one 64-bit
  /// register, three shifts, no branches.
  static std::uint64_t xorshiftNext(std::uint64_t &state) noexcept {
    std::uint64_t s = state;
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    state = s;
    return s;
  }

  /// Reduce a 64-bit RNG draw to `[0, n)` via multiply-high (Lemire 2018),
  /// replacing `xorshiftNext(rng) % n` in the steal hot path so the
  /// non-power-of-two case avoids `idiv`. Bias is bounded by `1 / 2^32`
  /// per bucket, irrelevant for victim selection. |n| must be non-zero.
  static std::uint32_t fastRange32(std::uint64_t x, std::uint32_t n) noexcept {
    return static_cast<std::uint32_t>(((x >> 32) * n) >> 32);
  }

  /// Common job-publish/join helper used by the reduce paths. Builds a
  /// `JobDescriptor` with the supplied static chunk shape, dispatches via
  /// `dispatchOne`, and returns once every worker has stamped its
  /// `doneEpoch`. The caller reads the partials array after this returns;
  /// the join's acquire-load on `doneEpoch` establishes happens-before for
  /// the partial writes.
  void dispatchReduceJob(std::size_t first, std::size_t last,
                         FunctionRef<void(std::size_t, std::size_t)> body,
                         CancellationToken tok, std::size_t participants,
                         std::size_t chunk, std::size_t nChunks) {
    detail::JobDescriptor desc;
    desc.first = first;
    desc.last = last;
    desc.participants = static_cast<std::uint32_t>(participants);
    desc.balance = Balance::StaticUniform;
    desc.priority = Priority::Throughput;
    desc.preWakeCompletionProbe = true;
    desc.body = body;
    desc.token = std::move(tok);
    desc.chunk = chunk;
    desc.blockCount = nChunks;

    dispatchOneStatic<Balance::StaticUniform>(desc);
  }

  /// Typed reduce dispatcher used by the parallel reducer engine. Reuses
  /// the per-thread shared `JobDescriptor` cache and tracks
  /// field-by-field whether the descriptor matches the previous
  /// dispatch; on a match the worker entry's reuse path skips the
  /// per-rank descriptor read.
  template <class HintsT, class Body>
  void dispatchReduceJobTyped(std::size_t first, std::size_t last, Body &body,
                              CancellationToken tok, std::size_t participants,
                              std::size_t chunk, std::size_t nChunks) {
    constexpr bool kCancellationActive = detail::kCancellationActive<HintsT>;

    detail::JobDescriptor &desc = sharedReduceDesc();
    bool keyMatches = true;
    if (desc.first != first) {
      desc.first = first;
      keyMatches = false;
    }
    if (desc.last != last) {
      desc.last = last;
      keyMatches = false;
    }
    if (desc.participants != participants) {
      desc.participants = static_cast<std::uint32_t>(participants);
      keyMatches = false;
    }
    if (desc.balance != Balance::StaticUniform) {
      desc.balance = Balance::StaticUniform;
      keyMatches = false;
    }
    if (desc.priority != HintsT::priority) {
      desc.priority = HintsT::priority;
      keyMatches = false;
    }
    if (!desc.preWakeCompletionProbe) {
      desc.preWakeCompletionProbe = true;
      keyMatches = false;
    }
    if (desc.chunk != chunk) {
      desc.chunk = chunk;
      keyMatches = false;
    }
    if (desc.blockCount != nChunks) {
      desc.blockCount = nChunks;
      keyMatches = false;
    }
    auto *fnAddr = static_cast<void *>(std::addressof(body));
    if (desc.fnPtr != fnAddr) {
      desc.fnPtr = fnAddr;
      keyMatches = false;
    }
    auto *expectedEntry =
        &detail::typedStaticContiguousWorkerEntry<HintsT, Body>;
    if (desc.workerEntry != expectedEntry) {
      desc.workerEntry = expectedEntry;
      keyMatches = false;
    }
    if (desc.workerStateBase != nullptr) {
      desc.workerStateBase = nullptr;
      keyMatches = false;
    }
    if constexpr (kCancellationActive) {
      if (desc.token != tok) {
        desc.token = std::move(tok);
        keyMatches = false;
      }
    } else {
      (void)tok;
    }

    dispatchOneStaticContiguousTypedSlot0Hinted<HintsT>(
        desc, body, !kCancellationActive && keyMatches);
  }

  /// Acquire the priority gate that serializes concurrent `dispatchOne`
  /// callers. Concurrent producers contending on the same pool pass through
  /// this gate before publishing the next generation. The gate is a small
  /// two-bucket priority lane:
  ///
  /// - `Priority::Latency` callers register their waiting count via
  ///   `m_latencyWaiting` and then acquire `m_dispatchMutex` directly. The
  ///   increment is observed by throughput / background callers, which back
  ///   off until it drops to zero before locking.
  /// - `Priority::Throughput` callers register via `m_throughputWaiting` and
  ///   acquire the mutex after spinning until the latency waiter count is
  ///   zero. This is the default and the hot path when only one producer
  ///   dispatches at a time.
  /// - `Priority::Background` callers wait for both `m_latencyWaiting` and
  ///   `m_throughputWaiting` to drop to zero before locking, AND
  ///   release-and-retry if either counter ticks up while they hold the lock
  ///   but have not yet started the dispatch body. Background is
  ///   best-effort and may starve under sustained higher-priority traffic;
  ///   the contract is "yield, do not preempt".
  ///
  /// The single-producer fast path bypasses the back-off entirely so the hot
  /// dispatch path pays one un-contended atomic increment, one atomic
  /// decrement, and one mutex lock/unlock pair.
  void acquireDispatchGate(Priority priority) noexcept {
    if (priority == Priority::Latency) {
      m_latencyWaiting.fetch_add(1, std::memory_order_acq_rel);
      m_dispatchMutex.lock();
      m_latencyWaiting.fetch_sub(1, std::memory_order_acq_rel);
      return;
    }
    if (priority == Priority::Throughput) {
      // Single-producer fast path: try the lock without registering. If it
      // succeeds and no latency caller has registered, the gate is held with
      // zero atomic increments. Re-checking `m_latencyWaiting` after `try_lock`
      // preserves priority ordering: a latency caller that registered between
      // our load and our `try_lock` wins because `lock()` blocks behind us
      // until we release; the recheck here covers the symmetric race.
      if (m_latencyWaiting.load(std::memory_order_acquire) == 0U &&
          m_dispatchMutex.try_lock()) {
        if (m_latencyWaiting.load(std::memory_order_acquire) == 0U) {
          return;
        }
        m_dispatchMutex.unlock();
      }
      m_throughputWaiting.fetch_add(1, std::memory_order_acq_rel);
      while (true) {
        // Defer to any latency caller that registered itself before us. The
        // release/acquire on the counter pairs with the latency caller's
        // increment; missing a transient bump merely costs one extra spin pass
        // (correctness-neutral).
        while (m_latencyWaiting.load(std::memory_order_acquire) != 0U) {
          std::this_thread::yield();
        }
        m_dispatchMutex.lock();
        if (m_latencyWaiting.load(std::memory_order_acquire) != 0U) {
          // Latency caller registered while we were waiting on the lock;
          // release so its own `lock()` can succeed and retry from the wait
          // loop.
          m_dispatchMutex.unlock();
          std::this_thread::yield();
          continue;
        }
        m_throughputWaiting.fetch_sub(1, std::memory_order_acq_rel);
        return;
      }
    }
    // Background: wait for both higher-priority lanes to drain before
    // contending.
    while (true) {
      while (m_latencyWaiting.load(std::memory_order_acquire) != 0U ||
             m_throughputWaiting.load(std::memory_order_acquire) != 0U) {
        std::this_thread::yield();
      }
      m_dispatchMutex.lock();
      if (m_latencyWaiting.load(std::memory_order_acquire) != 0U ||
          m_throughputWaiting.load(std::memory_order_acquire) != 0U) {
        // A higher-priority caller registered while we were waiting on the
        // lock; release so their own `lock()` can succeed and retry from the
        // wait loop.
        m_dispatchMutex.unlock();
        std::this_thread::yield();
        continue;
      }
      return;
    }
  }

  /// Release the priority gate previously acquired via
  /// `acquireDispatchGate`. Pair with the matching acquire in `dispatchOne`'s
  /// scope; idempotent only when called once per acquire (no double-release
  /// defense; the call site guarantees the pairing).
  void releaseDispatchGate() noexcept { m_dispatchMutex.unlock(); }

  /// Common publish / participate / join helper shared by every primitive.
  ///
  /// 1. Acquire the priority gate so concurrent producers serialize through
  ///    a 2-bucket lane.
  /// 2. Compute the next generation by adding `kPhaseStep` to the current
  ///    value (preserving shutdown / cancel flag bits).
  /// 3. Stamp the generation onto |desc| and publish via release-store on
  ///    `activeJob`.
  /// 4. Release-store the new `generation`; this is the publish edge
  ///    workers acquire.
  /// 5. Bump `futexWord` (relaxed) and broadcast `FUTEX_WAKE_PRIVATE` to
  ///    every parked worker.
  /// 6. Producer participates as slot 0 by running its share of the job
  ///    inline.
  /// 7. Acquire-load each background worker's `doneEpoch` until all reach
  ///    the new generation.
  /// 8. Release-store `nullptr` into `activeJob` so the slot is empty for
  ///    the next dispatch.
  /// 9. Release the priority gate.
  /// 10. Rethrow the captured first-exception if any.
  void dispatchOne(detail::JobDescriptor &desc) {
    if (desc.balance == Balance::StaticUniform) {
      dispatchOneStatic<Balance::StaticUniform>(desc);
      return;
    }
    dispatchOneStatic<Balance::DynamicChunked>(desc);
  }

  // RAII guard that installs slot-0 producer TLS context and restores it on
  // scope exit. Used by `dispatchOneStaticLockedBody` so a throw out of the
  // producer's inline body cannot leave the producer thread's TLS pointing
  // at this pool. The destructor runs even during stack unwind, restoring
  // the prior values.
  class TlsContextScope {
  public:
    /// Saves the current TLS pool context and installs the slot-0
    /// producer view: `|pool|`, `|kind|`, `|arenaIndex|`, `|slot|`. The
    /// destructor restores the saved context, so a throw out of the
    /// producer body never leaks a stale pool tag onto the thread.
    TlsContextScope(ThreadContext &ctx, ThreadPool *pool, PoolKind kind,
                    std::uint32_t arenaIndex, std::size_t slot) noexcept
        : m_ctx(ctx), m_savedInside(ctx.insidePoolWorker) {
      // Save outgoing context unconditionally. Earlier we only saved when
      // m_savedInside was true, which left a non-worker producer's slot tag
      // unwritten when cold-collapse passed slot=bit. The user body's
      // `workerIndex()` then read the stale slot 0 instead of the rank it was
      // running on.
      m_savedSlot = ctx.slot;
      m_savedPool = ctx.pool;
      m_savedKind = ctx.kind;
      m_savedArenaIndex = ctx.arenaIndex;
      m_ctx.slot = slot;
      m_ctx.insidePoolWorker = true;
      m_ctx.pool = pool;
      m_ctx.kind = kind;
      m_ctx.arenaIndex = arenaIndex;
    }
    ~TlsContextScope() {
      m_ctx.slot = m_savedSlot;
      m_ctx.insidePoolWorker = m_savedInside;
      m_ctx.pool = m_savedPool;
      m_ctx.kind = m_savedKind;
      m_ctx.arenaIndex = m_savedArenaIndex;
    }
    TlsContextScope(const TlsContextScope &) = delete;
    TlsContextScope &operator=(const TlsContextScope &) = delete;
    TlsContextScope(TlsContextScope &&) = delete;
    TlsContextScope &operator=(TlsContextScope &&) = delete;

  private:
    /// Reference to the calling thread's `ThreadContext` slot.
    ThreadContext &m_ctx;
    /// Slot id captured at construction; restored by the destructor.
    std::size_t m_savedSlot;
    /// Saved value of `ctx.insidePoolWorker`.
    bool m_savedInside;
    /// Saved value of `ctx.pool`.
    ThreadPool *m_savedPool;
    /// Saved value of `ctx.kind`.
    PoolKind m_savedKind;
    /// Saved value of `ctx.arenaIndex`.
    std::uint32_t m_savedArenaIndex;
  };

  // RAII helper that holds `m_dispatchMutex` across pre-publish scratch
  // mutation. Some primitives (`parallelChain`, `parallelScan`, `forkJoin`)
  // mutate pool-owned structures (`m_chainDoneSlots`, `m_workerDeques`,
  // per-task `state` pointers) before publishing the dispatch generation;
  // without holding the gate while doing so, two concurrent producers race
  // on those structures. Callers acquire a `DispatchLease` first, perform
  // their mutation, then call `dispatchOneStaticLocked`, which skips the
  // redundant gate acquisition.
  class DispatchLease {
  public:
    /// Acquires the dispatch gate on `|pool|` for `|priority|`. Skips the
    /// acquire when the calling thread owns an active `LowLatencyGuard`
    /// on this pool, since the LL contract is single-producer.
    DispatchLease(ThreadPool &pool, Priority priority) noexcept : m_pool(pool) {
      // Hot-spin scope fast path: while a `LowLatencyGuard` is alive AND the
      // current thread is the guard's owner, the contract is single-producer
      // and the dispatch gate is dead overhead. Skip both the acquire and the
      // matching release.
      //
      // A second thread that tries to dispatch concurrently while another
      // thread holds the guard MUST take the mutex, otherwise its publish
      // would race with the owner thread on `activeJob` and per-worker
      // mailboxes. Without the TLS gate, the prior contract was enforced by
      // documentation only; multi-producer benchmark / test code routinely
      // tripped the data race under TSAN.
      if (pool.m_control.hotSpinDepth.load(std::memory_order_acquire) != 0U &&
          lowLatencyOwnerDepthTls() != 0U) {
        m_skipped = true;
        return;
      }
      m_pool.acquireDispatchGate(priority);
    }
    ~DispatchLease() {
      if (!m_skipped) {
        m_pool.releaseDispatchGate();
      }
    }
    /// True when the constructor took the LL fast path and the
    /// destructor will not release the gate. Threaded into
    /// `dispatchOneStaticLockedBody` so the inner body avoids re-probing
    /// the same atomic.
    [[nodiscard]] bool gateSkipped() const noexcept { return m_skipped; }
    DispatchLease(const DispatchLease &) = delete;
    DispatchLease &operator=(const DispatchLease &) = delete;
    DispatchLease(DispatchLease &&) = delete;
    DispatchLease &operator=(DispatchLease &&) = delete;

  private:
    /// Pool the lease holds the gate on.
    ThreadPool &m_pool;
    /// True when the constructor took the LL fast path and skipped the
    /// gate acquire.
    bool m_skipped = false;
  };

  /// Static-balance dispatch entry. Acquires a `DispatchLease` and
  /// forwards into `dispatchOneStaticLockedBody`.
  template <Balance BalanceV>
  void dispatchOneStatic(detail::JobDescriptor &desc) {
    const DispatchLease lease(*this, desc.priority);
    // `gateSkipped()` mirrors the `hotSpinDepth != 0` probe the lease just
    // performed; thread it through so the inner body does not re-load the same
    // atomic on a fresh cache-line touch.
    dispatchOneStaticLockedBody<BalanceV>(desc, lease.gateSkipped());
  }

  /// Typed entry into the static-balance dispatch path: the producer's
  /// slot-0 inline body call bypasses `desc.body`'s `FunctionRef`
  /// indirection by invoking `fn` directly. Workers still see the
  /// descriptor with `desc.body` set, so they go through
  /// `runStaticPartition` unchanged. Only the producer's slot-0 inline call
  /// benefits.
  ///
  /// Caller responsibility: `desc.body` must still be set to a `FunctionRef`
  /// wrapping `fn` so workers can drive their share. This entry assumes
  /// `BalanceV == StaticUniform`.
  template <class HintsT, class FOp>
  [[gnu::always_inline]] void
  dispatchOneStaticTypedSlot0Hinted(detail::JobDescriptor &desc, FOp &fn,
                                    bool reuseHint = false) {
    // Pass the constexpr priority directly; avoids the desc.priority load on
    // the producer's pre-lease path. Under LL (hotSpinDepth != 0) the priority
    // arg is unused by the lease ctor.
    const DispatchLease lease(*this, HintsT::priority);
    dispatchOneStaticLockedBody<Balance::StaticUniform, FOp, HintsT>(
        desc, lease.gateSkipped(), &fn, reuseHint);
  }

  /// Typed entry for the static-contiguous slot-0 path. Drops the
  /// rank-claim CAS and the atomic-tail counter; each rank walks its
  /// `contiguousRankBlockSpan`. Used by the parallel reducer engine.
  template <class HintsT, class FOp>
  [[gnu::always_inline]] void
  dispatchOneStaticContiguousTypedSlot0Hinted(detail::JobDescriptor &desc,
                                              FOp &fn, bool reuseHint = false) {
    const DispatchLease lease(*this, HintsT::priority);
    dispatchOneStaticLockedBody<Balance::StaticUniform, FOp, HintsT, true>(
        desc, lease.gateSkipped(), &fn, reuseHint);
  }

  /// Untyped-priority counterpart of `dispatchOneStaticTypedSlot0Hinted`.
  /// Pulls the priority from `|desc|` rather than a compile-time
  /// `HintsT::priority`.
  template <class FOp>
  void dispatchOneStaticTypedSlot0(detail::JobDescriptor &desc, FOp &fn,
                                   bool reuseHint = false) {
    const DispatchLease lease(*this, desc.priority);
    dispatchOneStaticLockedBody<Balance::StaticUniform>(
        desc, lease.gateSkipped(), &fn, reuseHint);
  }

  /// Typed entry into the dynamic-balance dispatch path: the producer's
  /// slot-0 inline body call bypasses `desc.body`'s `FunctionRef` indirection
  /// by invoking `fn` directly. Sibling of
  /// `dispatchOneStaticTypedSlot0Hinted` for `Balance::DynamicChunked`
  /// callers.
  template <class HintsT, class FOp>
  [[gnu::always_inline]] void
  dispatchOneDynamicTypedSlot0Hinted(detail::JobDescriptor &desc, FOp &fn,
                                     bool reuseHint = false) {
    const DispatchLease lease(*this, HintsT::priority);
    dispatchOneStaticLockedBody<Balance::DynamicChunked, FOp, HintsT>(
        desc, lease.gateSkipped(), &fn, reuseHint);
  }

  /// Same as `dispatchOneStatic` but assumes the caller already holds the
  /// dispatch gate. Call sites that mutate pool-owned scratch (e.g.,
  /// zero-resetting `m_chainDoneSlots` or pushing descriptors onto
  /// `m_workerDeques`) acquire a `DispatchLease` first, perform their
  /// mutation, then invoke this overload to publish/participate/join. The
  /// lease's destructor releases the gate after the call returns (or after
  /// an exception unwinds through it).
  template <Balance BalanceV>
  void dispatchOneStaticLocked(detail::JobDescriptor &desc) {
    // Locked entry points are reached after the caller already mutated
    // pool-owned scratch under its own lease, so the gate state has not been
    // re-probed; recompute the LL flag here.
    const bool lowLatencyActive =
        m_control.hotSpinDepth.load(std::memory_order_acquire) != 0U;
    dispatchOneStaticLockedBody<BalanceV>(desc, lowLatencyActive);
  }

  /// Inner body of the locked dispatch path. Publishes the new
  /// generation, wakes parked workers, runs slot 0 inline, joins on
  /// every worker's `doneEpoch`, and rethrows the captured first
  /// exception (if any). `Slot0Body` and `Slot0Hints` carry typed slot-0
  /// information when the caller passed a typed entry; `ContiguousRank`
  /// selects the static-contiguous worker path.
  template <Balance BalanceV, class Slot0Body = std::nullptr_t,
            class Slot0Hints = void, bool ContiguousRank = false>
  [[gnu::always_inline]] void
  dispatchOneStaticLockedBody(detail::JobDescriptor &desc,
                              bool lowLatencyActive, Slot0Body *slot0 = nullptr,
                              bool reuseHint = false) {
    CITOR_COUNTERS_INC(dispatches);
    const std::uint64_t prev =
        m_control.generation.load(std::memory_order_relaxed);
    const std::uint64_t nextGen = prev + detail::PoolControl::kPhaseStep;
    // Same-command reuse: when the producer's TLS key matched the previous
    // dispatch, the workers' cached job parameters are still valid. We OR
    // kReuseBit into the published mailbox value so workers skip reading desc
    // fields entirely. Producer also skips the mailboxDesc store in this case
    // (the workers won't dereference it).
    //
    // Pool-level invalidation: the producer's `keyMatches` only sees its own
    // TLS desc; it does not observe whether a different thread (different TLS
    // desc) or a different pool (different worker mailboxes) intervened. The
    // workers' caches are primed against `m_lastPublishedDesc`, so reuse is
    // only safe when the producer's `&desc` IS that pointer. The
    // compare-and-store happens here under the dispatch gate, so a relaxed load
    // is sufficient.
    const bool reuseSafe =
        reuseHint &&
        (m_lastPublishedDesc.load(std::memory_order_relaxed) == &desc);
    const std::size_t participantsLocal = m_control.participants;
    // Same-command multi-block reuse means every worker already has a primed
    // typed-runner cache and the dispatch shape has at least one follow-up
    // block beyond the first rank round. In that hot shape, cold-collapse's
    // per-worker `claimedAt` CAS is coherence traffic with no expected save:
    // workers are already polling their mailbox and can run their own ranks.
    // Keep cold-collapse for first publishes, one-block-per-rank fanouts, and
    // non-reused calls where the producer may need to cover a genuinely late
    // worker.
    const bool hotReuseMultiBlock =
        reuseSafe && desc.blockCount > participantsLocal;
    const bool skipColdCollapse = (lowLatencyActive || hotReuseMultiBlock) &&
                                  participantsLocal > 2U &&
                                  desc.workerStateBase != nullptr;
    const std::uint64_t publishedMb =
        nextGen | (reuseSafe ? detail::PoolControl::kReuseBit : 0ULL) |
        (skipColdCollapse ? detail::PoolControl::kSkipClaimBit : 0ULL);
    const std::uint64_t doneSentinelMb =
        publishedMb | detail::PoolControl::kDoneBit;
    // Stash the dispatch's generation onto the descriptor so the cold-collapse
    // CAS-claim path (worker side: `typedStaticUniformWorkerEntry`; producer
    // side: the join-wait fallback below) can use it as the comparator for
    // `WorkerState::claimedAt`. The release-store on each worker's mailbox
    // sequenced-after this relaxed store is the visibility edge: workers
    // acquire-load mailbox and see the matching `desc.generation` write. The
    // historical "desc.generation field is dead" comment no longer holds now
    // that cold-collapse reads it.
    desc.generation = nextGen;

    m_control.activeJob.store(static_cast<void *>(&desc),
                              std::memory_order_release);
    m_control.generation.store(nextGen, std::memory_order_release);

    // Per-worker mailbox publish: write `mailboxDesc` (relaxed; sequenced
    // before the mailbox store by program order), then `mailbox` (release).
    // Both fields share the worker's private cache line, so the worker's
    // acquire-load on `mailbox` picks both up in a single line transit and
    // bypasses the shared `m_control.activeJob` line that previously cost every
    // worker one cache-line read per dispatch.
    {
      auto *workersBaseForPublish = m_workers.get();
      auto *descRaw = static_cast<void *>(&desc);
      // Drain pending cold-stamped acks before any publish (reuse or
      // not). Overwriting a previously cold-collapsed worker's mailbox
      // while it is still parked sends it down the plain-store ack
      // branch on wake, which never sets `kAckedBit`; a later non-reuse
      // publish would then spin here forever. Wake while waiting
      // because the worker we need an ack from may already be parked;
      // a pure cpuRelax spin would not resolve.
      if (m_coldStampedMask != 0U) [[unlikely]] {
        while (m_coldStampedMask != 0U) {
          std::uint64_t scan = m_coldStampedMask;
          std::uint64_t stillPending = 0U;
          while (scan != 0U) {
            const auto bit = static_cast<unsigned>(detail::ctzll(scan));
            scan &= scan - 1U;
            auto *w = workersBaseForPublish + bit;
            if ((w->mailbox.load(std::memory_order_acquire) &
                 detail::PoolControl::kAckedBit) == 0U) {
              stillPending |= std::uint64_t{1} << bit;
            }
          }
          if (stillPending == 0U) {
            m_coldStampedMask = 0U;
            break;
          }
          m_coldStampedMask = stillPending;
          if (!lowLatencyActive) {
            const std::uint32_t nextFutex =
                m_control.futexWord.load(std::memory_order_relaxed) + 1U;
            m_control.futexWord.store(nextFutex, std::memory_order_release);
            (void)detail::futexWakePrivate(&m_control.futexWord, 2);
          }
          detail::cpuRelax();
        }
      }
      if (reuseSafe) {
        // Same-command reuse: workers already cached desc fields on the prior
        // full publish. Skip the mailboxDesc store entirely; just bump mailbox
        // phase with kReuseBit set.
        for (std::size_t slot = 1; slot < participantsLocal; ++slot) {
          auto *w = workersBaseForPublish + slot;
          w->mailbox.store(publishedMb, std::memory_order_release);
        }
      } else {
        for (std::size_t slot = 1; slot < participantsLocal; ++slot) {
          auto *w = workersBaseForPublish + slot;
          if (w->mailboxDesc != descRaw) {
            w->mailboxDesc = descRaw;
          }
          w->mailbox.store(publishedMb, std::memory_order_release);
        }
        // Record the descriptor whose fields the workers' caches are now primed
        // with so the next dispatch's pool-level invalidation check can compare
        // against it.
        m_lastPublishedDesc.store(&desc, std::memory_order_relaxed);
      }
    }

    // Pre-wake hot-completion probe: when the descriptor's primitive is a
    // simple one-shot (parallelFor / parallelReduce / bulkForQueries), every
    // spinning background worker can observe the new mailbox, run a trivial
    // body, and stamp `doneEpoch == nextGen` in a handful of nanoseconds. If
    // all background workers have already stamped done by the time the producer
    // is about to bump `futexWord`, the syscall is unnecessary because there
    // are no parked workers to wake. The probe is bounded to a small number of
    // acquire-load sweeps so a probe miss adds at most a few hundred
    // nanoseconds before falling through to the unconditional wake. Skipping
    // the futex word bump on probe success is safe: a worker that parks between
    // this dispatch and the next will observe a stale `futexWord` value, but
    // the next dispatch's probe fails (parked worker has not stamped the next
    // epoch) and the unconditional wake bumps the word, releasing the parked
    // worker.
    bool preWakeAllDone = false;
    // Worker preserves all flag bits from `publishedMb` when stamping DONE on
    // its mailbox, so the producer's join waits for `publishedMb | kDoneBit`
    // (which already includes kReuseBit when set, matching the worker's stamp).
    const std::uint64_t doneSentinel = doneSentinelMb;
    if (!lowLatencyActive && desc.preWakeCompletionProbe &&
        participantsLocal > 1U) {
      auto *workersBase = m_workers.get();
      constexpr std::uint32_t kPreWakeProbeRounds = 4U;
      std::uint64_t pendingProbe = m_control.pendingMaskBits;
      for (std::uint32_t r = 0; r < kPreWakeProbeRounds; ++r) {
        std::uint64_t scan = pendingProbe;
        while (scan != 0U) {
          const auto bit = static_cast<unsigned>(detail::ctzll(scan));
          scan &= scan - 1U;
          if (((workersBase + bit)->mailbox.load(std::memory_order_acquire) &
               ~detail::PoolControl::kAckedBit) == doneSentinel) {
            pendingProbe &= ~(std::uint64_t{1} << bit);
          }
        }
        if (pendingProbe == 0U) {
          preWakeAllDone = true;
          break;
        }
        detail::cpuRelax();
      }
    }

    if (!lowLatencyActive && !preWakeAllDone) {
      // Hot-cadence skip: workers' spin-then-park budget is bounded by
      // `kSpinAfterBulkJob.maxCycles` of TSC time. If the previous dispatch
      // on this pool published within half that budget ago, no worker can
      // have completed its spin window and parked since then -- all of them
      // are still in the spin loop polling their mailbox lines, and the
      // futex syscall is a guaranteed no-op (kernel finds zero waiters).
      // The half-budget guard keeps a margin against worst-case dispatch
      // latency variance pushing a worker past the parking gate before
      // this branch fires.
      const std::uint64_t nowTsc = detail::readTsc();
      const std::uint64_t prevTsc = m_recentDispatchTsc;
      const std::uint64_t hotWindow =
          detail::kSpinAfterBulkJob.maxCycles / 2ULL;
      const bool hotCadence =
          prevTsc != 0ULL && nowTsc > prevTsc && (nowTsc - prevTsc) < hotWindow;
      if (!hotCadence) {
        // Single-writer load/store on `futexWord`: under `m_dispatchMutex`
        // the dispatching producer is the only writer, so a non-atomic load
        // + release store is sufficient to bump the parking token. Workers
        // acquire `generation` (not `futexWord`) for descriptor visibility;
        // the producer's lifetime contract serializes shutdown against
        // active dispatch.
        const std::uint32_t nextFutex =
            m_control.futexWord.load(std::memory_order_relaxed) + 1U;
        m_control.futexWord.store(nextFutex, std::memory_order_release);
        // Chain-wake-2: producer wakes only the first two parked workers;
        // each woken worker fires futex_wake(N=2) on its post-park branch
        // (see `workerMainLoop`), doubling the chain at logarithmic depth.
        (void)detail::futexWakePrivate(&m_control.futexWord, 2);
      }
      m_recentDispatchTsc = nowTsc;
    }

    // Producer participates as slot 0. Install the slot-0 TLS context once for
    // the inline body so a nested CPO call can detect the same-pool case via
    // the standard `tlsContext()` probe. The RAII guard restores TLS on every
    // exit path including exceptional unwind from the body, so a throwing user
    // closure cannot leave the producer's TLS pointing at this pool.
    {
      const TlsContextScope tlsGuard(tlsContext(), this, m_kind, m_arenaIndex,
                                     /*slot=*/0);
      if constexpr (std::is_same_v<Slot0Body, std::nullptr_t>) {
        // Untyped path: call through `desc.body`'s `FunctionRef` (callers that
        // don't have the body's type at this site, e.g. forkJoin's nested-state
        // dispatch).
        detail::runActiveJobStatic<BalanceV>(desc, 0U);
      } else if constexpr (BalanceV == Balance::StaticUniform) {
        // Typed path: bypass the indirect call by invoking `*slot0` directly.
        // Workers still see `desc.body` (FunctionRef) and run unchanged.
        if constexpr (ContiguousRank) {
          static_assert(!std::is_void_v<Slot0Hints>);
          detail::runContiguousRankPartitionTyped<Slot0Hints>(desc, 0U, *slot0);
        } else if constexpr (std::is_void_v<Slot0Hints>) {
          detail::runStaticPartitionTyped(desc, 0U, *slot0);
        } else {
          detail::runPartitionTypedHinted<BalanceV, Slot0Hints>(desc, 0U,
                                                                *slot0);
        }
      } else {
        // Typed Dynamic slot-0: same monomorphization as the Static fast path,
        // but with the shared atomic counter as the block-claim mechanism.
        // Avoids the FunctionRef indirect call cost on the producer's slot-0
        // body. After dropping Balance::Steal / Balance::Recursive (typed paths
        // only support StaticUniform + DynamicChunked) this is the
        // unconditional typed-Dynamic branch.
        static_assert(
            BalanceV == Balance::DynamicChunked,
            "typed slot-0 body only valid for StaticUniform / DynamicChunked");
        if constexpr (std::is_void_v<Slot0Hints>) {
          detail::runDynamicCounterTyped(desc, 0U, *slot0);
        } else {
          detail::runPartitionTypedHinted<BalanceV, Slot0Hints>(desc, 0U,
                                                                *slot0);
        }
      }
    }

    // Join: sweep every pending background worker's done-epoch each round,
    // count quiet rounds where no slot advanced, and yield to the scheduler
    // after a bounded number of quiet rounds. Yielding matters at
    // j=participants where the producer can displace a pinned worker on the
    // same CPU; spinning-only on a single slot's done line then prevents that
    // worker from ever stamping its epoch.
    //
    // Acquire load on doneEpoch is preserved -- it pairs with the worker's
    // release store and is the visibility edge that lets the producer observe
    // writes the worker performed inside its body.
    //
    // Reuse `participantsLocal` from the pre-wake probe block above; the field
    // is non-atomic and constant for the pool's lifetime, so re-reading it
    // would just be a redundant load.
    const std::size_t participants = participantsLocal;
    if (participants > 1) {
      auto *workersBase = m_workers.get();
      // Yield-only-on-CPU-collision: if a pending worker is pinned to the same
      // CPU the producer is currently running on, pure spinning cannot make
      // progress because only that worker can write its own done-epoch line.
      // Detect via `sched_getcpu` and yield to the scheduler. The probe is
      // gated by quietRounds to amortize the syscall.
      //
      // Cache `sched_getcpu()` once per join. The producer is auto-pinned by
      // the pool ctor for Standalone pools and by `bindProducerSlot()` for
      // explicit-pin call sites; in both shapes the CPU is invariant for the
      // call's lifetime. Pulling the syscall out of the per-64-rounds probe
      // saves ~10-20ns per gated probe under sustained join contention.
#ifdef __linux__
      const int producerCpu = sched_getcpu();
      const std::uint32_t producerCpuU =
          producerCpu < 0 ? UINT32_MAX
                          : static_cast<std::uint32_t>(producerCpu);
#endif
      const auto pendingPinnedToCurrentCpu = [workersBase
#ifdef __linux__
                                              ,
                                              producerCpuU
#endif
      ](std::uint64_t pending) noexcept {
#ifdef __linux__
        if (producerCpuU == UINT32_MAX) {
          return false;
        }
        std::uint64_t scan = pending;
        while (scan != 0U) {
          const auto bit = static_cast<unsigned>(detail::ctzll(scan));
          scan &= scan - 1U;
          if ((workersBase + bit)->cpuId == producerCpuU) {
            return true;
          }
        }
        return false;
#else
        (void)workersBase;
        (void)pending;
        return false;
#endif
      };
      if (participants <= 64U) {
        // Pending bitmask is precomputed at construction on `PoolControl`'s
        // `participants` line; slot 0 already cleared since the producer
        // participates inline. The join spins on each pending slot in turn:
        // parallel observation across slots was the bitmask scan's only
        // theoretical advantage, but for uniform workloads the total wait time
        // equals `max(slot stamp time)` either way, and for the single-worker
        // fanout the inner scan is a single-bit degenerate. Per-slot spin
        // removes the `ctzll/and/cmovne/rol` chain and the redundant outer
        // `pending == before` tracking on every PAUSE round.
        std::uint64_t pending = m_control.pendingMaskBits;
        std::uint32_t quietRounds = 0;
        constexpr std::uint32_t kJoinQuietRoundsBeforeYield = 4096;
        // Tight-first-N spin per slot: after producer publishes, hot workers
        // often stamp DONE before the first PAUSE would help. Probe with a
        // small bounded load+compare window first; if the stamp is not visible,
        // fall back to PAUSE-spin for slower arrivals.
        constexpr std::uint32_t kTightProbeIters = 8U;
        while (pending != 0U) {
          const auto bit = static_cast<unsigned>(detail::ctzll(pending));
          auto &w = *(workersBase + bit);
          bool tightDone = false;
          for (std::uint32_t i = 0; i < kTightProbeIters; ++i) {
            // Mask out `kAckedBit` so a worker's CAS-success stamp (which
            // carries `kAckedBit` on cold-collapse-eligible dispatches) is
            // recognized as `doneSentinel` here.
            if ((w.mailbox.load(std::memory_order_acquire) &
                 ~detail::PoolControl::kAckedBit) == doneSentinel) {
              tightDone = true;
              break;
            }
          }
          // Cold-collapse fallback: when the worker has not stamped within the
          // tight-probe window and the dispatch opted in (parallelFor sets
          // `workerStateBase`), the worker may still be parked or delayed. Race
          // the worker on `claimedAt`: if the producer wins, run the rank's
          // blocks while the worker is still catching up. The CAS itself is one
          // atomic op on a private cache line; on the hot path (tightDone=true)
          // this branch is never entered, so steady-state dispatch is
          // unchanged.
          //
          // Install the stolen-rank TLS context across the body call so a
          // nested same-pool CPO call from inside the user closure detects the
          // same-pool case via `tlsContext()`. Primitives with nested same-pool
          // support take that path; the rest fall through to inline instead of
          // re-entering `dispatchOneStatic` and self-deadlocking on the
          // non-recursive `m_dispatchMutex`. The RAII guard restores TLS on
          // every exit path including exceptional unwind.
          if (!tightDone && !skipColdCollapse &&
              desc.workerStateBase != nullptr &&
              detail::tryClaimRank(w, nextGen)) {
            if constexpr (!std::is_same_v<Slot0Body, std::nullptr_t> &&
                          BalanceV == Balance::StaticUniform) {
              // Static typed path runs rank `bit`'s strided blocks via the
              // monomorphized typed runner; under the cold-collapse race the
              // worker's own entry has not yet started and the strided blocks
              // are still pending.
              const TlsContextScope tlsGuard(tlsContext(), this, m_kind,
                                             m_arenaIndex,
                                             /*slot=*/bit);
              if constexpr (ContiguousRank) {
                static_assert(!std::is_void_v<Slot0Hints>);
                detail::runContiguousRankPartitionTyped<Slot0Hints>(desc, bit,
                                                                    *slot0);
              } else if constexpr (std::is_void_v<Slot0Hints>) {
                detail::runStaticPartitionTyped(desc, bit, *slot0);
              } else {
                detail::runPartitionTypedHinted<BalanceV, Slot0Hints>(desc, bit,
                                                                      *slot0);
              }
            } else if constexpr (BalanceV == Balance::DynamicChunked) {
              // Dynamic path: rank `bit`'s Phase A block is still pending
              // because the worker has not entered its typed entry. Run it
              // inline. Prefer the typed `*slot0` callable when the
              // dispatcher passed one (typed entry); fall back to
              // `desc.body`'s `FunctionRef` for the untyped entry.
              if (bit < desc.blockCount) {
                const TlsContextScope tlsGuard(tlsContext(), this, m_kind,
                                               m_arenaIndex,
                                               /*slot=*/bit);
                if constexpr (!std::is_same_v<Slot0Body, std::nullptr_t> &&
                              !std::is_void_v<Slot0Hints>) {
                  detail::runPartitionTypedHinted<BalanceV, Slot0Hints>(
                      desc, bit, *slot0);
                } else {
                  const std::size_t lo = desc.first + (bit * desc.chunk);
                  const std::size_t hi = std::min(lo + desc.chunk, desc.last);
                  if (lo < hi) {
                    if constexpr (!std::is_same_v<Slot0Body, std::nullptr_t>) {
                      (*slot0)(lo, hi);
                    } else {
                      desc.body(lo, hi);
                    }
                  }
                }
              }
            }
            // The CAS preserves a worker's `kAckedBit` if the worker raced
            // ahead of us (keeps the wait protocol's signal intact). On the
            // reuse-fast-path we never consult the mask, so skip the OR;
            // workers do not read prior-dispatch desc fields on reuse.
            std::uint64_t expectedMb = publishedMb;
            const bool stamped = w.mailbox.compare_exchange_strong(
                expectedMb, doneSentinel, std::memory_order_release,
                std::memory_order_acquire);
            if (stamped && !reuseSafe) {
              m_coldStampedMask |= (1ULL << bit);
            }
            tightDone = true;
          }
          while (!tightDone &&
                 (w.mailbox.load(std::memory_order_acquire) &
                  ~detail::PoolControl::kAckedBit) != doneSentinel) {
            detail::cpuRelax();
            // Quiet-rounds bookkeeping is dead weight under low-latency scope:
            // workers are hot-spinning so the CPU-collision yield is the wrong
            // remedy, and the iteration cap's yield never triggers within the
            // dispatch cadence either.
            if (!lowLatencyActive) {
              ++quietRounds;
              // Probe the CPU-collision predicate every 64 quiet rounds:
              // amortizes the `sched_getcpu` syscall while still yielding
              // promptly when the producer is sharing a CPU with a pending
              // pinned worker. Two independent yield triggers: (a) every 64th
              // quiet round, yield if a pending worker is pinned to the
              // producer's current CPU (collision); (b) after
              // `kJoinQuietRoundsBeforeYield` total quiet rounds, yield
              // unconditionally as the timeout fallback. Either trigger resets
              // the counter.
              const bool collisionYield = (quietRounds & 0x3FU) == 0U &&
                                          pendingPinnedToCurrentCpu(pending);
              const bool timeoutYield =
                  quietRounds >= kJoinQuietRoundsBeforeYield;
              if (collisionYield || timeoutYield) {
                quietRounds = 0;
                std::this_thread::yield();
              }
            }
          }
          pending &= pending - 1U;
          if (!lowLatencyActive) {
            quietRounds = 0;
          }
        }
      } else {
        for (std::size_t slot = 1; slot < participants; ++slot) {
          auto &w = *(workersBase + slot);
          while (true) {
            const std::uint64_t mb = w.mailbox.load(std::memory_order_acquire);
            if ((mb & ~detail::PoolControl::kAckedBit) == doneSentinel) {
              break;
            }
            detail::cpuRelax();
          }
        }
      }
    }
    // The active-job slot is overwritten by the next dispatch's release-store
    // and cleared by shutdown before the shutdown bit is set. Between
    // dispatches it may carry the previous descriptor pointer; workers only
    // consult it after observing the matching `generation`.

    // The dispatch gate is released by the caller's `DispatchLease` (or by
    // the wrapping `dispatchOneStatic`'s own lease) when the scope exits,
    // including on the rethrow path below: the lease's destructor runs as
    // the exception unwinds, so a concurrent producer can begin its own
    // dispatch as soon as the unwinder leaves this frame.

    // Rethrow the first captured exception, if any. Elide the load when the
    // slot-0 callable is statically known to be nothrow_invocable AND the typed
    // worker runner is in use; in that case neither slot-0 nor workers can
    // populate `desc.firstException`, so the load and rethrow path are dead.
    // Untyped dispatch (Slot0Body == nullptr_t) keeps the load because the
    // worker side falls back to the untyped runStaticPartition which uses
    // desc.body whose throw-ness cannot be determined at this site.
    constexpr bool kSlot0Nothrow =
        !std::is_same_v<Slot0Body, std::nullptr_t> &&
        std::is_nothrow_invocable_v<Slot0Body &, std::size_t, std::size_t>;
    if constexpr (!kSlot0Nothrow) {
      rethrowIfCaptured(desc);
    }
  }

  /// Common shutdown sequence shared by `~ThreadPool` and the constructor's
  /// failure path. Idempotent; calling twice is a no-op because the shutdown
  /// bit is already set on the second pass.
  void shutdownAndJoin() noexcept {
    if (m_shutdownComplete) {
      return;
    }
    m_shutdownComplete = true;

    // Clear `activeJob` BEFORE setting the shutdown bit so worker's
    // `shouldExit` (which checks `activeJob == nullptr` once it observes the
    // shutdown bit on `generation`) sees a coherent pair: shutdown bit set +
    // activeJob nullptr means "no more work, exit cleanly". The dispatch hot
    // path no longer clears `activeJob` per-dispatch (the next dispatch
    // overwrites it via release-store), so the slot may carry the last
    // published descriptor pointer until shutdown -- valid only as a value the
    // worker compares against `nullptr`, never dereferenced.
    m_control.activeJob.store(nullptr, std::memory_order_release);
    m_control.generation.fetch_or(detail::PoolControl::kShutdownBit,
                                  std::memory_order_release);
    // Stamp each background worker's mailbox with the shutdown sentinel so
    // spinning workers (which read their own mailbox, not
    // `m_control.generation`) observe shutdown without having to load the
    // shared control line on every spin iteration.
    {
      auto *workersBase = m_workers.get();
      const std::size_t participantsLocal = m_control.participants;
      for (std::size_t slot = 1; slot < participantsLocal; ++slot) {
        auto *w = workersBase + slot;
        const std::uint64_t prev = w->mailbox.load(std::memory_order_relaxed);
        const std::uint64_t flagBits =
            prev & (detail::PoolControl::kPhaseStep - 1);
        const std::uint64_t shutdownMb =
            ((prev + detail::PoolControl::kPhaseStep) &
             ~(detail::PoolControl::kPhaseStep - 1)) |
            flagBits | detail::PoolControl::kShutdownBit;
        w->mailbox.store(shutdownMb, std::memory_order_release);
      }
    }
    // Shutdown is single-writer (the destructor) by lifetime contract -- no
    // concurrent dispatch races shutdown -- so a load + release-store is enough
    // to bump the parking token.
    {
      const std::uint32_t nextFutex =
          m_control.futexWord.load(std::memory_order_relaxed) + 1U;
      m_control.futexWord.store(nextFutex, std::memory_order_release);
    }
    (void)detail::futexWakePrivate(&m_control.futexWord, INT_MAX);

#ifdef __linux__
    for (const pthread_t &th : m_workerThreads) {
      (void)pthread_join(th, nullptr);
    }
    m_workerThreads.clear();
    // Pair the constructor's best-effort mlock so we don't leak locked pages
    // past pool lifetime when the WorkerArrayDeleter then frees the block.
    // munlock failure is non-fatal (mlock may have failed too, in which case
    // there's nothing to undo).
    if (m_workers != nullptr) {
      (void)munlock(m_workers.get(),
                    sizeof(detail::WorkerState) * m_control.participants);
    }
#else
    for (auto &th : m_fallbackThreads) {
      if (th.joinable()) {
        th.join();
      }
    }
    m_fallbackThreads.clear();
#endif
    m_workerSpawnArgs.clear();
  }

  /// Custom deleter that destroys each `WorkerState` in place and frees the
  /// aligned heap block.
  struct WorkerArrayDeleter {
    /// Number of `WorkerState` instances stored in the block; supplied by
    /// the constructor.
    std::size_t count = 0;

    // Destroy and deallocate the worker block previously created via
    // aligned `operator new`. |ptr| may be `nullptr`.
    void operator()(detail::WorkerState *ptr) const noexcept {
      if (ptr == nullptr) {
        return;
      }
      for (std::size_t i = count; i > 0; --i) {
        (ptr + (i - 1))->~WorkerState();
      }
      ::operator delete(static_cast<void *>(ptr), std::align_val_t{kCacheLine});
    }
  };

  /// Custom deleter that destroys each `ChainDoneSlot` in place and frees
  /// the aligned heap block.
  struct ChainDoneSlotDeleter {
    /// Number of slots stored in the block; supplied by the constructor.
    std::size_t count = 0;

    // Destroy and deallocate the slot block previously created via aligned
    // `operator new`. |ptr| may be `nullptr`.
    void operator()(detail::ChainDoneSlot *ptr) const noexcept {
      if (ptr == nullptr) {
        return;
      }
      for (std::size_t i = count; i > 0; --i) {
        (ptr + (i - 1))->~ChainDoneSlot();
      }
      ::operator delete(static_cast<void *>(ptr), std::align_val_t{kCacheLine});
    }
  };

  /// Deleter for the pool-owned `PlexDoneSlot` block; mirrors
  /// `ChainDoneSlotDeleter`.
  struct PlexDoneSlotDeleter {
    /// Number of slots the deleter must destroy in reverse order before
    /// freeing the cache-line-aligned backing allocation.
    std::size_t count = 0;
    void operator()(detail::PlexDoneSlot *ptr) const noexcept {
      if (ptr == nullptr) {
        return;
      }
      for (std::size_t i = count; i > 0; --i) {
        (ptr + (i - 1))->~PlexDoneSlot();
      }
      ::operator delete(static_cast<void *>(ptr), std::align_val_t{kCacheLine});
    }
  };

  /// Worker-affinity policy chosen at construction. Drives the pool ctor's
  /// `pthread_attr_setaffinity_np` mask -- single-CPU mask for `PerCpu`,
  /// CCD's full CPU set for `PerCluster`, no pin for `None`. Declared here
  /// so its initializer-list position matches the ctor's leading-field
  /// initializer.
  Affinity m_workerAffinity = Affinity::PerCpu;

  /// Origin tag: distinguishes user-owned pools from `PoolGroup`-owned
  /// arenas.
  PoolKind m_kind = PoolKind::Standalone;

  /// Zero-based arena index inside the owning `PoolGroup`; `0` for
  /// `Standalone` pools.
  std::uint32_t m_arenaIndex = 0;

  /// Pool topology snapshot; immutable after construction.
  detail::Topology m_topology;

  /// Shared control block; cache-line aligned for false-sharing avoidance.
  detail::PoolControl m_control{};

  /// Diagnostic counters incremented at the dispatch / inline-fallback /
  /// park / wake / dynamic counter / cancellation sites. Read via
  /// `snapshotCounters()`. Lives on its own cache line so the relaxed RMWs
  /// never bounce with `m_control`'s contended atomics.
  detail::PoolCounters m_counters{};

  /// Owning pointer to the aligned worker-state block; the deleter destroys
  /// each element.
  std::unique_ptr<detail::WorkerState, WorkerArrayDeleter> m_workers;

  /// Pool-owned per-worker `done` slot block reused by every `parallelChain`
  /// and `parallelScan` dispatch. Allocated once at construction time, sized
  /// `participants()`. Each call reserves a fresh interval in
  /// `m_chainEpochBase` and stamps absolute targets during the dispatch; the
  /// block is never zero-reset between calls because the per-call interval
  /// is disjoint from every prior call's stamps. Reusing the block keeps the
  /// hot dispatch path off `operator new` / `operator delete`.
  std::unique_ptr<detail::ChainDoneSlot, ChainDoneSlotDeleter> m_chainDoneSlots;

  /// Pool-owned per-worker `done` slot block reused by every `runPlex`
  /// dispatch. Allocated once at construction time, sized `participants()`.
  /// Each plex call reserves a fresh interval in `m_plexEpochBase` and
  /// stamps absolute targets; the block is never zero-reset between calls.
  std::unique_ptr<detail::PlexDoneSlot, PlexDoneSlotDeleter> m_plexDoneSlots;

  /// Monotonic epoch counter for plex done-slot stamps; advanced under the
  /// dispatch gate by `nPhases + 1` per call so successive calls reserve
  /// disjoint intervals.
  std::uint64_t m_plexEpochBase = 0;

  /// Monotonically-advancing epoch counter for chain and scan done-slot
  /// stamps. Each `parallelChain` / `parallelScan` call captures the
  /// current value as its `state.epochBase` under the dispatch gate, then
  /// advances by `nStages + 1` (chain) or `3` (scan) so successive calls
  /// reserve disjoint intervals. Workers stamp `done = epochBase +
  /// relative_target` and peers wait on `done >= epochBase + target`;
  /// prior-dispatch stamps cannot satisfy a current wait because they are
  /// strictly less than the new base. Plain integral because every mutation
  /// happens under the dispatch gate that already serializes chain / scan
  /// dispatches.
  std::uint64_t m_chainEpochBase = 0;

  /// TSC of the most recent dispatch's publish window, written by
  /// `dispatchOneStaticLockedBody` under `m_dispatchMutex` (single-writer).
  /// Used as a hot-cadence predicate to skip the `futex_wake` syscall when
  /// the previous dispatch was within a worker's spin-budget ago: in that
  /// window no worker can have parked, so the kernel transit would find
  /// zero waiters. Plain integral because every mutation happens under the
  /// dispatch gate. Read by the same writer path in immediate succession,
  /// so torn-read concerns do not apply.
  std::uint64_t m_recentDispatchTsc = 0;

  /// Per-worker Chase-Lev work-stealing deques. One deque per participant;
  /// allocated at construction time so the `forkJoin` steal probe never
  /// pays an allocator round-trip on its hot path. Each deque is owner-only
  /// on its matching slot's worker thread (the producer for slot 0); any
  /// thread may invoke `detail::ChaseLevDeque::steal` on a victim deque.
  std::vector<std::unique_ptr<detail::ChaseLevDeque<detail::Task *>>>
      m_workerDeques;

  /// Per-slot CCD index, cached for fast lookup by `forkJoin`'s
  /// victim-selection probe. Sized `participants()`; populated at
  /// construction time from each worker's `ccdId`. The victim-selection RNG
  /// reads this array (not `WorkerState`) so the steal probe stays off the
  /// worker's hot identity line.
  std::vector<std::uint32_t> m_ccdOfSlot;

  /// One-time coherence ping-pong probe result, run at pool construction on
  /// multi-CCD topologies. Primitives that benefit from CCD-aware
  /// partitioning (currently `parallelScan`) read
  /// `m_coherenceProbe.crossOverIntraRatio` to derive their cross-CCD work
  /// share without any hardware-specific constants in the engine. Default
  /// value (`valid == false`, ratio == 1.0) on single-CCD pools and on
  /// arenas owned by a `PoolGroup`.
  detail::CoherenceProbe m_coherenceProbe;

  /// Pre-resolved `parallelScan` topology, computed once at pool ctor (after
  /// the coherence probe lands) from invariant pool inputs (`m_ccdOfSlot`,
  /// `m_workers[i].cpuId`, `m_coherenceProbe.matrix.cpus`,
  /// `m_coherenceProbe.clusters.clusterIdOfCpuIndex`,
  /// `m_coherenceProbe.maxCrossOverIntraRatio`). The scan path reads these
  /// fields directly instead of recomputing the slot-to-cluster mapping +
  /// contiguity check + asymmetric-bias derivation on every call.
  ///
  /// `m_scanClusterIdOfSlot[s]` is the cluster id for slot `s` (per the
  /// probe's clustering or zero when the probe is invalid).
  /// `m_scanClusterFirstSlot[k]` and `m_scanClusterSlotCount[k]` are the
  /// contiguous slot range belonging to cluster `k`; only valid when
  /// `m_scanUseHierarchical` is true. `m_scanAsymmetricNum` is the
  /// producer-CCD volume fraction (out of 16) used by `slotRange`'s
  /// asymmetric path.
  std::vector<std::uint32_t> m_scanClusterIdOfSlot;
  /// First slot in each cluster's contiguous range. Only meaningful when
  /// `m_scanUseHierarchical` is true.
  std::vector<std::uint32_t> m_scanClusterFirstSlot;
  /// Slot count for each cluster's contiguous range. Companion to
  /// `m_scanClusterFirstSlot`.
  std::vector<std::uint32_t> m_scanClusterSlotCount;
  /// Number of distinct clusters discovered by the coherence probe; zero
  /// when the hierarchical path is disabled.
  std::uint32_t m_scanNumClusters = 0;
  /// True when the scan engine is allowed to take the hierarchical
  /// per-cluster reduce path. Set when the probe found at least two
  /// non-empty contiguous clusters and slot 0 lives in cluster 0.
  bool m_scanUseHierarchical = false;
  /// Producer-CCD volume fraction (out of 16) used by `slotRange`'s
  /// asymmetric partition. Defaults to half.
  std::uint32_t m_scanAsymmetricNum = 8;
  /// CCD index that owns slot 0; `UINT32_MAX` until set.
  std::uint32_t m_scanProducerCcd = UINT32_MAX;
  /// Number of slots whose `ccdOfSlot` equals `m_scanProducerCcd`.
  std::uint32_t m_scanSlotsOnProducerCcd = 0;

  /// Cached background-worker CPU ids for `bindProducerSlot`'s worker-CPU
  /// exclusion logic. Populated once at construction so the producer-side
  /// affinity guard never allocates.
  std::vector<std::uint32_t> m_workerCpus;

  /// Precomputed same-CCD victim list for each slot. Indexed by self-slot;
  /// each row contains every other slot whose `ccdOfSlot` matches. The
  /// fork-join steal probe iterates this list directly when CCD-local
  /// affinity is requested, skipping the per-step `% participants` modulo
  /// and the per-step CCD comparison.
  std::vector<std::vector<std::uint32_t>> m_sameCcdVictims;

  /// Precomputed cross-CCD victim list for each slot (used as the CCD-local
  /// fallback ring). Indexed by self-slot; each row contains every other
  /// slot whose `ccdOfSlot` differs from the self slot's CCD. Pairs with
  /// `m_sameCcdVictims` so a CCD-local steal probe scans same-CCD victims,
  /// then falls back to cross-CCD victims without re-probing the same-CCD
  /// set.
  std::vector<std::vector<std::uint32_t>> m_crossCcdVictims;

  /// Precomputed all-victim list for each slot (used when CCD affinity is
  /// not requested). Indexed by self-slot; each row contains every other
  /// slot in monotonic order. The steal probe wraps around this row using
  /// a precomputed start index instead of `(start + step) % participants`.
  std::vector<std::vector<std::uint32_t>> m_allVictims;

#ifdef __linux__
  /// Background pthreads (Linux). Sized `participants() - 1`.
  std::vector<pthread_t> m_workerThreads;
#else
  /// Background `std::thread` instances (non-Linux fallback). Sized
  /// `participants() - 1`.
  std::vector<std::thread> m_fallbackThreads;
#endif

  /// Persistent storage for the per-worker spawn arguments handed to the
  /// pthread entry function.
  std::vector<WorkerSpawnArg> m_workerSpawnArgs;

  /// Count of workers that have completed startup (affinity bind + TLS +
  /// ready to enter `workerMainLoop`). The constructor waits on this so
  /// the first dispatch never races worker startup; without the barrier,
  /// first-dispatch latency includes the slowest worker's
  /// time-to-reach-loop.
  alignas(kCacheLine) std::atomic<std::uint32_t> m_workersReady{0};

  /// Whether `shutdownAndJoin` has already run; protects against
  /// double-join in the dtor.
  bool m_shutdownComplete = false;

  /// In-flight detached-task counter incremented by `submitDetached` and
  /// decremented by the trampoline. The destructor blocks until this
  /// reaches zero.
  std::atomic<std::uint64_t> m_detachedInFlight{0};

  /// Mutex serializing the detached-throw slot, the dtor's wait predicate,
  /// and the spawn rollback path. Also used by `lastDetachedException` for
  /// race-free reads.
  mutable std::mutex m_detachedMutex;

  /// Condition variable signalled when an in-flight detached task
  /// decrements the counter. The destructor's wait predicate observes the
  /// counter going to zero.
  std::condition_variable m_detachedDone;

  /// Latched on the first detached-task throw; subsequent throws drop.
  /// Read out via `lastDetachedException`; read takes `m_detachedMutex`
  /// for race freedom.
  std::exception_ptr m_detachedException;

  /// Mutex serializing concurrent `dispatchOne` callers through the
  /// priority gate. Held only for the duration of a single primitive's
  /// publish/participate/join cycle so concurrent producers from different
  /// threads do not interleave dispatches against the same worker pool.
  /// Single-producer call sites pay one un-contended `lock`/`unlock` pair
  /// on the dispatch hot path; the contention path is rare.
  std::mutex m_dispatchMutex;

  /// Pointer to the descriptor whose fields the workers' TLS caches are
  /// currently primed with. Updated under `m_dispatchMutex` on every full
  /// (non-reuse) publish.
  ///
  /// The same-command reuse fast path must only fire when the workers'
  /// cached job parameters match the producer's TLS descriptor. The
  /// producer's `keyMatches` check (field-by-field equality against its
  /// own thread-local `desc`) does not see whether a different thread or a
  /// different pool intervened between two of this producer's calls. A
  /// second pointer compare against this pool-level "last full publish"
  /// pointer closes that window: reuse is only valid when the producer's
  /// descriptor IS the one the workers most recently cached. Mutated only
  /// under the dispatch gate, so a relaxed atomic load is sufficient on
  /// the producer's pre-lease read.
  std::atomic<const detail::JobDescriptor *> m_lastPublishedDesc{nullptr};

  /// Bitmask of worker slots whose mailbox the producer self-stamped on a
  /// prior !reuseSafe cold-collapse, drained by the next dispatch's publish
  /// path before reusing this stack frame's `JobDescriptor` storage.
  std::uint64_t m_coldStampedMask{0};

  /// Number of `Priority::Latency` callers currently waiting on the
  /// dispatch gate. Throughput and Background callers spin on this until
  /// it drops to zero before acquiring `m_dispatchMutex`, giving latency
  /// callers first access to the lock.
  std::atomic<std::uint32_t> m_latencyWaiting{0};

  /// Number of `Priority::Throughput` callers currently waiting on the
  /// dispatch gate. Background callers wait on both this and
  /// `m_latencyWaiting` to drain before locking, so any concurrent
  /// throughput producer reaches the workers first. Background may be
  /// reordered behind any number of higher-priority dispatches.
  std::atomic<std::uint32_t> m_throughputWaiting{0};
};

} // namespace citor
