// Many-independent-queries throughput.
//
// Measures `citor::ThreadPool::bulkForQueries` head-to-head
// against the full competitor set running the same per-query body over
// `q = 10000` queries of dimension `d = 64` against a synthetic point cloud
// of `n = 10000`. Each query body computes one distance per indexed point so
// the per-query cost stays high enough that pool dispatch never dominates the
// measurement (we measure the throughput of the bulk fan-out, not of
// `parallelFor` dispatch alone).
//
// Per-pool primitive mapping:
//   - citor pool              -> `bulkForQueries<BulkHints>`.
//   - BS::thread_pool          -> `submit_blocks(0, q, body).wait()` (native).
//   - dp::thread_pool          -> N back-to-back `enqueue` futures + waits;
//                                 the pool ships no native multi-block
//                                 primitive.
//   - task_thread_pool         -> N back-to-back `submit` futures + waits;
//                                 the pool ships no native multi-block
//                                 primitive.
//   - riften::Thiefpool        -> N back-to-back `enqueue` futures + waits;
//                                 the pool ships no native multi-block
//                                 primitive.
//   - oneTBB                   -> `parallelFor` via `tbb::parallel_for` inside
//   arena.
//   - Taskflow                 -> N-task taskflow + run + wait.
//   - Eigen::ThreadPool        -> N-block schedule + barrier wait.
//   - OpenMP                   -> `#pragma omp parallel for schedule(static)`.

#include <BS_thread_pool.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <future>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "citor/hints.h"
#include "citor/thread_pool.h"

#include "bench_format.h"
#include "bench_registry.h"
#include "competitor_traits.h"
#include "cycle_clock.h"

