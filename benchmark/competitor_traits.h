#pragma once

#include <thread_pool/thread_pool.h>

#include <BS_thread_pool.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <task_thread_pool.hpp>
#include <utility>
#include <vector>

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
template <class Pool> struct CompetitorTraits;

/// Bench-only runtime hint preset that mirrors the empty-fan-out workload's expectations.
///
/// Static-uniform balance, no inline-fallback gate (`estimatedItemNs == 0`), no cancellation
/// checks. The struct is shaped like the `BulkBalancedHints` sibling but with the fields a
/// dispatch-floor bench cares about: a single worker-strided block per participant, no extra
/// branching on the hot path.
struct EmptyFanoutHints {
  static constexpr citor::Balance balance =
      citor::Balance::StaticUniform;
  static constexpr citor::Determinism determinism =
      citor::Determinism::FixedBlockOrder;
  static constexpr citor::Affinity affinity =
      citor::Affinity::PhysicalCores;
  static constexpr citor::Priority priority =
      citor::Priority::Throughput;
  static constexpr citor::Partition partition =
      citor::Partition::ContiguousRanges;
  static constexpr double estimatedItemNs = 0.0;
  static constexpr double minTaskUs = 0.0;
  static constexpr std::size_t chunk = 1;
  static constexpr bool tlsRequired = false;
  static constexpr bool allowProducer = true;
  static constexpr bool allowWorkerSteal = false;
  static constexpr bool allowNestedParallelism = false;
  static constexpr bool fpDeterministicTree = true;
  static constexpr bool cancellationChecks = false;
  static constexpr bool pipelineSameChunk = false;
};

/// Trait for the new `citor::ThreadPool`.
///
/// The bench dispatches the closure as a single static-uniform `parallelFor<EmptyFanoutHints>`
/// spanning `[first, last)`. That mirrors the "submit one block, wait" shape the BS / dp / task /
/// riften adapters use, so dispatch latency is measured on equal footing.
template <> struct CompetitorTraits<citor::ThreadPool> {
  /// Display name used in the bench table's rightmost column.
  static constexpr const char *name = "citor::ThreadPool";

  /// Construct the new pool with |participants| total participants.
  ///
  /// `citor::ThreadPool` interprets `participants` as
  /// "producer + background workers"; slot 0 is the producer thread, so the
  /// pool spawns `participants - 1` background pthreads.
  ///
  /// participants Total participant count, including the producer.
  /// Owning pointer to the constructed pool.
  static auto make(std::size_t participants) {
    return std::make_unique<citor::ThreadPool>(participants);
  }

  /// Dispatch the closure as a single `parallelFor<EmptyFanoutHints>` invocation.
  ///
  /// The bench harness submits an empty range `[0, 1)` to isolate dispatch latency from any
  /// per-block work. With `chunk = 1` and a one-element range the call publishes one block, the
  /// producer runs it inline as slot 0, and the join waits until every background worker stamps
  /// the matching `doneEpoch`.
  ///
  /// pool  Pool instance to dispatch into.
  /// first Inclusive lower bound of the range.
  /// last  Exclusive upper bound of the range.
  /// fn    Callable invoked once per block as `fn(blockFirst, blockAfterLast)`.
  template <class Fn>
  static void submitBlocksAndWait(citor::ThreadPool &pool, std::size_t first,
                                  std::size_t last, Fn fn) {
    pool.parallelFor<EmptyFanoutHints>(first, last, fn);
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

  /// Submit a single block over `[first, last)` and wait for completion.
  ///
  /// pool  Pool the block is submitted to.
  /// first Inclusive lower bound of the range.
  /// last  Exclusive upper bound of the range.
  /// fn    Callable invoked once with the block's range.
  template <class Fn>
  static void submitBlocksAndWait(BS::light_thread_pool &pool, std::size_t first, std::size_t last,
                                  Fn fn) {
    pool.submit_blocks(first, last, fn, /*num_blocks=*/std::size_t{1}).wait();
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
    // `dp::thread_pool::enqueue` invokes the closure as a `const` callable in
    // its C++17 fallback path (no `mutable` on the wrapping lambda). Use a
    // const-callable closure so the compile-time fallback path stays valid.
    auto fut = pool.enqueue([first, last, fn]() { fn(first, last); });
    fut.get();
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
    auto fut = pool.submit([first, last, fn = std::move(fn)]() mutable { fn(first, last); });
    fut.get();
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
    auto fut = pool.enqueue([first, last, fn = std::move(fn)]() mutable { fn(first, last); });
    fut.get();
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

  /// Range-partitioned `parallel_for`. The chunk hint maps to `blocked_range::grainsize`.
  template <class Fn>
  static void parallelFor(::tbb::task_arena &arena, std::size_t first, std::size_t last,
                          std::size_t grain, Fn fn) {
    arena.execute([&] {
      ::tbb::parallel_for(
          ::tbb::blocked_range<std::size_t>{first, last, grain == 0 ? std::size_t{1} : grain},
          [&](const ::tbb::blocked_range<std::size_t> &r) { fn(r.begin(), r.end()); });
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
  /// arena      Arena to dispatch into.
  /// first      Inclusive lower bound of the stage range.
  /// last       Exclusive upper bound of the stage range.
  /// stageCount Number of sequential stages.
  /// fn         Body invoked as `fn(stageIdx, blockFirst, blockAfterLast)`.
  template <class Fn>
  static void parallelChain(::tbb::task_arena &arena, std::size_t first, std::size_t last,
                            std::size_t stageCount, Fn fn) {
    for (std::size_t stage = 0; stage < stageCount; ++stage) {
      arena.execute([&] {
        ::tbb::parallel_for(::tbb::blocked_range<std::size_t>{first, last},
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
///   - `parallelReduce`      -> emulated; Taskflow does not ship a first-class
///     reduce, so the trait runs `for_each_index` over the chunk grid and
///     atomically merges per-thread partials.
///   - `parallelScan`        -> emulated; Taskflow does not ship a scan, so
///     a sequential prefix sum is run on the producer thread for correctness.
///     This is honest -- a competitor that lacks the primitive cannot win the
///     row by definition.
///   - `fanout`              -> single-task taskflow.
///   - `parallelChain`       -> N taskflows joined back-to-back; Taskflow has
///     no shared rendezvous chain, so each stage is a fresh graph.
template <> struct CompetitorTraits<::tf::Executor> {
  /// Display name used in the bench table's rightmost column.

} // namespace citor::bench
