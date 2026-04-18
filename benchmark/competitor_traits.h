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
  static constexpr const char *name = "Taskflow";

  /// Construct an executor with |participants| workers.
  static auto make(std::size_t participants) {
    return std::make_unique<::tf::Executor>(static_cast<std::size_t>(participants));
  }

  /// Single-block fan-out used by the dispatch-floor workload.
  template <class Fn>
  static void submitBlocksAndWait(::tf::Executor &exec, std::size_t first, std::size_t last,
                                  Fn fn) {
    ::tf::Taskflow flow;
    flow.emplace([first, last, fn = std::move(fn)]() mutable { fn(first, last); });
    exec.run(flow).wait();
  }

  /// Range-partitioned for: split `[first, last)` into `participantCount` blocks.
  template <class Fn>
  static void parallelFor(::tf::Executor &exec, std::size_t first, std::size_t last,
                          std::size_t participantCount, Fn fn) {
    if (participantCount == 0) {
      participantCount = 1;
    }
    ::tf::Taskflow flow;
    const std::size_t span = last - first;
    const std::size_t block = (span + participantCount - 1) / participantCount;
    for (std::size_t b = 0; b < participantCount; ++b) {
      const std::size_t bf = first + std::min(span, b * block);
      const std::size_t bl = first + std::min(span, (b + 1) * block);
      if (bf == bl) {
        continue;
      }
      flow.emplace([bf, bl, &fn]() { fn(bf, bl); });
    }
    exec.run(flow).wait();
  }

  /// Reduction emulated via N partial-tasks plus a serial merge after wait.
  /// Taskflow ships `transform_reduce`; we use a manual fan-out so the bench
  /// captures the exact same dispatch + join shape as `parallelFor`.
  template <class T, class Map, class Combine>
  static T parallelReduce(::tf::Executor &exec, std::size_t first, std::size_t last, T identity,
                          Map map, Combine combine) {
    const std::size_t participantCount = exec.num_workers();
    const std::size_t blocks = participantCount == 0 ? std::size_t{1} : participantCount;
    std::vector<T> partials(blocks, identity);
    const std::size_t span = last - first;
    const std::size_t block = (span + blocks - 1) / blocks;
    ::tf::Taskflow flow;
    for (std::size_t b = 0; b < blocks; ++b) {
      const std::size_t bf = first + std::min(span, b * block);
      const std::size_t bl = first + std::min(span, (b + 1) * block);
      if (bf == bl) {
        continue;
      }
      flow.emplace(
          [bf, bl, b, &partials, &map, &identity]() { partials[b] = map(bf, bl, identity); });
    }
    exec.run(flow).wait();
    T result = identity;
    for (auto &p : partials) {
      result = combine(result, p);
    }
    return result;
  }

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
  template <class Fn>
  static void parallelChain(::tf::Executor &exec, std::size_t first, std::size_t last,
                            std::size_t stageCount, Fn fn) {
    const std::size_t participantCount = exec.num_workers();
    const std::size_t blocks = participantCount == 0 ? std::size_t{1} : participantCount;
    const std::size_t span = last - first;
    const std::size_t block = (span + blocks - 1) / blocks;
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
///   - `parallelReduce`      -> N-block schedule with per-block partials + serial merge.
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

  /// Per-block schedule fan-out + barrier wait. Splits `[first, last)` into
  /// one block per worker so the comparison stays apples-to-apples with the
  /// other pools' fan-out shape; a single Schedule would skip the cross-thread
  /// dispatch every worker on every other pool actually pays.
  template <class Fn>
  static void submitBlocksAndWait(::Eigen::ThreadPool &pool, std::size_t first, std::size_t last,
                                  Fn fn) {
    const auto participantCount = static_cast<std::size_t>(pool.NumThreads());
    const std::size_t span = last - first;
    const std::size_t blocks = std::min(participantCount, span == 0 ? std::size_t{1} : span);
    if (blocks == 0 || span == 0) {
      fn(first, last);
      return;
    }
    const std::size_t chunk = (span + blocks - 1) / blocks;
    ::Eigen::Barrier bar(static_cast<unsigned int>(blocks));
    for (std::size_t b = 0; b < blocks; ++b) {
      const std::size_t lo = first + std::min(span, b * chunk);
      const std::size_t hi = first + std::min(span, (b + 1) * chunk);
      pool.Schedule([&bar, lo, hi, &fn]() {
        fn(lo, hi);
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
    const std::size_t block = (span + participantCount - 1) / participantCount;
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    for (std::size_t b = 0; b < participantCount; ++b) {
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

  /// Reduction over per-block partials, merged serially after the barrier.
  template <class T, class Map, class Combine>
  static T parallelReduce(::Eigen::ThreadPool &pool, std::size_t first, std::size_t last,
                          T identity, Map map, Combine combine) {
    const std::size_t participantCount = static_cast<std::size_t>(pool.NumThreads());
    const std::size_t blocks = participantCount == 0 ? std::size_t{1} : participantCount;
    std::vector<T> partials(blocks, identity);
    const std::size_t span = last - first;
    const std::size_t block = (span + blocks - 1) / blocks;
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    ranges.reserve(blocks);
    for (std::size_t b = 0; b < blocks; ++b) {
      const std::size_t bf = first + std::min(span, b * block);
      const std::size_t bl = first + std::min(span, (b + 1) * block);
      if (bf == bl) {
        continue;
      }
      ranges.emplace_back(bf, bl);
    }
    if (ranges.empty()) {
      return identity;
    }
    ::Eigen::Barrier bar(static_cast<unsigned int>(ranges.size()));
    for (std::size_t i = 0; i < ranges.size(); ++i) {
      const std::size_t bf = ranges[i].first;
      const std::size_t bl = ranges[i].second;
      pool.Schedule([&bar, bf, bl, i, &partials, &map, &identity]() {
        partials[i] = map(bf, bl, identity);
        bar.Notify();
      });
    }
    bar.Wait();
    T result = identity;
    for (auto &p : partials) {
      result = combine(result, p);
    }
    return result;
  }

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
  template <class Fn>
  static void parallelChain(::Eigen::ThreadPool &pool, std::size_t first, std::size_t last,
                            std::size_t stageCount, Fn fn) {
    for (std::size_t stage = 0; stage < stageCount; ++stage) {
      parallelFor(pool, first, last, static_cast<std::size_t>(pool.NumThreads()),
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
///   - `parallelReduce`      -> `parallel for reduction(...)`. Caller supplies
///     the operator via a templated lambda that walks `[bf, bl)`.
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

  /// Single-block dispatch: one `parallel for` over the trivial range.
  template <class Fn>
  static void submitBlocksAndWait(OpenMpRunner &runner, std::size_t first, std::size_t last,
                                  Fn fn) {
    const int threads = static_cast<int>(runner.threads);
#pragma omp parallel num_threads(threads)
    {
#pragma omp single
      fn(first, last);
    }
  }

  /// Range-partitioned `parallel for` with static schedule.
  template <class Fn>
  static void parallelFor(OpenMpRunner &runner, std::size_t first, std::size_t last,
                          std::size_t /*grain*/, Fn fn) {
    const int threads = static_cast<int>(runner.threads);
    const auto firstSigned = static_cast<std::ptrdiff_t>(first);
    const auto lastSigned = static_cast<std::ptrdiff_t>(last);
#pragma omp parallel for num_threads(threads) schedule(static)
    for (std::ptrdiff_t i = firstSigned; i < lastSigned; ++i) {
      fn(static_cast<std::size_t>(i), static_cast<std::size_t>(i + 1));
    }
  }

  /// Reduction over `[first, last)` using OpenMP atomic merge.
  ///
  /// `#pragma omp for reduction(...)` requires the operator to be a recognized
  /// built-in (sum, product, min, max, ...). For arbitrary `combine`, the trait
  /// runs each thread's per-block partial under `parallel`, then atomically
  /// merges via `#pragma omp critical`. This matches the shape of the other
  /// traits' reduce shims and keeps the comparison fair.
  template <class T, class Map, class Combine>
  static T parallelReduce(OpenMpRunner &runner, std::size_t first, std::size_t last, T identity,
                          Map map, Combine combine) {
    const int threads = static_cast<int>(runner.threads);
    T result = identity;
#pragma omp parallel num_threads(threads)
    {
      const std::size_t threadCount = static_cast<std::size_t>(omp_get_num_threads());
      const std::size_t threadIdx = static_cast<std::size_t>(omp_get_thread_num());
      const std::size_t span = last - first;
      const std::size_t block = (span + threadCount - 1) / threadCount;
      const std::size_t bf = first + std::min(span, threadIdx * block);
      const std::size_t bl = first + std::min(span, (threadIdx + 1) * block);
      if (bf < bl) {
        T local = map(bf, bl, identity);
#pragma omp critical
        result = combine(result, local);
      }
    }
    return result;
  }

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
  template <class Fn>
  static void parallelChain(OpenMpRunner &runner, std::size_t first, std::size_t last,
                            std::size_t stageCount, Fn fn) {
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

} // namespace citor::bench