namespace citor::bench {
namespace {

/// Iterations per measurement window. 100 keeps each row's wall time near
/// seconds at the prescribed (q, n, d) shape; the median + err% are stable
/// across reruns.
constexpr std::size_t kIterations = 20;

/// Warmup iterations dropped from the sample window. The persistent workers
/// settle their poll budget after a handful of dispatches; we discard those.
constexpr std::size_t kWarmupIterations = 3;

/// Query count fanned across workers per `bulkForQueries` invocation.
constexpr std::size_t kQueries = 10'000;

/// Reference point count the synthetic body iterates over per query. The
/// inner loop runs `kRefPoints` distance evaluations per query.
constexpr std::size_t kRefPoints = 10'000;

/// Feature dimension; sets the per-pair distance cost to roughly `kDim`
/// multiply-adds.
constexpr std::size_t kDim = 64;

/// Worker count for the bench. Lines up with the representative bulk-query
/// workload size.
constexpr std::size_t kParticipants = 16;

/// Synthetic per-query body: compute the squared-Euclidean distance from
///        `query[q]` to every reference point and store the minimum in
///        `out[q]`.
///
/// The body is structurally a hot inner loop over a contiguous block: read a
/// row, read a block of contiguous rows, accumulate per-pair squared sums,
/// write a single scalar per query.
inline void runOneQuery(std::size_t qIdx, const float *queries,
                        const float *refs, float *out) noexcept {
  const float *q = queries + (qIdx * kDim);
  float best = std::numeric_limits<float>::max();
  for (std::size_t r = 0; r < kRefPoints; ++r) {
    const float *p = refs + (r * kDim);
    float acc = 0.0F;
    for (std::size_t k = 0; k < kDim; ++k) {
      const float diff = q[k] - p[k];
      acc += diff * diff;
    }
    best = std::min(acc, best);
  }
  out[qIdx] = best;
}

/// Workload buffers shared across every pool's row.
struct Workload {
  std::vector<float> queries;
  std::vector<float> refs;
  std::vector<float> outBuf;
};

[[nodiscard]] Workload buildWorkload() {
  Workload w;
  w.queries.assign(kQueries * kDim, 0.0F);
  w.refs.assign(kRefPoints * kDim, 0.0F);
  w.outBuf.assign(kQueries, 0.0F);
  std::mt19937 rng(0x5eedU);
  std::uniform_real_distribution<float> dist(-1.0F, 1.0F);
  for (auto &v : w.queries) {
    v = dist(rng);
  }
  for (auto &v : w.refs) {
    v = dist(rng);
  }
  return w;
}

/// Helper: run the per-query body across `[0, kQueries)` partitioned into
/// `blocks` contiguous chunks. Used as the inline shim for pools whose only
/// API is `enqueue`-style single-task submission (dp, task, riften); the
/// helper computes block boundaries the same way every other adapter does.
inline std::pair<std::size_t, std::size_t>
blockRange(std::size_t blockIdx, std::size_t blocks) noexcept {
  const std::size_t blockSize = (kQueries + blocks - 1) / blocks;
  const std::size_t lo = std::min(kQueries, blockIdx * blockSize);
  const std::size_t hi = std::min(kQueries, (blockIdx + 1) * blockSize);
  return {lo, hi};
}

/// Generic measurement loop: invoke |run|() once per warmup iteration, then
/// `kIterations` measured iterations stamping the cycle delta around each call.
template <class RunFn>
[[nodiscard]] BenchRow measureLoop(const char *name,
                                   const CyclesPerNanosecond &cal, RunFn run) {
  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    run();
  }
  std::vector<double> samples;
  samples.reserve(kIterations);
  for (std::size_t i = 0; i < kIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    run();
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
  }
  return finalizeRow(name, samples);
}

[[nodiscard]] BenchRow measureNewPool(const CyclesPerNanosecond &cal) {
  ThreadPool pool(kParticipants);
  Workload w = buildWorkload();
  const float *queries = w.queries.data();
  const float *refs = w.refs.data();
  float *out = w.outBuf.data();
  auto body = [queries, refs, out](std::size_t lo, std::size_t hi) {
    for (std::size_t q = lo; q < hi; ++q) {
      runOneQuery(q, queries, refs, out);
    }
  };
  BenchRow row = measureLoop("citor::ThreadPool", cal, [&] {
    pool.bulkForQueries<citor::BulkHints>(kQueries, body);
  });
  (void)out[0];
  return row;
}

[[nodiscard]] BenchRow measureBsPool(const CyclesPerNanosecond &cal) {
  BS::light_thread_pool pool(kParticipants);
  Workload w = buildWorkload();
  const float *queries = w.queries.data();
  const float *refs = w.refs.data();
  float *out = w.outBuf.data();
  auto body = [queries, refs, out](std::size_t lo, std::size_t hi) {
    for (std::size_t q = lo; q < hi; ++q) {
      runOneQuery(q, queries, refs, out);
    }
  };
  BenchRow row = measureLoop("BS::thread_pool", cal, [&] {
    pool.submit_blocks(std::size_t{0}, kQueries, body).wait();
  });
  (void)out[0];
  return row;
}

/// dp::thread_pool ships no multi-block API; we fan out N independent enqueues
/// (one per block) and join the resulting futures. This is the natural
/// dp-shaped equivalent of `submit_blocks`.
[[nodiscard]] BenchRow measureDpPool(const CyclesPerNanosecond &cal) {
  dp::thread_pool<> pool(static_cast<unsigned int>(kParticipants));
  Workload w = buildWorkload();
  const float *queries = w.queries.data();
  const float *refs = w.refs.data();
  float *out = w.outBuf.data();
  const std::size_t blocks = kParticipants;
  auto bodyChunk = [queries, refs, out](std::size_t lo, std::size_t hi) {
    for (std::size_t q = lo; q < hi; ++q) {
      runOneQuery(q, queries, refs, out);
    }
  };
  BenchRow row = measureLoop("dp::thread_pool", cal, [&] {
    std::vector<std::future<void>> futs;
    futs.reserve(blocks);
    for (std::size_t b = 0; b < blocks; ++b) {
      auto [lo, hi] = blockRange(b, blocks);
      if (lo == hi) {
        continue;
      }
      futs.emplace_back(
          pool.enqueue([lo, hi, &bodyChunk]() { bodyChunk(lo, hi); }));
    }
    for (auto &f : futs) {
      f.get();
    }
  });
  (void)out[0];
  return row;
}

/// task_thread_pool ships no multi-block API; same N-future shape as dp.
[[nodiscard]] BenchRow measureTaskPool(const CyclesPerNanosecond &cal) {
  ::task_thread_pool::task_thread_pool pool(
      static_cast<unsigned int>(kParticipants));
  Workload w = buildWorkload();
  const float *queries = w.queries.data();
  const float *refs = w.refs.data();
  float *out = w.outBuf.data();
  const std::size_t blocks = kParticipants;
  auto bodyChunk = [queries, refs, out](std::size_t lo, std::size_t hi) {
    for (std::size_t q = lo; q < hi; ++q) {
      runOneQuery(q, queries, refs, out);
    }
  };
  BenchRow row = measureLoop("task_thread_pool", cal, [&] {
    std::vector<std::future<void>> futs;
    futs.reserve(blocks);
    for (std::size_t b = 0; b < blocks; ++b) {
      auto [lo, hi] = blockRange(b, blocks);
      if (lo == hi) {
        continue;
      }
      futs.emplace_back(
          pool.submit([lo, hi, &bodyChunk]() { bodyChunk(lo, hi); }));
    }
    for (auto &f : futs) {
      f.get();
    }
  });
  (void)out[0];
  return row;
}

/// riften::Thiefpool ships no multi-block API; same N-future shape as dp.
[[nodiscard]] BenchRow measureRiftenPool(const CyclesPerNanosecond &cal) {
  riften::Thiefpool pool(kParticipants);
  Workload w = buildWorkload();
  const float *queries = w.queries.data();
  const float *refs = w.refs.data();
  float *out = w.outBuf.data();
  const std::size_t blocks = kParticipants;
  auto bodyChunk = [queries, refs, out](std::size_t lo, std::size_t hi) {
    for (std::size_t q = lo; q < hi; ++q) {
      runOneQuery(q, queries, refs, out);
    }
  };
  BenchRow row = measureLoop("riften::Thiefpool", cal, [&] {
    std::vector<std::future<void>> futs;
    futs.reserve(blocks);
    for (std::size_t b = 0; b < blocks; ++b) {
      auto [lo, hi] = blockRange(b, blocks);
      if (lo == hi) {
        continue;
      }
      futs.emplace_back(
          pool.enqueue([lo, hi, &bodyChunk]() { bodyChunk(lo, hi); }));
    }
    for (auto &f : futs) {
      f.get();
    }
  });
  (void)out[0];
  return row;
}

#ifdef CITOR_BENCH_HAS_TBB
[[nodiscard]] BenchRow measureTbbPool(const CyclesPerNanosecond &cal) {
  auto arena = CompetitorTraits<::tbb::task_arena>::make(kParticipants);
  Workload w = buildWorkload();
  const float *queries = w.queries.data();
  const float *refs = w.refs.data();
  float *out = w.outBuf.data();
  const std::size_t grain = (kQueries + kParticipants - 1) / kParticipants;
  auto bodyChunk = [queries, refs, out](std::size_t lo, std::size_t hi) {
    for (std::size_t q = lo; q < hi; ++q) {
      runOneQuery(q, queries, refs, out);
    }
  };
  BenchRow row =
      measureLoop(CompetitorTraits<::tbb::task_arena>::name, cal, [&] {
        CompetitorTraits<::tbb::task_arena>::parallelFor(
            *arena, std::size_t{0}, kQueries, grain, bodyChunk);
      });
  (void)out[0];
  return row;
}
#endif

#ifdef CITOR_BENCH_HAS_TASKFLOW
[[nodiscard]] BenchRow measureTaskflowPool(const CyclesPerNanosecond &cal) {
  auto exec = CompetitorTraits<::tf::Executor>::make(kParticipants);
  Workload w = buildWorkload();
  const float *queries = w.queries.data();
  const float *refs = w.refs.data();
  float *out = w.outBuf.data();
  auto bodyChunk = [queries, refs, out](std::size_t lo, std::size_t hi) {
    for (std::size_t q = lo; q < hi; ++q) {
      runOneQuery(q, queries, refs, out);
    }
  };
  BenchRow row = measureLoop(CompetitorTraits<::tf::Executor>::name, cal, [&] {
    CompetitorTraits<::tf::Executor>::parallelFor(
        *exec, std::size_t{0}, kQueries, kParticipants, bodyChunk);
  });
  (void)out[0];
  return row;
}
#endif

#ifdef CITOR_BENCH_HAS_EIGEN_THREADPOOL
[[nodiscard]] BenchRow measureEigenPool(const CyclesPerNanosecond &cal) {
  auto pool = CompetitorTraits<::Eigen::ThreadPool>::make(kParticipants);
  Workload w = buildWorkload();
  const float *queries = w.queries.data();
  const float *refs = w.refs.data();
  float *out = w.outBuf.data();
  auto bodyChunk = [queries, refs, out](std::size_t lo, std::size_t hi) {
    for (std::size_t q = lo; q < hi; ++q) {
      runOneQuery(q, queries, refs, out);
    }
  };
  BenchRow row =
      measureLoop(CompetitorTraits<::Eigen::ThreadPool>::name, cal, [&] {
        CompetitorTraits<::Eigen::ThreadPool>::parallelFor(
            *pool, std::size_t{0}, kQueries, kParticipants, bodyChunk);
      });
  (void)out[0];
  return row;
}
#endif

#ifdef CITOR_BENCH_HAS_LEOPARD
[[nodiscard]] BenchRow measureLeopardPool(const CyclesPerNanosecond &cal) {
  auto pool = CompetitorTraits<hmthrp::ThreadPool>::make(kParticipants);
  Workload w = buildWorkload();
  const float *queries = w.queries.data();
  const float *refs = w.refs.data();
  float *out = w.outBuf.data();
  auto bodyChunk = [queries, refs, out](std::size_t lo, std::size_t hi) {
    for (std::size_t q = lo; q < hi; ++q) {
      runOneQuery(q, queries, refs, out);
    }
  };
  BenchRow row =
      measureLoop(CompetitorTraits<hmthrp::ThreadPool>::name, cal, [&] {
        CompetitorTraits<hmthrp::ThreadPool>::parallelFor(
            *pool, std::size_t{0}, kQueries, kParticipants, bodyChunk);
      });
  (void)out[0];
  return row;
}
#endif

#ifdef CITOR_BENCH_HAS_DISPENSO
[[nodiscard]] BenchRow measureDispensoPool(const CyclesPerNanosecond &cal) {
  auto pool = CompetitorTraits<dispenso::ThreadPool>::make(kParticipants);
  Workload w = buildWorkload();
  const float *queries = w.queries.data();
  const float *refs = w.refs.data();
  float *out = w.outBuf.data();
  auto bodyChunk = [queries, refs, out](std::size_t lo, std::size_t hi) {
    for (std::size_t q = lo; q < hi; ++q) {
      runOneQuery(q, queries, refs, out);
    }
  };
  BenchRow row =
      measureLoop(CompetitorTraits<dispenso::ThreadPool>::name, cal, [&] {
        CompetitorTraits<dispenso::ThreadPool>::parallelFor(
            *pool, std::size_t{0}, kQueries, kParticipants, bodyChunk);
      });
  (void)out[0];
  return row;
}
#endif

#ifdef CITOR_BENCH_HAS_OPENMP
[[nodiscard]] BenchRow measureOpenMpPool(const CyclesPerNanosecond &cal) {
  auto runner = CompetitorTraits<OpenMpRunner>::make(kParticipants);
  Workload w = buildWorkload();
  const float *queries = w.queries.data();
  const float *refs = w.refs.data();
  float *out = w.outBuf.data();
  // The OpenMP `parallelFor` trait runs item-by-item (`fn(i, i+1)`); for this
  // workload we want a per-block call so the shape matches the other adapters,
  // so route through `parallelChain` with one stage to invoke `fn(stage, lo,
  // hi)` is wrong shape. Use a direct `#pragma omp parallel for` over kQueries
  // with one item per thread block via static schedule.
  auto bodyChunk = [queries, refs, out](std::size_t lo, std::size_t hi) {
    for (std::size_t q = lo; q < hi; ++q) {
      runOneQuery(q, queries, refs, out);
    }
  };
  BenchRow row = measureLoop(CompetitorTraits<OpenMpRunner>::name, cal, [&] {
    const auto threads = static_cast<int>(runner->threads);
    const auto blocks = static_cast<std::size_t>(threads);
#pragma omp parallel num_threads(threads)
    {
      const auto tid = static_cast<std::size_t>(omp_get_thread_num());
      const std::size_t blockSize = (kQueries + blocks - 1) / blocks;
      const std::size_t lo = std::min(kQueries, tid * blockSize);
      const std::size_t hi = std::min(kQueries, (tid + 1) * blockSize);
      if (lo < hi) {
        bodyChunk(lo, hi);
      }
    }
  });
  (void)out[0];
  return row;
}
#endif

BenchTable buildTable(const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload = "bulk_for_queries_q10k_n10k_d64_j16";
  table.rows.push_back(measureNewPool(cal));
  table.rows.push_back(measureBsPool(cal));
  table.rows.push_back(measureDpPool(cal));
  table.rows.push_back(measureTaskPool(cal));
  table.rows.push_back(measureRiftenPool(cal));
#ifdef CITOR_BENCH_HAS_TBB
  table.rows.push_back(measureTbbPool(cal));
#endif
#ifdef CITOR_BENCH_HAS_TASKFLOW
  table.rows.push_back(measureTaskflowPool(cal));
#endif
#ifdef CITOR_BENCH_HAS_EIGEN_THREADPOOL
  table.rows.push_back(measureEigenPool(cal));
#endif
#ifdef CITOR_BENCH_HAS_OPENMP
  table.rows.push_back(measureOpenMpPool(cal));
#endif
#ifdef CITOR_BENCH_HAS_LEOPARD
  table.rows.push_back(measureLeopardPool(cal));
#endif
#ifdef CITOR_BENCH_HAS_DISPENSO
  table.rows.push_back(measureDispensoPool(cal));
#endif
  return table;
}

/// File-scope registrar: pushes the workload into the bench registry at TU
/// initialization time.
struct BulkForQueriesRegistrar {
  BulkForQueriesRegistrar() {
    registerWorkload(
        {.name = "bulk_for_queries_q10k_n10k_d64_j16", .run = &buildTable});
  }
};

const BulkForQueriesRegistrar kRegistrar;

} // namespace

} // namespace citor::bench
