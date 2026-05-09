#pragma once

#include <thread_pool/thread_pool.h>

#include <BS_thread_pool.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <task_thread_pool.hpp>
#include <utility>
#include <vector>

#include "citor/hints.h"
#include "citor/thread_pool.h"

#include "riften/thiefpool.hpp"

#ifdef CITOR_BENCH_HAS_TBB
#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/parallel_reduce.h>
#include <oneapi/tbb/parallel_scan.h>
#include <oneapi/tbb/task_arena.h>
#endif

#ifdef CITOR_BENCH_HAS_TASKFLOW
#include <taskflow/taskflow.hpp>
#endif

#ifdef CITOR_BENCH_HAS_EIGEN_THREADPOOL
// Eigen's NonBlockingThreadPool ships under `unsupported/Eigen/CXX11`. Pulling
// the umbrella `<unsupported/Eigen/CXX11/ThreadPool>` header is the upstream
// supported entry point; it defines `Eigen::ThreadPool` (typedef for the
// non-blocking pool) plus `Eigen::Barrier`.
#include <unsupported/Eigen/CXX11/ThreadPool>
#endif

#ifdef CITOR_BENCH_HAS_OPENMP
#include <omp.h>

#include <atomic>
#endif

#ifdef CITOR_BENCH_HAS_LEOPARD
#include <Leopard/ThreadPool.h>
#endif

#ifdef CITOR_BENCH_HAS_DISPENSO
#include <dispenso/parallel_for.h>
#include <dispenso/thread_pool.h>
#endif

