#pragma once

#include <type_traits>
#include <utility>

#include "citor/thread_pool.h"

#ifdef CITOR_BENCH_HAS_TBB
#include <oneapi/tbb/task_arena.h>
#include <oneapi/tbb/task_group.h>
#endif

#ifdef CITOR_BENCH_HAS_TASKFLOW
#include <taskflow/taskflow.hpp>
#endif

#ifdef CITOR_BENCH_HAS_OPENMP
#include <omp.h>
#endif

#ifdef CITOR_BENCH_HAS_DISPENSO
#include <dispenso/task_set.h>
#include <dispenso/thread_pool.h>
#endif

namespace citor::bench {

/// Compile-time advertisement of whether `Pool` supports recursive
///        spawn-and-wait without deadlocking.
///
/// The recursive shape used by `fork_join_bench.cpp` and downstream Tier-2
/// cilksort / Strassen workloads is `spawn(left, right); wait();` from inside
/// a worker. That requires the waiting thread to either run children itself or
/// yield work to other workers; otherwise every level of recursion consumes a
/// worker forever and the call deadlocks once depth exceeds the worker count
/// (see `fork_join_bench.cpp:23-33` for the deadlock rationale documenting
/// which competitors are excluded for this reason).
///
/// `supportsRecursiveSpawn = true` for the pools whose wait primitives drain
/// the local queue: `citor::ThreadPool` (slot-0 producer participates), oneTBB
/// (`task_group::wait` runs tasks the calling worker steals), OpenMP
/// (`#pragma omp task` + `#pragma omp taskwait`), and Taskflow Subflow when
/// the recursive child is spawned via `tf::Subflow::emplace` (the subflow
/// scheduler descheduled the parent until children complete).
///
/// `supportsRecursiveSpawn = false` (the default) for pools whose wait path is
/// a `std::future::get()` that blocks the calling worker on a condition
/// variable the scheduler does not coordinate with: BS::thread_pool,
/// dp::thread_pool, task_thread_pool, riften::Thiefpool, Eigen::ThreadPool,
/// and Taskflow's `Executor::async` + future shape. Trying to recursively
/// spawn through those adapters deadlocks.
///
/// The trait is consumed by `recursiveSpawn` below; specializing it for a new
/// pool is the contract for adding it to the recursive-fork-join benches.
template <class Pool> struct RecursiveForkJoinTraits {
  static constexpr bool supportsRecursiveSpawn = false;
};

template <> struct RecursiveForkJoinTraits<::citor::ThreadPool> {
  static constexpr bool supportsRecursiveSpawn = true;
};

#ifdef CITOR_BENCH_HAS_TBB
template <> struct RecursiveForkJoinTraits<::tbb::task_arena> {
  static constexpr bool supportsRecursiveSpawn = true;
};
#endif

#ifdef CITOR_BENCH_HAS_OPENMP
// `OpenMpRunner` lives in `competitor_traits.h`; forward-declare it here so
// the trait can specialize without dragging the OpenMP shim's full header
// into every TU that pulls this helper.
struct OpenMpRunner;
template <> struct RecursiveForkJoinTraits<OpenMpRunner> {
  static constexpr bool supportsRecursiveSpawn = true;
};
#endif

#ifdef CITOR_BENCH_HAS_TASKFLOW
// Taskflow's recursive support is conditional on the caller using
// `tf::Subflow::emplace`; the generic `Executor::async` path deadlocks. The
// helper template surfaces the constraint at compile time: callers pass
// `tf::Subflow*` rather than the executor itself, and `recursiveSpawn` keys
// off this specialization.
template <> struct RecursiveForkJoinTraits<::tf::Subflow> {
  static constexpr bool supportsRecursiveSpawn = true;
};
#endif

#ifdef CITOR_BENCH_HAS_DISPENSO
// dispenso supports recursive spawn via `dispenso::TaskSet::schedule` from
// inside a worker; the TaskSet destructor coordinates with the worker's
// scheduler so the parent task yields scheduling rights to its children.
// Verified empirically against a fib(22) probe (`scratch/forkjoin_dispenso_probe.cpp`):
// recursive `TaskSet::schedule(...)` inside a worker completes without
// deadlock, unlike Leopard's `dispatch` which blocks the calling worker on a
// future and livelocks the global queue.
template <> struct RecursiveForkJoinTraits<::dispenso::ThreadPool> {
  static constexpr bool supportsRecursiveSpawn = true;
};
#endif

namespace detail {

/// Helper used to defer the static_assert until a non-recursive Pool is
/// actually instantiated; relying on `false` directly would fire even on the
/// supported specializations because the assert is evaluated unconditionally
/// during template definition.
template <class> struct AlwaysFalse : std::false_type {};

} // namespace detail

/// Default hints type used by `recursiveSpawn` / `recursiveSpawn2` when
///        the caller does not supply one. Mirrors `fork_join_bench`'s
///        `ForkJoinHints` (split-CCD affinity); the citor `forkJoin`
///        primitive's `HintsT` template parameter has no default, so the
///        helper templates need a concrete type.
struct DefaultRecursiveSpawnHints {
  static constexpr ::citor::Affinity affinity = ::citor::Affinity::CcdLocal;
};

/// Recursively spawn |body| twice and wait for both children.
///
/// The helper picks the right submission primitive for `Pool` based on
/// `RecursiveForkJoinTraits<Pool>`. The body must accept a `Pool&` and is
/// invoked twice in parallel; the helper returns once both children have
/// joined.
///
/// For pools with `supportsRecursiveSpawn == false`, the template fails to
/// compile via `static_assert` with a fixed diagnostic. The diagnostic is
/// deliberately instructive ("use a different bench shape") rather than
/// generic so reviewers see why the substitution was made.
///
/// Pool  Pool type satisfying `RecursiveForkJoinTraits<Pool>::supportsRecursiveSpawn`.
/// Body  Callable invoked as `body(pool)` for each child.
/// pool  Pool reference passed to each child body.
/// body  Body to spawn twice; not moved (called twice).
template <class Pool, class Body> inline void recursiveSpawn(Pool &pool, Body &&body) {
  if constexpr (!RecursiveForkJoinTraits<Pool>::supportsRecursiveSpawn) {
    static_assert(detail::AlwaysFalse<Pool>::value,
                  "Pool does not support recursive spawn; use a different bench shape.");
  } else if constexpr (std::is_same_v<Pool, ::citor::ThreadPool>) {
    // citor's `forkJoin` runs both children with the producer as slot 0; the
    // wait drains the local queue so depth is bounded only by stack size.
    pool.template forkJoin<DefaultRecursiveSpawnHints>([&]() { body(pool); },
                                                       [&]() { body(pool); });
  }
#ifdef CITOR_BENCH_HAS_TBB
  else if constexpr (std::is_same_v<Pool, ::tbb::task_arena>) {
    pool.execute([&] {
      ::tbb::task_group g;
      g.run([&] { body(pool); });
      g.run([&] { body(pool); });
      g.wait();
    });
  }
#endif
#ifdef CITOR_BENCH_HAS_OPENMP
  else if constexpr (std::is_same_v<Pool, OpenMpRunner>) {
    // OpenMP recursive shape: `task` + `taskwait`. The runner's `threads`
    // count is consulted by the surrounding `parallel` region the caller is
    // expected to have opened; this helper only emits the inner `task`
    // boundary so it can be invoked from inside a worker.
#pragma omp task shared(pool, body)
    body(pool);
#pragma omp task shared(pool, body)
    body(pool);
#pragma omp taskwait
  }
#endif
#ifdef CITOR_BENCH_HAS_TASKFLOW
  else if constexpr (std::is_same_v<Pool, ::tf::Subflow>) {
    // Taskflow's first-class recursive shape: `Subflow::emplace` schedules
    // children whose lifetime is tied to the surrounding subflow. The
    // executor coordinates the wait with the worker's local queue, unlike
    // `Executor::async` + `std::future::get()` which deadlocks.
    pool.emplace([&](::tf::Subflow &child) { body(child); });
    pool.emplace([&](::tf::Subflow &child) { body(child); });
    pool.join();
  }
#endif
#ifdef CITOR_BENCH_HAS_DISPENSO
  else if constexpr (std::is_same_v<Pool, ::dispenso::ThreadPool>) {
    // dispenso's TaskSet destructor runs as a wait-and-drain; nested
    // `schedule` calls from inside a worker are picked up by other workers
    // (or the producer's drain on the dtor path).
    ::dispenso::TaskSet ts(pool);
    ts.schedule([&]() { body(pool); });
    ts.schedule([&]() { body(pool); });
  }
#endif
}

/// Recursively spawn two distinct children and wait for both to join.
///
/// Two-body sibling of `recursiveSpawn:` the helper picks the same
/// submission primitive but invokes |left| and |right| exactly once each
/// (not the same body twice). Used by divide-and-conquer workloads where
/// the left and right children process different ranges (UTS, knapsack,
/// cilksort, Strassen).
///
/// Compile-time gating mirrors the single-body overload: pools with
/// `supportsRecursiveSpawn == false` fail to compile, with a diagnostic
/// pointing at the alternative bench shape.
///
/// Pool   Pool type satisfying `RecursiveForkJoinTraits<Pool>::supportsRecursiveSpawn`.
/// Left   Callable invoked as `left(pool)`; runs once on the left child.
/// Right  Callable invoked as `right(pool)`; runs once on the right child.
/// pool   Pool reference passed to both child bodies.
/// left   Left-child body; not moved (called once).
/// right  Right-child body; not moved (called once).
template <class Pool, class Left, class Right>
inline void recursiveSpawn2(Pool &pool, Left &&left, Right &&right) {
  if constexpr (!RecursiveForkJoinTraits<Pool>::supportsRecursiveSpawn) {
    static_assert(detail::AlwaysFalse<Pool>::value,
                  "Pool does not support recursive spawn; use a different bench shape.");
  } else if constexpr (std::is_same_v<Pool, ::citor::ThreadPool>) {
    pool.template forkJoin<DefaultRecursiveSpawnHints>([&]() { left(pool); },
                                                       [&]() { right(pool); });
  }
#ifdef CITOR_BENCH_HAS_TBB
  else if constexpr (std::is_same_v<Pool, ::tbb::task_arena>) {
    pool.execute([&] {
      ::tbb::task_group g;
      g.run([&] { left(pool); });
      g.run([&] { right(pool); });
      g.wait();
    });
  }
#endif
#ifdef CITOR_BENCH_HAS_OPENMP
  else if constexpr (std::is_same_v<Pool, OpenMpRunner>) {
#pragma omp task shared(pool, left)
    left(pool);
#pragma omp task shared(pool, right)
    right(pool);
#pragma omp taskwait
  }
#endif
#ifdef CITOR_BENCH_HAS_TASKFLOW
  else if constexpr (std::is_same_v<Pool, ::tf::Subflow>) {
    pool.emplace([&](::tf::Subflow &child) { left(child); });
    pool.emplace([&](::tf::Subflow &child) { right(child); });
    pool.join();
  }
#endif
#ifdef CITOR_BENCH_HAS_DISPENSO
  else if constexpr (std::is_same_v<Pool, ::dispenso::ThreadPool>) {
    ::dispenso::TaskSet ts(pool);
    ts.schedule([&]() { left(pool); });
    ts.schedule([&]() { right(pool); });
  }
#endif
}

} // namespace citor::bench
