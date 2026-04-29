// Square dense single-precision matmul workload for the comparative pool bench.
//
// `C = A * B` over `N in {512, 1024, 2048, 4096}` at j = 16. The kernel is the
// standard ikj-ordered triple loop on row-major buffers; every pool runs the
// identical inner loop and only the row-block dispatch shape differs. j = 16
// matches the loop's headline target and the host's physical-core floor.
//
// The bench is the most universal real-kernel proxy: dp::thread_pool's
// published comparison page benchmarks matmul exclusively across competitor
// pools at five sizes. We replicate that shape so the headline table directly
// answers "how does citor::ThreadPool fare on actual GEMM dispatch?".

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "bench_format.h"
#include "bench_registry.h"
#include "competitor_traits.h"
#include "cycle_clock.h"

namespace citor::bench {
namespace {

/// Per-cell sample budget. The largest cell (`N=4096`) on this host is
/// hundreds of milliseconds per iteration; 20 samples keeps a row under ten
/// seconds while the median stays stable.
constexpr std::size_t kIterations = 20;

/// Warmup iterations dropped from the sample window. The first run pays the
/// pool's wake-up plus the first MMU touches on freshly-allocated A/B/C.
constexpr std::size_t kWarmupIterations = 5;

/// 64-byte aligned `float[]` deleter for the matmul buffers; pairs with a
/// `std::unique_ptr` so the `BenchRow` owner does not leak on early return.
struct AlignedFloatDeleter {
  void operator()(float *p) const noexcept {
    if (p != nullptr) {
      std::free(p);
    }
  }
};

using AlignedFloatBuffer = std::unique_ptr<float[], AlignedFloatDeleter>;

/// Allocate an aligned float buffer of `count` elements; aborts on failure.
AlignedFloatBuffer allocateAlignedFloats(std::size_t count) {
  void *raw = nullptr;
  const std::size_t bytes = ((count * sizeof(float) + 63U) / 64U) * 64U;
  if (::posix_memalign(&raw, 64U, bytes) != 0) {
    std::abort();
  }
  return AlignedFloatBuffer{static_cast<float *>(raw)};
}

/// Deterministic LCG-seeded fill so every (pool, N) cell sees the same A/B.
void deterministicFill(float *p, std::size_t count, std::uint32_t seed) noexcept {
  std::uint32_t state = seed;
  for (std::size_t i = 0; i < count; ++i) {
    state = state * 1664525U + 1013904223U;
    p[i] = static_cast<float>(state >> 12U) * (1.0F / 1048576.0F);
  }
}

/// Per-row-block fill seeded from the row index. Independent across blocks so
/// the parallel first-touch init can run on each pool's worker partition
/// without an order-dependent cross-block carry. Stable across participant
/// counts because `seed ^ i` does not depend on the partition shape.
inline void deterministicFillBlock(float *p, std::size_t n, std::size_t rowFirst,
                                   std::size_t rowLast, std::uint32_t seed) noexcept {
  for (std::size_t i = rowFirst; i < rowLast; ++i) {
    std::uint32_t state = seed ^ static_cast<std::uint32_t>(i);
    float *row = p + i * n;
    for (std::size_t j = 0; j < n; ++j) {
      state = state * 1664525U + 1013904223U;
      row[j] = static_cast<float>(state >> 12U) * (1.0F / 1048576.0F);
    }
  }
}

/// Compute one row block of `C = A * B` with ikj loop order. `A`, `B`, `C` are
/// row-major `N x N` buffers; the body owns rows in `[rowFirst, rowLast)`.
inline void matmulRowBlock(std::size_t rowFirst, std::size_t rowLast, std::size_t n,
                           const float *aBase, const float *bBase, float *cBase) noexcept {
  for (std::size_t i = rowFirst; i < rowLast; ++i) {
    float *cRow = cBase + i * n;
    for (std::size_t j = 0; j < n; ++j) {
      cRow[j] = 0.0F;
    }
    for (std::size_t k = 0; k < n; ++k) {
      const float aik = aBase[i * n + k];
      const float *bRow = bBase + k * n;
      for (std::size_t j = 0; j < n; ++j) {
        cRow[j] += aik * bRow[j];
      }
    }
  }
}

/// Per-pool row-block dispatch helper.
///
/// Each specialization fans `participants` row-blocks of `[0, n)` onto its
/// pool, invoking `body(rowFirst, rowLast)` once per block, and waits for all
/// blocks to finish. The shape mirrors `parallelFor` semantics across pools
/// that lack a fan-out primitive (BS, dp, task, riften, Eigen) and routes
/// through the natural primitive otherwise (citor, oneTBB, Taskflow, OpenMP).
template <class Pool> struct MatmulDispatch;

/// Hint preset for the matmul row-block dispatch: cancellation polls disabled.
struct MatmulHints : citor::HintsDefaults {
  static constexpr bool cancellationChecks = false;
};

template <> struct MatmulDispatch<citor::ThreadPool> {
  template <class Fn>
  static void run(citor::ThreadPool &pool, std::size_t n, std::size_t participants, Fn fn) {
    (void)participants;
    pool.parallelFor<MatmulHints>(std::size_t{0}, n,
                                  [&fn](std::size_t lo, std::size_t hi) { fn(lo, hi); });
  }
};

/// BS::light_thread_pool exposes `submit_blocks(first, last, fn, num_blocks)`
/// which already partitions the range; we set `num_blocks = participants`.
template <> struct MatmulDispatch<BS::light_thread_pool> {
  template <class Fn>
  static void run(BS::light_thread_pool &pool, std::size_t n, std::size_t participants, Fn fn) {
    pool.submit_blocks(std::size_t{0}, n, fn, participants).wait();
  }
};

template <> struct MatmulDispatch<dp::thread_pool<>> {
  template <class Fn>
  static void run(dp::thread_pool<> &pool, std::size_t n, std::size_t participants, Fn fn) {
    const std::size_t block = (n + participants - 1) / participants;
    std::vector<std::future<void>> futures;
    futures.reserve(participants);
    for (std::size_t b = 0; b < participants; ++b) {
      const std::size_t lo = std::min(n, b * block);
      const std::size_t hi = std::min(n, (b + 1) * block);
      if (lo >= hi) {
        continue;
      }
      futures.emplace_back(pool.enqueue([lo, hi, &fn]() { fn(lo, hi); }));
    }
    for (auto &f : futures) {
      f.get();
    }
  }
};

template <> struct MatmulDispatch<::task_thread_pool::task_thread_pool> {
  template <class Fn>
  static void run(::task_thread_pool::task_thread_pool &pool, std::size_t n,
                  std::size_t participants, Fn fn) {
    const std::size_t block = (n + participants - 1) / participants;
    std::vector<std::future<void>> futures;
    futures.reserve(participants);
    for (std::size_t b = 0; b < participants; ++b) {
      const std::size_t lo = std::min(n, b * block);
      const std::size_t hi = std::min(n, (b + 1) * block);
      if (lo >= hi) {
        continue;
      }
      futures.emplace_back(pool.submit([lo, hi, &fn]() { fn(lo, hi); }));
    }
    for (auto &f : futures) {
      f.get();
    }
  }
};

template <> struct MatmulDispatch<riften::Thiefpool> {
  template <class Fn>
  static void run(riften::Thiefpool &pool, std::size_t n, std::size_t participants, Fn fn) {
    const std::size_t block = (n + participants - 1) / participants;
    std::vector<std::future<void>> futures;
    futures.reserve(participants);
    for (std::size_t b = 0; b < participants; ++b) {
      const std::size_t lo = std::min(n, b * block);
      const std::size_t hi = std::min(n, (b + 1) * block);
      if (lo >= hi) {
        continue;
      }
      futures.emplace_back(pool.enqueue([lo, hi, &fn]() { fn(lo, hi); }));
    }
    for (auto &f : futures) {
      f.get();
    }
  }
};

#ifdef CITOR_BENCH_HAS_TBB
template <> struct MatmulDispatch<::tbb::task_arena> {
  template <class Fn>
  static void run(::tbb::task_arena &arena, std::size_t n, std::size_t participants, Fn fn) {
    const std::size_t grain = (n + participants - 1) / participants;
    arena.execute([&] {
      ::tbb::parallel_for(
          ::tbb::blocked_range<std::size_t>{0, n, grain == 0 ? std::size_t{1} : grain},
          [&fn](const ::tbb::blocked_range<std::size_t> &r) { fn(r.begin(), r.end()); },
          ::tbb::simple_partitioner{});
    });
  }
};
#endif

#ifdef CITOR_BENCH_HAS_TASKFLOW
template <> struct MatmulDispatch<::tf::Executor> {
  template <class Fn>
  static void run(::tf::Executor &exec, std::size_t n, std::size_t participants, Fn fn) {
    ::tf::Taskflow flow;
    const std::size_t block = (n + participants - 1) / participants;
    for (std::size_t b = 0; b < participants; ++b) {
      const std::size_t lo = std::min(n, b * block);
      const std::size_t hi = std::min(n, (b + 1) * block);
      if (lo >= hi) {
        continue;
      }
      flow.emplace([lo, hi, &fn]() { fn(lo, hi); });
    }
    exec.run(flow).wait();
  }
};
#endif

#ifdef CITOR_BENCH_HAS_EIGEN_THREADPOOL
template <> struct MatmulDispatch<::Eigen::ThreadPool> {
  template <class Fn>
  static void run(::Eigen::ThreadPool &pool, std::size_t n, std::size_t participants, Fn fn) {
    const std::size_t block = (n + participants - 1) / participants;
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    ranges.reserve(participants);
    for (std::size_t b = 0; b < participants; ++b) {
      const std::size_t lo = std::min(n, b * block);
      const std::size_t hi = std::min(n, (b + 1) * block);
      if (lo < hi) {
        ranges.emplace_back(lo, hi);
      }
    }
    if (ranges.empty()) {
      return;
    }
    ::Eigen::Barrier bar(static_cast<unsigned int>(ranges.size()));
    for (const auto &r : ranges) {
      const std::size_t lo = r.first;
      const std::size_t hi = r.second;
      pool.Schedule([&bar, lo, hi, &fn]() {
        fn(lo, hi);
        bar.Notify();
      });
    }
    bar.Wait();
  }
};
#endif

#ifdef CITOR_BENCH_HAS_OPENMP
template <> struct MatmulDispatch<OpenMpRunner> {
  template <class Fn>
  static void run(OpenMpRunner &runner, std::size_t n, std::size_t participants, Fn fn) {
    (void)participants;
    const int threads = static_cast<int>(runner.threads);
    const auto last = static_cast<std::ptrdiff_t>(n);
#pragma omp parallel num_threads(threads)
    {
      const std::size_t threadCount = static_cast<std::size_t>(omp_get_num_threads());
      const std::size_t threadIdx = static_cast<std::size_t>(omp_get_thread_num());
      const std::size_t block = (n + threadCount - 1) / threadCount;
      const std::size_t lo = std::min(n, threadIdx * block);
      const std::size_t hi = std::min(n, (threadIdx + 1) * block);
      if (lo < hi) {
        fn(lo, hi);
      }
    }
    (void)last;
  }
};
#endif

/// Sample one pool's per-call wall time for `C = A * B` at `N`.
///
/// PoolT          Concrete pool type (the trait's specialization key).
/// participants   Total worker count to construct the pool with.
/// n              Square dimension; matrices are `N x N` row-major.
/// cal            Calibration constant for converting cycles to ns.
/// A populated `BenchRow` ready for the comparison table.
template <class PoolT>
[[nodiscard]] BenchRow measureMatmul(std::size_t participants, std::size_t n,
                                     const CyclesPerNanosecond &cal) {
  using Traits = CompetitorTraits<PoolT>;
  auto pool = Traits::make(participants);

  const std::size_t elems = n * n;
  AlignedFloatBuffer aBuf = allocateAlignedFloats(elems);
  AlignedFloatBuffer bBuf = allocateAlignedFloats(elems);
  AlignedFloatBuffer cBuf = allocateAlignedFloats(elems);

  // Parallel first-touch init through the same dispatch path the timed
  // matmul uses. Each pool's worker assignment owns the rows it later
  // computes on, so memory pages first-fault on the worker's NUMA node
  // (CCD-local on AMD Zen multi-CCD). Symmetric across pools: the same
  // partition shape that times the matmul also seeds the buffers.
  float *aMut = aBuf.get();
  float *bMut = bBuf.get();
  float *cMut = cBuf.get();
  const auto fillBody = [aMut, bMut, cMut, n](std::size_t lo, std::size_t hi) noexcept {
    deterministicFillBlock(aMut, n, lo, hi, 0xA1B2C3D4U);
    deterministicFillBlock(bMut, n, lo, hi, 0x5E6F7081U);
    for (std::size_t i = lo; i < hi; ++i) {
      float *row = cMut + i * n;
      for (std::size_t j = 0; j < n; ++j) {
        row[j] = 0.0F;
      }
    }
  };
  MatmulDispatch<PoolT>::run(*pool, n, participants, fillBody);

  const float *aBase = aBuf.get();
  const float *bBase = bBuf.get();
  float *cBase = cBuf.get();

  const auto body = [aBase, bBase, cBase, n](std::size_t lo, std::size_t hi) noexcept {
    matmulRowBlock(lo, hi, n, aBase, bBase, cBase);
  };

  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    MatmulDispatch<PoolT>::run(*pool, n, participants, body);
  }