namespace citor::bench {

/// Adapter shim that lets every competitor pool accept the same closure.
///
/// Each competitor's API differs (closure type, return type, blocking
/// semantics). The bench harness drives all of them through one entry point
/// by specializing this trait. The two required hooks are:
///
///   - `make(participants)`: construct the competitor with |participants|
///     workers including any producer-thread participation. Returns a
///     `std::unique_ptr` so the bench owns the lifetime.
///   - `submitBlocksAndWait(pool, first, last, fn)`: dispatch a closure over
///     `[first, last)` such that `fn(blockFirst, blockAfterLast)` is invoked
///     once per block, blocking until all blocks complete.
///
/// The trait does not impose a chunking policy because each competitor handles
/// range partitioning differently; the empty-fan-out workload submits the full
/// range as a single block, which is sufficient to measure dispatch latency.
///
/// Shim primitive contract: `parallelFor`, `parallelReduce`, and `parallelChain`
/// shims early-return when `participantCount == 0` (`parallelReduce` returns
/// |identity|, `parallelChain` is a no-op) for the BS::light_thread_pool
/// specialization. Callers must pass at least 1. The empty-range guard
/// (`last <= first`) is uniform across the three primitives. Other future-pool
/// adapters (dp / task / riften) historically clamp `participantCount==0` to
/// `blocks=1`; bench callers always pass a positive count, so the two paths
/// agree in practice.
template <class Pool> struct CompetitorTraits;

/// Trait for the new `citor::ThreadPool`.
///
/// Routes through citor's public `DynamicHints` preset so the bench measures the user-facing
/// default the engine ships with (DynamicChunked balance, engine-derived chunk size,
/// cold-collapse on). Cells that want a side-by-side Static-vs-Dynamic comparison call
/// `pool.parallelFor<H>` directly with `citor::StaticHints` / `citor::DynamicHints` and bypass
/// this shim.
template <> struct CompetitorTraits<citor::ThreadPool> {
  /// Display name used in the bench table's rightmost column.
  static constexpr const char *name = "citor::ThreadPool";

  /// Construct the new pool with |participants| total participants. citor counts the producer
  /// as slot 0, so the pool spawns `participants - 1` background pthreads.
  static auto make(std::size_t participants) {
    return std::make_unique<citor::ThreadPool>(participants);
  }

  template <class Fn>
  static void submitBlocksAndWait(citor::ThreadPool &pool, std::size_t first, std::size_t last,
                                  Fn fn) {
    pool.parallelFor<citor::DynamicHints>(first, last, fn);
  }

  template <class Fn>
  static void parallelFor(citor::ThreadPool &pool, std::size_t first, std::size_t last,
                          std::size_t /*participantCount*/, Fn fn) {
    pool.parallelFor<citor::DynamicHints>(first, last, fn);
  }
};

/// Trait for `BS::light_thread_pool` (BS::thread_pool with no flags).
///
/// The bench submits one block via `submit_blocks` with `num_blocks` set to 1
/// and waits on the returned `multi_future`. The single-block invocation
/// isolates dispatch latency from any per-block range partitioning the BS pool
/// would otherwise apply.
template <> struct CompetitorTraits<BS::light_thread_pool> {
  /// Display name used in the bench table's rightmost column.
  static constexpr const char *name = "BS::thread_pool";

  /// Construct a `BS::light_thread_pool` with |participants| workers.
  ///
  /// BS interprets the count as "background workers"; the producer parks during
  /// `wait()`. To match `citor::ThreadPool` semantics in the
  /// bench (which counts the producer as a participant) the trait passes the
  /// full count to BS so the pool has the same total worker capacity.
  ///
  /// participants Number of background workers to spawn.
  /// Owning pointer to the constructed pool.
  static auto make(std::size_t participants) {
    return std::make_unique<BS::light_thread_pool>(participants);
  }

  /// Submit `last - first` blocks of size 1 and wait for all of them.
  ///
  /// Per-block dispatch with one body invocation per element matches the shape
  /// citor's `parallelFor<citor::DynamicHints>` produces (auto-derived chunk, blockCount =
  /// range size), so the comparison measures the same logical work for every
  /// pool: every block of every range is a separate scheduling decision rather
  /// than one task running on one worker.
  template <class Fn>
  static void submitBlocksAndWait(BS::light_thread_pool &pool, std::size_t first, std::size_t last,
                                  Fn fn) {
    const std::size_t blocks = last > first ? (last - first) : std::size_t{0};
    if (blocks == 0U) {
      return;
    }
    pool.submit_blocks(first, last, fn, blocks).wait();
  }

  /// Range-partitioned for using `submit_blocks` with `participantCount` blocks
  /// (one block per worker), matching BS's natural single-fanout shape.
  template <class Fn>
  static void parallelFor(BS::light_thread_pool &pool, std::size_t first, std::size_t last,
                          std::size_t participantCount, Fn fn) {
    if (last <= first || participantCount == 0U) {
      return;
    }
    const std::size_t blocks = participantCount;
    pool.submit_blocks(first, last, fn, blocks).wait();
  }

  /// Reduction emulated as N back-to-back `submit_blocks` waves with per-block
  /// partials merged serially after the future barrier resolves. The shape is
  /// honest: BS has no first-class reduce primitive, so the bench measures
  /// dispatch + barrier-wait per block, not a decentralized reduce tree.
  ///
  /// |participantCount| is the caller-supplied block count: BS exposes
  /// `get_thread_count()` but the shim takes the participant count uniformly
  /// across all four future-pool adapters so call sites do not need
  /// per-pool branching to derive it.
  template <class T, class Map, class Combine>
  static T parallelReduce(BS::light_thread_pool &pool, std::size_t first, std::size_t last,
                          std::size_t participantCount, T identity, Map map, Combine combine) {
    if (last <= first || participantCount == 0U) {
      return identity;
    }
    const std::size_t blocks = participantCount;
    std::vector<T> partials(blocks, identity);
    // Per-block fetch_add gives each block a unique partials slot without
    // inferring the index from `bf`. BS partitions with floor-division plus
    // remainder distribution, so the previous ceiling-division reverse-mapping
    // collided indices when `(last - first) % blocks != 0`.
    std::atomic<std::size_t> nextIdx{0};
    pool.submit_blocks(
            first, last,
            [&partials, &map, identity, &nextIdx](std::size_t bf, std::size_t bl) {
              const std::size_t idx = nextIdx.fetch_add(1U, std::memory_order_relaxed);
              partials[idx] = map(bf, bl, identity);
            },
            blocks)
        .wait();
    T result = identity;
    for (auto &p : partials) {
      result = combine(result, p);
    }
    return result;
  }

  /// Scan: serial fallback. BS has no scan primitive; emulating with N waves
  /// over the future-mutex path would not converge to a sub-millisecond
  /// per-stage cost the bench could measure honestly.
  template <class T, class Body>
  static T parallelScan(BS::light_thread_pool &pool, std::size_t first, std::size_t last,
                        T identity, Body body) {
    (void)pool;
    return body(first, last, identity);
  }

  /// Fan-out: single closure dispatched via `submit_task`; `multi_future::wait`
  /// is the rendezvous.
  template <class Fn> static void fanout(BS::light_thread_pool &pool, Fn fn) {
    pool.submit_task(std::move(fn)).wait();
  }

  /// Chain emulated as |stageCount| back-to-back `submit_blocks` waves. Each
  /// stage pays the future-barrier mutex/cv path; BS has no shared rendezvous
  /// chain primitive.
  ///
  /// |participantCount| is the caller-supplied block count, threaded
  /// uniformly across BS / dp / task / riften so call sites can use the same
  /// signature regardless of which future-pool adapter is the row's subject.
  template <class Fn>
  static void parallelChain(BS::light_thread_pool &pool, std::size_t first, std::size_t last,
                            std::size_t participantCount, std::size_t stageCount, Fn fn) {
    if (participantCount == 0U) {
      return;
    }
    const std::size_t blocks = participantCount;
    for (std::size_t stage = 0; stage < stageCount; ++stage) {
      pool.submit_blocks(
              first, last, [stage, &fn](std::size_t bf, std::size_t bl) { fn(stage, bf, bl); },
              blocks)
          .wait();
    }
  }
};

/// Trait for `dp::thread_pool` (DeveloperPaul123/thread-pool).
///
/// `dp::thread_pool` has no fan-out primitive; the bench uses `enqueue` (which
/// returns a `std::future<void>`) to submit a single closure invoking the body
/// over the full range, then waits on the future. This isolates the pool's
/// dispatch latency in the same shape as `submit_blocks(num_blocks=1)`.
template <> struct CompetitorTraits<dp::thread_pool<>> {
  /// Display name used in the bench table's rightmost column.
  static constexpr const char *name = "dp::thread_pool";

  /// Construct a `dp::thread_pool` with |participants| workers.
  ///
  /// `dp::thread_pool` accepts an `unsigned int` worker count; the producer
  /// parks on the future returned from `enqueue` so the count represents the
  /// number of background workers.
  ///
  /// participants Number of workers to spawn.
  /// Owning pointer to the constructed pool.
  static auto make(std::size_t participants) {
    return std::make_unique<dp::thread_pool<>>(static_cast<unsigned int>(participants));
  }

  /// Submit a closure spanning `[first, last)` and wait for it.
  ///
  /// The lambda captures |first| and |last| by value so the future can be
  /// waited on after the trait returns; |fn| is moved in.
  ///
  /// pool  Pool the closure is submitted to.
  /// first Inclusive lower bound of the range.
  /// last  Exclusive upper bound of the range.
  /// fn    Callable invoked with the block's range.
  template <class Fn>
  static void submitBlocksAndWait(dp::thread_pool<> &pool, std::size_t first, std::size_t last,
                                  Fn fn) {
    // Per-block dispatch with one body invocation per element so the bench
    // measures actual N-way fan-out and not a single-task short-circuit. Each
    // enqueue returns a future; the loop waits on all of them.
    if (last <= first) {
      return;
    }
    std::vector<std::future<void>> futures;
    futures.reserve(last - first);
    for (std::size_t i = first; i < last; ++i) {
      futures.emplace_back(pool.enqueue([i, &fn]() { fn(i, i + 1U); }));
    }
    for (auto &f : futures) {
      f.get();
    }
  }

  /// Range-partitioned for: split `[first, last)` into |participantCount|
  /// blocks, enqueue each as a future, wait on all of them. The future-mutex
  /// path is the rendezvous bound; per-stage costs are not a decentralized
  /// done-epoch scan.
  template <class Fn>
  static void parallelFor(dp::thread_pool<> &pool, std::size_t first, std::size_t last,
                          std::size_t participantCount, Fn fn) {
    if (last <= first || participantCount == 0U) {
      return;
    }
    const std::size_t span = last - first;
    const std::size_t blocks = participantCount;
    const std::size_t block = (span + blocks - 1U) / blocks;
    std::vector<std::future<void>> futures;
    futures.reserve(blocks);
    for (std::size_t b = 0; b < blocks; ++b) {
      const std::size_t bf = first + std::min(span, b * block);
      const std::size_t bl = first + std::min(span, (b + 1) * block);
      if (bf == bl) {
        continue;
      }
      futures.emplace_back(pool.enqueue([bf, bl, &fn]() { fn(bf, bl); }));
    }
    for (auto &f : futures) {
      f.get();
    }
  }

  /// Reduction emulated as N enqueue + per-block partials + serial merge.
  /// dp has no reduce primitive; the bench measures fan-out + future-wait.
  ///
  /// |participantCount| is the caller-supplied block count, threaded
  /// uniformly across BS / dp / task / riften so call sites can use the same
  /// signature regardless of which future-pool adapter is the row's subject.
  template <class T, class Map, class Combine>
  static T parallelReduce(dp::thread_pool<> &pool, std::size_t first, std::size_t last,
                          std::size_t participantCount, T identity, Map map, Combine combine) {
    const std::size_t blocks = participantCount == 0U ? std::size_t{1} : participantCount;
    std::vector<T> partials(blocks, identity);
    const std::size_t span = last - first;
    const std::size_t block = (span + blocks - 1U) / blocks;
    std::vector<std::future<void>> futures;
    futures.reserve(blocks);
    for (std::size_t b = 0; b < blocks; ++b) {
      const std::size_t bf = first + std::min(span, b * block);
      const std::size_t bl = first + std::min(span, (b + 1) * block);
      if (bf == bl) {
        continue;
      }
      futures.emplace_back(pool.enqueue(
          [bf, bl, b, &partials, &map, identity]() { partials[b] = map(bf, bl, identity); }));
    }
    for (auto &f : futures) {
      f.get();
    }
    T result = identity;
    for (auto &p : partials) {
      result = combine(result, p);
    }
    return result;
  }

  /// Scan: serial fallback; dp has no scan primitive.
  template <class T, class Body>
  static T parallelScan(dp::thread_pool<> &pool, std::size_t first, std::size_t last, T identity,
                        Body body) {
    (void)pool;
    return body(first, last, identity);
  }

  /// Fan-out: enqueue one closure, wait on its future.
  template <class Fn> static void fanout(dp::thread_pool<> &pool, Fn fn) {
    pool.enqueue(std::move(fn)).get();
  }

  /// Chain emulated as |stageCount| back-to-back parallelFor waves. Each
  /// stage pays the future-mutex barrier; dp has no shared rendezvous chain.
  ///
  /// |participantCount| is the caller-supplied block count, threaded
  /// uniformly across BS / dp / task / riften so call sites can use the same
  /// signature regardless of which future-pool adapter is the row's subject.
  template <class Fn>
  static void parallelChain(dp::thread_pool<> &pool, std::size_t first, std::size_t last,
                            std::size_t participantCount, std::size_t stageCount, Fn fn) {
    const std::size_t blocks = participantCount == 0U ? std::size_t{1} : participantCount;
    for (std::size_t stage = 0; stage < stageCount; ++stage) {
      parallelFor(pool, first, last, blocks,
                  [stage, &fn](std::size_t bf, std::size_t bl) { fn(stage, bf, bl); });
    }
  }
};

/// Trait for `task_thread_pool::task_thread_pool`.
///
/// The pool's `submit` returns `std::future<R>`; the bench uses it to dispatch
/// a single closure and wait. `wait_for_tasks` exists but does not return
/// exceptions per-task; the future-based shape gives consistent error
/// surfacing.
template <> struct CompetitorTraits<::task_thread_pool::task_thread_pool> {
  /// Display name used in the bench table's rightmost column.
  static constexpr const char *name = "task_thread_pool";

  /// Construct a `task_thread_pool` with |participants| workers.
  ///
  /// participants Worker count.
  /// Owning pointer to the constructed pool.
  static auto make(std::size_t participants) {
    return std::make_unique<::task_thread_pool::task_thread_pool>(
        static_cast<unsigned int>(participants));
  }

  /// Submit a closure spanning `[first, last)` and wait for it.
  ///
  /// pool  Pool the closure is submitted to.
  /// first Inclusive lower bound of the range.
  /// last  Exclusive upper bound of the range.
  /// fn    Callable invoked with the block's range.
  template <class Fn>
  static void submitBlocksAndWait(::task_thread_pool::task_thread_pool &pool, std::size_t first,
                                  std::size_t last, Fn fn) {
    // Per-block dispatch matching every other adapter's shape.
    if (last <= first) {
      return;
    }
    std::vector<std::future<void>> futures;
    futures.reserve(last - first);
    for (std::size_t i = first; i < last; ++i) {
      futures.emplace_back(pool.submit([i, &fn]() { fn(i, i + 1U); }));
    }
    for (auto &f : futures) {
      f.get();
    }
  }

  /// Range-partitioned for. One block per worker; future-mutex barrier is the rendezvous.
  template <class Fn>
  static void parallelFor(::task_thread_pool::task_thread_pool &pool, std::size_t first,
                          std::size_t last, std::size_t participantCount, Fn fn) {
    if (last <= first || participantCount == 0U) {
      return;
    }
    const std::size_t span = last - first;
    const std::size_t blocks = participantCount;
    const std::size_t block = (span + blocks - 1U) / blocks;
    std::vector<std::future<void>> futures;
    futures.reserve(blocks);
    for (std::size_t b = 0; b < blocks; ++b) {
      const std::size_t bf = first + std::min(span, b * block);
      const std::size_t bl = first + std::min(span, (b + 1) * block);
      if (bf == bl) {
        continue;
      }
      futures.emplace_back(pool.submit([bf, bl, &fn]() { fn(bf, bl); }));
    }
    for (auto &f : futures) {
      f.get();
    }
  }

  /// Reduction emulated as N submit + per-block partials + serial merge.
  ///
  /// |participantCount| is the caller-supplied block count, threaded
  /// uniformly across BS / dp / task / riften so call sites can use the same
  /// signature regardless of which future-pool adapter is the row's subject.
  template <class T, class Map, class Combine>
  static T parallelReduce(::task_thread_pool::task_thread_pool &pool, std::size_t first,
                          std::size_t last, std::size_t participantCount, T identity, Map map,
                          Combine combine) {
    const std::size_t blocks = participantCount == 0U ? std::size_t{1} : participantCount;
    std::vector<T> partials(blocks, identity);
    const std::size_t span = last - first;
    const std::size_t block = (span + blocks - 1U) / blocks;
    std::vector<std::future<void>> futures;
    futures.reserve(blocks);
    for (std::size_t b = 0; b < blocks; ++b) {
      const std::size_t bf = first + std::min(span, b * block);
      const std::size_t bl = first + std::min(span, (b + 1) * block);
      if (bf == bl) {
        continue;
      }
      futures.emplace_back(pool.submit(
          [bf, bl, b, &partials, &map, identity]() { partials[b] = map(bf, bl, identity); }));
    }
    for (auto &f : futures) {
      f.get();
    }
    T result = identity;
    for (auto &p : partials) {
      result = combine(result, p);
    }
    return result;
  }

  /// Scan: serial fallback; task_thread_pool has no scan primitive.
  template <class T, class Body>
  static T parallelScan(::task_thread_pool::task_thread_pool &pool, std::size_t first,
                        std::size_t last, T identity, Body body) {
    (void)pool;
    return body(first, last, identity);
  }

  /// Fan-out: submit one closure, wait on its future.
  template <class Fn> static void fanout(::task_thread_pool::task_thread_pool &pool, Fn fn) {
    pool.submit(std::move(fn)).get();
  }

  /// Chain emulated as |stageCount| back-to-back parallelFor waves.
  ///
  /// |participantCount| is the caller-supplied block count, threaded
  /// uniformly across BS / dp / task / riften so call sites can use the same
  /// signature regardless of which future-pool adapter is the row's subject.
  template <class Fn>
  static void parallelChain(::task_thread_pool::task_thread_pool &pool, std::size_t first,
                            std::size_t last, std::size_t participantCount, std::size_t stageCount,
                            Fn fn) {
    const std::size_t blocks = participantCount == 0U ? std::size_t{1} : participantCount;
    for (std::size_t stage = 0; stage < stageCount; ++stage) {
      parallelFor(pool, first, last, blocks,
                  [stage, &fn](std::size_t bf, std::size_t bl) { fn(stage, bf, bl); });
    }
  }
};

/// Trait for `riften::Thiefpool` (ConorWilliams/Threadpool).
///
/// `Thiefpool` uses a `riften::Deque` backbone with work-stealing; `enqueue`
/// returns `std::future<R>` so the bench follows the same single-closure +
/// wait pattern as `dp::thread_pool` and `task_thread_pool`.
template <> struct CompetitorTraits<riften::Thiefpool> {
  /// Display name used in the bench table's rightmost column.
  static constexpr const char *name = "riften::Thiefpool";

  /// Construct a `riften::Thiefpool` with |participants| workers.
  ///
  /// participants Worker count.
  /// Owning pointer to the constructed pool.
  static auto make(std::size_t participants) {
    return std::make_unique<riften::Thiefpool>(participants);
  }

  /// Submit a closure spanning `[first, last)` and wait for it.
  ///
  /// pool  Pool the closure is submitted to.
  /// first Inclusive lower bound of the range.
  /// last  Exclusive upper bound of the range.
  /// fn    Callable invoked with the block's range.
  template <class Fn>
  static void submitBlocksAndWait(riften::Thiefpool &pool, std::size_t first, std::size_t last,
                                  Fn fn) {
    // Per-block dispatch matching every other adapter's shape.
    if (last <= first) {
      return;
    }
    std::vector<std::future<void>> futures;
    futures.reserve(last - first);
    for (std::size_t i = first; i < last; ++i) {
      futures.emplace_back(pool.enqueue([i, &fn]() { fn(i, i + 1U); }));
    }
    for (auto &f : futures) {
      f.get();
    }
  }

  /// Range-partitioned for. Riften has no public worker-count query; the
  /// caller supplies |participantCount| (it is the same value the bench used
  /// to construct the pool via `make()`). The future-mutex barrier is the
  /// rendezvous bound; per-stage costs are not a decentralized done-epoch
  /// scan, so this adapter is honestly slower than the same workload through
  /// citor's first-class `parallelFor`.
  template <class Fn>
  static void parallelFor(riften::Thiefpool &pool, std::size_t first, std::size_t last,
                          std::size_t participantCount, Fn fn) {
    if (last <= first || participantCount == 0U) {
      return;
    }
    const std::size_t span = last - first;
    const std::size_t blocks = participantCount;
    const std::size_t block = (span + blocks - 1U) / blocks;
    std::vector<std::future<void>> futures;
    futures.reserve(blocks);
    for (std::size_t b = 0; b < blocks; ++b) {
      const std::size_t bf = first + std::min(span, b * block);
      const std::size_t bl = first + std::min(span, (b + 1) * block);
      if (bf == bl) {
        continue;
      }
      futures.emplace_back(pool.enqueue([bf, bl, &fn]() { fn(bf, bl); }));
    }
    for (auto &f : futures) {
      f.get();
    }
  }

  /// Reduction emulated as N enqueue + per-block partials + serial merge. The
  /// caller supplies the participant count for the partition (riften has no
  /// public worker-count query; see `parallelFor`).
  template <class T, class Map, class Combine>
  static T parallelReduce(riften::Thiefpool &pool, std::size_t first, std::size_t last,
                          std::size_t participantCount, T identity, Map map, Combine combine) {
    const std::size_t blocks = participantCount == 0U ? std::size_t{1} : participantCount;
    std::vector<T> partials(blocks, identity);
    const std::size_t span = last - first;
    const std::size_t block = (span + blocks - 1U) / blocks;
    std::vector<std::future<void>> futures;
    futures.reserve(blocks);
    for (std::size_t b = 0; b < blocks; ++b) {
      const std::size_t bf = first + std::min(span, b * block);
      const std::size_t bl = first + std::min(span, (b + 1) * block);
      if (bf == bl) {
        continue;
      }
      futures.emplace_back(pool.enqueue(
          [bf, bl, b, &partials, &map, identity]() { partials[b] = map(bf, bl, identity); }));
    }
    for (auto &f : futures) {
      f.get();
    }
    T result = identity;
    for (auto &p : partials) {
      result = combine(result, p);
    }
    return result;
  }

  /// Scan: serial fallback; riften has no scan primitive.
  template <class T, class Body>
  static T parallelScan(riften::Thiefpool &pool, std::size_t first, std::size_t last, T identity,
                        Body body) {
    (void)pool;
    return body(first, last, identity);
  }

  /// Fan-out: enqueue one closure, wait on its future.
  template <class Fn> static void fanout(riften::Thiefpool &pool, Fn fn) {
    pool.enqueue(std::move(fn)).get();
  }

  /// Chain emulated as |stageCount| back-to-back parallelFor waves. Each
  /// stage pays the future-mutex barrier; riften has no shared rendezvous
  /// chain primitive.
  template <class Fn>
  static void parallelChain(riften::Thiefpool &pool, std::size_t first, std::size_t last,
                            std::size_t participantCount, std::size_t stageCount, Fn fn) {
    for (std::size_t stage = 0; stage < stageCount; ++stage) {
      parallelFor(pool, first, last, participantCount,
                  [stage, &fn](std::size_t bf, std::size_t bl) { fn(stage, bf, bl); });
    }
  }
};

#ifdef CITOR_BENCH_HAS_TBB
/// Trait for oneTBB driving work through a `tbb::task_arena`.
///
/// `task_arena` lets us pin oneTBB to an explicit worker count (matching the
/// bench's |participants| parameter) without consulting the global control,
/// which avoids polluting other workloads' TBB state across rows. The arena
/// dispatches every primitive via `arena.execute([&]{ ... })` so the body runs
/// on the arena's workers; outside the arena, oneTBB primitives would run on
/// whichever global slots happen to be free.
///
/// Primitive mapping (each shimmed via `arena.execute`):
///   - `submitBlocksAndWait` -> `tbb::parallel_for` with one-block range.
///   - `parallelFor`         -> `tbb::parallel_for` + `tbb::blocked_range`.
///   - `parallelReduce`      -> `tbb::parallel_reduce` + `tbb::blocked_range`.
///   - `parallelScan`        -> `tbb::parallel_scan` + `tbb::blocked_range`.
///   - `fanout`              -> `arena.execute` of a closure already shaped
///     for the arena.
///   - `parallelChain`       -> emulated as a back-to-back sequence of
///     `tbb::parallel_for` waves; oneTBB does not ship a first-class chain
///     primitive.
template <> struct CompetitorTraits<::tbb::task_arena> {
  /// Display name used in the bench table's rightmost column.
  static constexpr const char *name = "oneTBB";

  /// Construct a `task_arena` sized to |participants|.
  ///
  /// `tbb::task_arena` accepts a max-concurrency parameter and a reserved-for-master
  /// count; the bench reserves one slot for the producer thread, mirroring how
  /// `citor::ThreadPool` treats slot 0 as the producer.
  ///
  /// participants Total participant count, including the producer.
  /// Owning pointer to the constructed arena.
  static auto make(std::size_t participants) {
    return std::make_unique<::tbb::task_arena>(static_cast<int>(participants), /*reserved=*/1);
  }

  /// Per-block fan-out, used by `empty_fanout_bench.cpp` style workloads. The
  /// grainsize is set to 1 plus the `simple_partitioner` so TBB does split the
  /// range into N tasks, mirroring our pool's per-slot partition; without this
  /// TBB's auto-partitioner collapses small ranges to a single inline block on
  /// the producer thread and reports a measurement that bypasses real fan-out.
  template <class Fn>
  static void submitBlocksAndWait(::tbb::task_arena &arena, std::size_t first, std::size_t last,
                                  Fn fn) {
    arena.execute([&] {
      ::tbb::parallel_for(
          ::tbb::blocked_range<std::size_t>{first, last, /*grainsize=*/1},
          [&](const ::tbb::blocked_range<std::size_t> &r) { fn(r.begin(), r.end()); },
          ::tbb::simple_partitioner{});
    });
  }

  /// Range-partitioned `parallel_for`. Uses TBB's `auto_partitioner` so the
  /// runtime adaptively splits ranges in response to worker idle signals --
  /// TBB's idiomatic shape. `participantCount` is consumed only as the
  /// construction hint via `make()`; the partitioner picks the per-task
  /// grain itself.
  template <class Fn>
  static void parallelFor(::tbb::task_arena &arena, std::size_t first, std::size_t last,
                          std::size_t /*participantCount*/, Fn fn) {
    arena.execute([&] {
      ::tbb::parallel_for(
          ::tbb::blocked_range<std::size_t>{first, last},
          [&](const ::tbb::blocked_range<std::size_t> &r) { fn(r.begin(), r.end()); },
          ::tbb::auto_partitioner{});
    });
  }

  /// Reduction with associative `combine`. Identity is the seed value.
  template <class T, class Map, class Combine>
  static T parallelReduce(::tbb::task_arena &arena, std::size_t first, std::size_t last, T identity,
                          Map map, Combine combine) {
    T result = identity;
    arena.execute([&] {
      result = ::tbb::parallel_reduce(
          ::tbb::blocked_range<std::size_t>{first, last}, identity,
          [&](const ::tbb::blocked_range<std::size_t> &r, T local) {
            return map(r.begin(), r.end(), local);
          },
          combine);
    });
    return result;
  }

  /// Inclusive scan. `body` mirrors oneTBB's `parallel_scan` body shape.
  template <class T, class Body>
  static T parallelScan(::tbb::task_arena &arena, std::size_t first, std::size_t last, T identity,
                        Body body) {
    T result = identity;
    arena.execute([&] {
      result = ::tbb::parallel_scan(::tbb::blocked_range<std::size_t>{first, last}, identity, body,
                                    std::plus<T>{});
    });
    return result;
  }

  /// Fan-out / submit-detached emulation; oneTBB has no detached primitive,
  /// so the closure runs to completion under the arena and the producer waits.
  template <class Fn> static void fanout(::tbb::task_arena &arena, Fn fn) {
    arena.execute(std::move(fn));
  }

  /// Emulate `parallelChain` via |stageCount| back-to-back `parallel_for` waves.
  ///
  /// oneTBB has no first-class chain primitive (no shared rendezvous, no per-stage
  /// descriptor reuse). Each stage closes a fresh `parallel_for`, paying full
  /// dispatch + join overhead per stage. This shape is the honest comparison
  /// baseline for our `parallelChain` primitive.
  ///
  /// |participantCount| is the grain hint passed through to the inner
  /// `parallel_for`. The argument exists so call sites can template over the
  /// same `Traits::parallelChain` signature for every adapter; the future-pool
  /// shims need it as a block count, the native shims accept it as a chunk
  /// hint.
  ///
  /// arena            Arena to dispatch into.
  /// first            Inclusive lower bound of the stage range.
  /// last             Exclusive upper bound of the stage range.
  /// participantCount Per-stage block count (used as `parallel_for` grain hint).
  /// stageCount       Number of sequential stages.
  /// fn               Body invoked as `fn(stageIdx, blockFirst, blockAfterLast)`.
  template <class Fn>
  static void parallelChain(::tbb::task_arena &arena, std::size_t first, std::size_t last,
                            std::size_t participantCount, std::size_t stageCount, Fn fn) {
    const std::size_t span = last - first;
    const std::size_t blocks = participantCount == 0U ? std::size_t{1} : participantCount;
    const std::size_t grain = (span + blocks - 1U) / blocks;
    for (std::size_t stage = 0; stage < stageCount; ++stage) {
      arena.execute([&] {
        ::tbb::parallel_for(
            ::tbb::blocked_range<std::size_t>{first, last, grain == 0U ? std::size_t{1} : grain},
            [&, stage](const ::tbb::blocked_range<std::size_t> &r) {
              fn(stage, r.begin(), r.end());
            });
      });
    }
  }
};
#endif // CITOR_BENCH_HAS_TBB

#ifdef CITOR_BENCH_HAS_TASKFLOW
/// Trait for Taskflow's `tf::Executor`.
///
/// Taskflow's primitive surface is graph-based: tasks are nodes, executor runs
/// the graph. The bench shapes each primitive into a `tf::Taskflow` graph and
/// blocks on `executor.run().wait()`.
///
/// Primitive mapping:
///   - `submitBlocksAndWait` -> single-task taskflow + run + wait.
///   - `parallelFor`         -> N range-strided tasks, one per worker block.
///   - `parallelReduce`      -> not provided; the Pareto-reduce bench inlines
///     per-pool because the bookkeeping varies enough that a uniform signature
///     would obscure it.
///   - `parallelScan`        -> emulated; Taskflow does not ship a scan, so
///     a sequential prefix sum is run on the producer thread for correctness.
///     This is honest -- a competitor that lacks the primitive cannot win the
///     row by definition.
///   - `fanout`              -> single-task taskflow.
///   - `parallelChain`       -> N taskflows joined back-to-back; Taskflow has
///     no shared rendezvous chain, so each stage is a fresh graph.
template <> struct CompetitorTraits<::tf::Executor> {
  /// Display name used in the bench table's rightmost column.
  static constexpr const char *name = "Taskflow";

  /// Construct an executor with |participants| workers.
  static auto make(std::size_t participants) {
    return std::make_unique<::tf::Executor>(static_cast<std::size_t>(participants));
  }

  /// Per-block fan-out used by the dispatch-floor workload. One task per element
  /// matches the shape every other adapter uses.
  template <class Fn>
  static void submitBlocksAndWait(::tf::Executor &exec, std::size_t first, std::size_t last,
                                  Fn fn) {
    if (last <= first) {
      return;
    }
    ::tf::Taskflow flow;
    for (std::size_t i = first; i < last; ++i) {
      flow.emplace([i, &fn]() { fn(i, i + 1U); });
    }
    exec.run(flow).wait();
  }

  /// Range-partitioned for: split `[first, last)` into `participantCount`
  /// blocks. Taskflow's executor work-steals between workers, so an idle
  /// worker can pick up another's block when the tail straggles.
  template <class Fn>
  static void parallelFor(::tf::Executor &exec, std::size_t first, std::size_t last,
                          std::size_t participantCount, Fn fn) {
    if (participantCount == 0) {
      participantCount = 1;
    }
    ::tf::Taskflow flow;
    const std::size_t span = last - first;
    const std::size_t blocks = participantCount;
    const std::size_t block = (span + blocks - 1) / blocks;
    for (std::size_t b = 0; b < blocks; ++b) {
      const std::size_t bf = first + std::min(span, b * block);
      const std::size_t bl = first + std::min(span, (b + 1) * block);
      if (bf == bl) {
        continue;
      }
      flow.emplace([bf, bl, &fn]() { fn(bf, bl); });
    }
    exec.run(flow).wait();
  }

  // Native-pool parallelReduce shim is not provided; the Pareto-reduce bench
  // inlines per-pool because the bookkeeping varies enough that a uniform
  // signature would obscure it.

  /// Scan emulated as a serial pass on the producer; Taskflow has no parallel scan.
  template <class T, class Body>
  static T parallelScan(::tf::Executor &exec, std::size_t first, std::size_t last, T identity,
                        Body body) {
    (void)exec;
    return body(first, last, identity);
  }

  /// Fan-out / submit-detached: dispatch one task and wait. Taskflow has
  /// `silent_async` for true detached, but the bench measures dispatch latency,
  /// so we wait on the run handle to keep the timing honest.
  template <class Fn> static void fanout(::tf::Executor &exec, Fn fn) {
    ::tf::Taskflow flow;
    flow.emplace(std::move(fn));
    exec.run(flow).wait();
  }

  /// Chain emulated as |stageCount| sequential taskflow runs. Documents the
  /// substitution: Taskflow has no shared-rendezvous chain primitive.
  ///
  /// |participantCount| is the per-stage block count (caller-supplied so
  /// every adapter's `parallelChain` shares the same signature).
  template <class Fn>
  static void parallelChain(::tf::Executor &exec, std::size_t first, std::size_t last,
                            std::size_t participantCount, std::size_t stageCount, Fn fn) {
    const std::size_t blocks = participantCount == 0U ? std::size_t{1} : participantCount;
    const std::size_t span = last - first;
    const std::size_t block = (span + blocks - 1U) / blocks;
    for (std::size_t stage = 0; stage < stageCount; ++stage) {
      ::tf::Taskflow flow;
      for (std::size_t b = 0; b < blocks; ++b) {
        const std::size_t bf = first + std::min(span, b * block);
        const std::size_t bl = first + std::min(span, (b + 1) * block);
        if (bf == bl) {
          continue;
        }
        flow.emplace([bf, bl, stage, &fn]() { fn(stage, bf, bl); });
      }
      exec.run(flow).wait();
    }
  }
};
#endif // CITOR_BENCH_HAS_TASKFLOW

#ifdef CITOR_BENCH_HAS_EIGEN_THREADPOOL
/// Trait for Eigen's `Eigen::ThreadPool` (the non-blocking pool).
///
/// Eigen ships only `Schedule()` and `Barrier`; every other primitive is
/// shimmed on top of those two pieces. The pattern is uniform: split the range
/// into N blocks, `Schedule()` each block with a barrier `Notify` at the end,
/// and `Wait()` on the producer.
///
/// Primitive mapping (every primitive shims around `Schedule()` + `Barrier`):
///   - `submitBlocksAndWait` -> 1-block schedule + barrier wait.
///   - `parallelFor`         -> N-block schedule + barrier wait.
///   - `parallelReduce`      -> not provided; the Pareto-reduce bench inlines
///     per-pool because the bookkeeping varies enough that a uniform signature
///     would obscure it.
///   - `parallelScan`        -> serial fallback; Eigen has no scan primitive.
///   - `fanout`              -> single `Schedule` + barrier wait.
///   - `parallelChain`       -> back-to-back parallelFor waves.
template <> struct CompetitorTraits<::Eigen::ThreadPool> {
  /// Display name used in the bench table's rightmost column.
  static constexpr const char *name = "Eigen::ThreadPool";

  /// Construct an Eigen non-blocking thread pool with |participants| workers.
  static auto make(std::size_t participants) {
    return std::make_unique<::Eigen::ThreadPool>(static_cast<int>(participants));
  }

  /// Per-element schedule fan-out + barrier wait. One Schedule call per element
  /// matches every other adapter's shape so the comparison measures actual
  /// fan-out cost, not a collapsed single-task path.
  template <class Fn>
  static void submitBlocksAndWait(::Eigen::ThreadPool &pool, std::size_t first, std::size_t last,
                                  Fn fn) {
    if (last <= first) {
      return;
    }
    const std::size_t blocks = last - first;
    ::Eigen::Barrier bar(static_cast<unsigned int>(blocks));
    for (std::size_t i = first; i < last; ++i) {
      pool.Schedule([&bar, i, &fn]() {
        fn(i, i + 1U);
        bar.Notify();
      });
    }
    bar.Wait();
  }

  /// Range-partitioned for. Each block = one `Schedule()` + barrier `Notify`.
  template <class Fn>
  static void parallelFor(::Eigen::ThreadPool &pool, std::size_t first, std::size_t last,
                          std::size_t participantCount, Fn fn) {
    if (participantCount == 0) {
      participantCount = 1;
    }
    const std::size_t span = last - first;
    const std::size_t blocks = participantCount;
    const std::size_t block = (span + blocks - 1) / blocks;
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    for (std::size_t b = 0; b < blocks; ++b) {
      const std::size_t bf = first + std::min(span, b * block);
      const std::size_t bl = first + std::min(span, (b + 1) * block);
      if (bf == bl) {
        continue;
      }
      ranges.emplace_back(bf, bl);
    }
    if (ranges.empty()) {
      return;
    }
    ::Eigen::Barrier bar(static_cast<unsigned int>(ranges.size()));
    for (const auto &range : ranges) {
      const std::size_t bf = range.first;
      const std::size_t bl = range.second;
      pool.Schedule([&bar, bf, bl, &fn]() {
        fn(bf, bl);
        bar.Notify();
      });
    }
    bar.Wait();
  }

  // Native-pool parallelReduce shim is not provided; the Pareto-reduce bench
  // inlines per-pool because the bookkeeping varies enough that a uniform
  // signature would obscure it.

  /// Scan: serial fallback; Eigen has no scan primitive.
  template <class T, class Body>
  static T parallelScan(::Eigen::ThreadPool &pool, std::size_t first, std::size_t last, T identity,
                        Body body) {
    (void)pool;
    return body(first, last, identity);
  }

  /// Fan-out: one `Schedule()` + barrier wait.
  template <class Fn> static void fanout(::Eigen::ThreadPool &pool, Fn fn) {
    ::Eigen::Barrier bar(/*count=*/1);
    pool.Schedule([&bar, fn = std::move(fn)]() mutable {
      fn();
      bar.Notify();
    });
    bar.Wait();
  }

  /// Chain: emulated as `stageCount` back-to-back `parallelFor` waves.
  ///
  /// |participantCount| is the per-stage block count (caller-supplied so
  /// every adapter's `parallelChain` shares the same signature).
  template <class Fn>
  static void parallelChain(::Eigen::ThreadPool &pool, std::size_t first, std::size_t last,
                            std::size_t participantCount, std::size_t stageCount, Fn fn) {
    const std::size_t blocks = participantCount == 0U ? std::size_t{1} : participantCount;
    for (std::size_t stage = 0; stage < stageCount; ++stage) {
      parallelFor(pool, first, last, blocks,
                  [&fn, stage](std::size_t bf, std::size_t bl) { fn(stage, bf, bl); });
    }
  }
};
#endif // CITOR_BENCH_HAS_EIGEN_THREADPOOL

#ifdef CITOR_BENCH_HAS_OPENMP
/// Thin wrapper so `CompetitorTraits<>` can specialize on a unique type.
///
/// OpenMP is pragma-based; there is no first-class "OpenMP runtime object" to
/// specialize on. The bench owns a single `OpenMpRunner` per row; the `make()`
/// trait constructs it with the requested participant count and the trait
/// methods route the closure through `#pragma omp parallel for` directives.
struct OpenMpRunner {
  /// Number of OpenMP threads used by every primitive on this runner.
  std::size_t threads;
};

/// Trait for the OpenMP shim wrapper.
///
/// Primitive mapping (each opens a fresh `parallel` region):
///   - `submitBlocksAndWait` -> single-block `parallel for` over `[first, last)`.
///   - `parallelFor`         -> `parallel for schedule(static)`.
///   - `parallelReduce`      -> not provided; the Pareto-reduce bench inlines
///     per-pool because the bookkeeping varies enough that a uniform signature
///     would obscure it.
///   - `parallelScan`        -> serial fallback wrapped in a single-thread
///     parallel region; OpenMP gained `scan` in 5.0 but the directive depends
///     on the compiler/runtime supporting `inscan` clauses, which is not
///     uniform.
///   - `fanout`              -> `parallel` region invoking the closure on every
///     thread (the closure receives the thread index).
///   - `parallelChain`       -> |stageCount| back-to-back `parallel for`
///     regions. OpenMP has no chain primitive; each stage pays full overhead.
template <> struct CompetitorTraits<OpenMpRunner> {
  /// Display name used in the bench table's rightmost column.
  static constexpr const char *name = "OpenMP";

  /// Construct an `OpenMpRunner` carrying the participant count.
  static auto make(std::size_t participants) {
    return std::make_unique<OpenMpRunner>(OpenMpRunner{participants});
  }

  /// Per-element parallel for. One body invocation per element matches every
  /// other adapter's shape so the comparison measures actual N-way fan-out and
  /// not a `single` short-circuit.
  template <class Fn>
  static void submitBlocksAndWait(OpenMpRunner &runner, std::size_t first, std::size_t last,
                                  Fn fn) {
    if (last <= first) {
      return;
    }
    const int threads = static_cast<int>(runner.threads);
    const auto firstSigned = static_cast<std::ptrdiff_t>(first);
    const auto lastSigned = static_cast<std::ptrdiff_t>(last);
#pragma omp parallel for num_threads(threads) schedule(static, 1)
    for (std::ptrdiff_t i = firstSigned; i < lastSigned; ++i) {
      fn(static_cast<std::size_t>(i), static_cast<std::size_t>(i + 1));
    }
  }

  /// Range-partitioned `parallel for` over `threads` blocks with dynamic
  /// schedule. `schedule(dynamic, 1)` over the block index lets fast libomp
  /// workers pull additional blocks past their static share when one rank
  /// straggles.
  template <class Fn>
  static void parallelFor(OpenMpRunner &runner, std::size_t first, std::size_t last,
                          std::size_t /*participantCount*/, Fn fn) {
    if (last <= first) {
      return;
    }
    const int threads = static_cast<int>(runner.threads);
    const std::size_t span = last - first;
    const std::size_t blocks = static_cast<std::size_t>(threads);
    const std::size_t block = (span + blocks - 1U) / blocks;
    const auto blocksSigned = static_cast<std::ptrdiff_t>(blocks);
#pragma omp parallel for num_threads(threads) schedule(dynamic, 1)
    for (std::ptrdiff_t b = 0; b < blocksSigned; ++b) {
      const std::size_t lo = first + std::min(span, static_cast<std::size_t>(b) * block);
      const std::size_t hi = first + std::min(span, (static_cast<std::size_t>(b) + 1U) * block);
      if (lo < hi) {
        fn(lo, hi);
      }
    }
  }

  // Native-pool parallelReduce shim is not provided; the Pareto-reduce bench
  // inlines per-pool because the bookkeeping varies enough that a uniform
  // signature would obscure it.

  /// Scan: serial fallback. OpenMP 5.0's `scan` directive is uneven in support.
  template <class T, class Body>
  static T parallelScan(OpenMpRunner &runner, std::size_t first, std::size_t last, T identity,
                        Body body) {
    (void)runner;
    return body(first, last, identity);
  }

  /// Fan-out: open a parallel region; |fn| is invoked once per thread.
  template <class Fn> static void fanout(OpenMpRunner &runner, Fn fn) {
    const int threads = static_cast<int>(runner.threads);
#pragma omp parallel num_threads(threads)
    {
#pragma omp single
      fn();
    }
  }

  /// Chain: back-to-back `parallel for` regions. OpenMP has no chain primitive.
  ///
  /// |participantCount| is unused on this adapter (the runner's thread count
  /// drives `num_threads`); the argument exists so call sites can use the
  /// same `parallelChain` signature across every adapter.
  template <class Fn>
  static void parallelChain(OpenMpRunner &runner, std::size_t first, std::size_t last,
                            std::size_t /*participantCount*/, std::size_t stageCount, Fn fn) {
    const int threads = static_cast<int>(runner.threads);
    const auto firstSigned = static_cast<std::ptrdiff_t>(first);
    const auto lastSigned = static_cast<std::ptrdiff_t>(last);
    for (std::size_t stage = 0; stage < stageCount; ++stage) {
#pragma omp parallel for num_threads(threads) schedule(static)
      for (std::ptrdiff_t i = firstSigned; i < lastSigned; ++i) {
        fn(stage, static_cast<std::size_t>(i), static_cast<std::size_t>(i + 1));
      }
    }
  }
};
#endif // CITOR_BENCH_HAS_OPENMP

#ifdef CITOR_BENCH_HAS_LEOPARD
/// Trait for `hmthrp::ThreadPool` (Leopard) -- C++20 work-stealing pool with `dispatch`
///        and `parallel_loop`.
///
/// Leopard's `parallel_loop` auto-partitions into one block per worker thread and returns a
/// vector of futures (one per chunk). The bench shim wires it through `dispatch()` directly so
/// the partition is `participantCount` blocks (one per worker), matching every other peer.
/// `submitBlocksAndWait` on this adapter issues `last - first` per-element tasks, mirroring
/// BS / dp / task / riften.
template <> struct CompetitorTraits<hmthrp::ThreadPool> {
  static constexpr const char *name = "Leopard::ThreadPool";

  static auto make(std::size_t participants) {
    return std::make_unique<hmthrp::ThreadPool>(participants);
  }

  template <class Fn>
  static void submitBlocksAndWait(hmthrp::ThreadPool &pool, std::size_t first, std::size_t last,
                                  Fn fn) {
    if (last <= first) {
      return;
    }
    std::vector<std::future<void>> futures;
    futures.reserve(last - first);
    for (std::size_t i = first; i < last; ++i) {
      futures.emplace_back(pool.dispatch(false, [i, &fn]() { fn(i, i + 1U); }));
    }
    for (auto &f : futures) {
      f.get();
    }
  }

  template <class Fn>
  static void parallelFor(hmthrp::ThreadPool &pool, std::size_t first, std::size_t last,
                          std::size_t participantCount, Fn fn) {
    if (last <= first || participantCount == 0U) {
      return;
    }
    const std::size_t span = last - first;
    const std::size_t blocks = participantCount;
    const std::size_t block = (span + blocks - 1U) / blocks;
    std::vector<std::future<void>> futures;
    futures.reserve(blocks);
    for (std::size_t b = 0; b < blocks; ++b) {
      const std::size_t bf = first + std::min(span, b * block);
      const std::size_t bl = first + std::min(span, (b + 1) * block);
      if (bf == bl) {
        continue;
      }
      futures.emplace_back(pool.dispatch(false, [bf, bl, &fn]() { fn(bf, bl); }));
    }
    for (auto &f : futures) {
      f.get();
    }
  }

  /// Reduction emulated as N dispatch + per-block partials + serial merge. Leopard has no
  /// first-class reduce primitive; the bench measures fan-out + future-wait, mirroring the
  /// riften / dp / task adapter shape.
  template <class T, class Map, class Combine>
  static T parallelReduce(hmthrp::ThreadPool &pool, std::size_t first, std::size_t last,
                          std::size_t participantCount, T identity, Map map, Combine combine) {
    const std::size_t blocks = participantCount == 0U ? std::size_t{1} : participantCount;
    std::vector<T> partials(blocks, identity);
    const std::size_t span = last - first;
    const std::size_t block = (span + blocks - 1U) / blocks;
    std::vector<std::future<void>> futures;
    futures.reserve(blocks);
    for (std::size_t b = 0; b < blocks; ++b) {
      const std::size_t bf = first + std::min(span, b * block);
      const std::size_t bl = first + std::min(span, (b + 1) * block);
      if (bf == bl) {
        continue;
      }
      futures.emplace_back(pool.dispatch(
          false, [bf, bl, b, &partials, &map, identity]() { partials[b] = map(bf, bl, identity); }));
    }
    for (auto &f : futures) {
      f.get();
    }
    T result = identity;
    for (auto &p : partials) {
      result = combine(result, p);
    }
    return result;
  }

  /// Scan: serial fallback. Leopard has no scan primitive.
  template <class T, class Body>
  static T parallelScan(hmthrp::ThreadPool &pool, std::size_t first, std::size_t last, T identity,
                        Body body) {
    (void)pool;
    return body(first, last, identity);
  }

  /// Fan-out: dispatch one closure, wait on its future.
  template <class Fn> static void fanout(hmthrp::ThreadPool &pool, Fn fn) {
    pool.dispatch(false, std::move(fn)).get();
  }

  /// Chain emulated as |stageCount| back-to-back parallelFor waves.
  template <class Fn>
  static void parallelChain(hmthrp::ThreadPool &pool, std::size_t first, std::size_t last,
                            std::size_t participantCount, std::size_t stageCount, Fn fn) {
    for (std::size_t stage = 0; stage < stageCount; ++stage) {
      parallelFor(pool, first, last, participantCount,
                  [stage, &fn](std::size_t bf, std::size_t bl) { fn(stage, bf, bl); });
    }
  }
};
#endif // CITOR_BENCH_HAS_LEOPARD

#ifdef CITOR_BENCH_HAS_DISPENSO
/// Trait for `dispenso::ThreadPool` (Meta) -- work-stealing pool with native
///        `parallel_for` over `ChunkedRange`.
///
/// The shim binds a per-row `dispenso::ThreadPool` to a `dispenso::TaskSet` and routes the
/// dispatch through `dispenso::parallel_for(taskSet, ChunkedRange, body)`. The chunk size is
/// `participantCount` blocks (one per worker), matching every other peer's natural shape.
template <> struct CompetitorTraits<dispenso::ThreadPool> {
  static constexpr const char *name = "dispenso::ThreadPool";

  /// Construct a pool with the producer counted in the benchmark's total participant count.
  /// dispenso's constructor spawns only background workers; `TaskSet::wait()` lets the caller
  /// drain queued work as an additional participant.
  static auto make(std::size_t participants) {
    const std::size_t workerCount = participants > 0U ? participants - 1U : 0U;
    return std::make_unique<dispenso::ThreadPool>(workerCount);
  }

  template <class Fn>
  static void submitBlocksAndWait(dispenso::ThreadPool &pool, std::size_t first, std::size_t last,
                                  Fn fn) {
    if (last <= first) {
      return;
    }
    // Recommended form: `parallel_for(taskSet, start, end, body)` lets dispenso pick its
    // default chunking strategy (`kStatic`). Avoids the explicit-ChunkedRange + small-range
    // trap that triggers dispenso's inline-fallback with no consideration of body cost.
    dispenso::TaskSet taskSet(pool);
    dispenso::parallel_for(taskSet, first, last,
                           [&fn](std::size_t lo, std::size_t hi) { fn(lo, hi); });
  }

  template <class Fn>
  static void parallelFor(dispenso::ThreadPool &pool, std::size_t first, std::size_t last,
                          std::size_t /*participantCount*/, Fn fn) {
    if (last <= first) {
      return;
    }
    // Same recommended form for the bulk-partition entry. dispenso's default chunking
    // produces a partition shape comparable to citor's auto-derived chunk on bulk
    // workloads.
    dispenso::TaskSet taskSet(pool);
    dispenso::parallel_for(taskSet, first, last,
                           [&fn](std::size_t lo, std::size_t hi) { fn(lo, hi); });
  }

  /// Reduction emulated as N parallel_for chunks + per-block partials + serial merge.
  /// dispenso ships `parallel_for_deferred` and a TaskSet, but no first-class reduce; the
  /// bench measures fan-out + barrier-wait through the TaskSet's destructor.
  template <class T, class Map, class Combine>
  static T parallelReduce(dispenso::ThreadPool &pool, std::size_t first, std::size_t last,
                          std::size_t participantCount, T identity, Map map, Combine combine) {
    const std::size_t blocks = participantCount == 0U ? std::size_t{1} : participantCount;
    std::vector<T> partials(blocks, identity);
    const std::size_t span = last - first;
    const std::size_t block = (span + blocks - 1U) / blocks;
    {
      dispenso::TaskSet taskSet(pool);
      for (std::size_t b = 0; b < blocks; ++b) {
        const std::size_t bf = first + std::min(span, b * block);
        const std::size_t bl = first + std::min(span, (b + 1) * block);
        if (bf == bl) {
          continue;
        }
        taskSet.schedule(
            [bf, bl, b, &partials, &map, identity]() { partials[b] = map(bf, bl, identity); });
      }
    }
    T result = identity;
    for (auto &p : partials) {
      result = combine(result, p);
    }
    return result;
  }

  /// Scan: serial fallback. dispenso has no scan primitive.
  template <class T, class Body>
  static T parallelScan(dispenso::ThreadPool &pool, std::size_t first, std::size_t last, T identity,
                        Body body) {
    (void)pool;
    return body(first, last, identity);
  }

  /// Fan-out: schedule one closure, wait on the TaskSet's destructor.
  template <class Fn> static void fanout(dispenso::ThreadPool &pool, Fn fn) {
    dispenso::TaskSet taskSet(pool);
    taskSet.schedule(std::move(fn));
  }

  /// Chain emulated as |stageCount| back-to-back parallelFor waves.
  template <class Fn>
  static void parallelChain(dispenso::ThreadPool &pool, std::size_t first, std::size_t last,
                            std::size_t participantCount, std::size_t stageCount, Fn fn) {
    for (std::size_t stage = 0; stage < stageCount; ++stage) {
      parallelFor(pool, first, last, participantCount,
                  [stage, &fn](std::size_t bf, std::size_t bl) { fn(stage, bf, bl); });
    }
  }
};
#endif // CITOR_BENCH_HAS_DISPENSO

} // namespace citor::bench