  std::vector<double> samples;
  samples.reserve(kIterations);
  for (std::size_t i = 0; i < kIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    MatmulDispatch<PoolT>::run(*pool, n, participants, body);
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
  }

  // Touch the result so the optimizer keeps the kernel.
  std::atomic_signal_fence(std::memory_order_seq_cst);
  volatile float sink = cBase[0] + cBase[elems - 1];
  (void)sink;

  return finalizeRow(Traits::name, samples);
}

/// One square-matmul cell in the sweep.
struct MatmulCell {
  std::size_t n;
  const char *suffix;
};

/// Four-size sweep: 512 / 1024 / 2048 / 4096. Matches the dp::thread_pool
/// published comparison harness so the sizes remain familiar to users
/// evaluating the table.
constexpr std::array<MatmulCell, 4> kCells{{
    {.n = 512, .suffix = "n512"},
    {.n = 1024, .suffix = "n1024"},
    {.n = 2048, .suffix = "n2048"},
    {.n = 4096, .suffix = "n4096"},
}};

/// Build a matmul comparison table for one `(j, N)` cell.
BenchTable buildTable(std::size_t participants, MatmulCell cell, const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload = std::string{"matmul_j16_"} + cell.suffix;

  table.rows.push_back(measureMatmul<citor::ThreadPool>(participants, cell.n, cal));
  table.rows.push_back(measureMatmul<BS::light_thread_pool>(participants, cell.n, cal));
  table.rows.push_back(measureMatmul<dp::thread_pool<>>(participants, cell.n, cal));
  table.rows.push_back(
      measureMatmul<::task_thread_pool::task_thread_pool>(participants, cell.n, cal));
  table.rows.push_back(measureMatmul<riften::Thiefpool>(participants, cell.n, cal));
#ifdef CITOR_BENCH_HAS_TBB
  table.rows.push_back(measureMatmul<::tbb::task_arena>(participants, cell.n, cal));
#endif
#ifdef CITOR_BENCH_HAS_TASKFLOW
  table.rows.push_back(measureMatmul<::tf::Executor>(participants, cell.n, cal));
#endif
#ifdef CITOR_BENCH_HAS_EIGEN_THREADPOOL
  table.rows.push_back(measureMatmul<::Eigen::ThreadPool>(participants, cell.n, cal));
#endif
#ifdef CITOR_BENCH_HAS_OPENMP
  table.rows.push_back(measureMatmul<OpenMpRunner>(participants, cell.n, cal));
#endif

  return table;
}

template <std::size_t CellIdx> BenchTable runCell(const CyclesPerNanosecond &cal) {
  constexpr MatmulCell cell = kCells[CellIdx];
  return buildTable(/*participants=*/16, cell, cal);
}

struct MatmulRegistrar {
  MatmulRegistrar() {
    registerWorkload({.name = "matmul_j16_n512", .run = &runCell<0>});
    registerWorkload({.name = "matmul_j16_n1024", .run = &runCell<1>});
    registerWorkload({.name = "matmul_j16_n2048", .run = &runCell<2>});
    registerWorkload({.name = "matmul_j16_n4096", .run = &runCell<3>});
  }
};

const MatmulRegistrar kRegistrar;

} // namespace
} // namespace citor::bench
