// Recursive divide-and-conquer matmul for the comparative pool bench.
//
// `R = A * B` over `N x N` float matrices using the standard 2x2 block
// recursion. Each level splits A, B, R into 4 quadrants and dispatches 8
// half-sized sub-products in two parallel waves of 4:
//
//   Phase 1 (overwrite):    R00 = A00 * B00,  R01 = A00 * B01
//                           R10 = A10 * B00,  R11 = A10 * B01
//   Phase 2 (accumulate):   R00 += A01 * B10, R01 += A01 * B11
//                           R10 += A11 * B10, R11 += A11 * B11
//
// The two phases must serialize because they target the same output
// quadrants. Inside each phase the four sub-products are independent.
//
// This is libfork's published bench shape (`bench/source/matmul/libfork.cpp`):
// continuation-stealing pools that allow the parent task to migrate to a
// thief mid-recursion are tuned for this workload. citor's child-stealing
// `forkJoinAll(4, ...)` cannot replicate that exactly; the cell exists
// regardless because mirroring the libfork-canonical shape is the honest
// comparison.
//
// Pool eligibility: same as Strassen. Two sequential `recursiveSpawnN(pool, 4)`
// calls on the same Pool reference -- Taskflow Subflow's single-shot
// `subflow.join()` cannot compose with this shape, so Subflow is excluded
// (mirrors the documented Strassen exclusion).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "citor/always_assert.h"
#include "citor/hints.h"
#include "citor/thread_pool.h"

#include "aligned_alloc.h"
#include "bench_format.h"
#include "bench_registry.h"
#include "competitor_traits.h"
#include "cycle_clock.h"
#include "recursive_forkjoin_helper.h"

#ifdef CITOR_BENCH_HAS_TBB
#include <oneapi/tbb/task_arena.h>
#include <oneapi/tbb/task_group.h>
#endif

#include "libfork_runners.h"
#include "tmc_runners.h"

namespace citor::bench {
namespace {

constexpr std::size_t kIterations = 10;
constexpr std::size_t kWarmupIterations = 2;

/// Sub-matrix size below which the recursion drops into the naive ijk
/// matmul. 64 keeps the leaf inside L1 (a 64x64 float tile = 16 KiB,
/// each pair fits in 32 KiB L1).
constexpr std::size_t kSeqCutoff = 64;

struct AlignedFloatDeleter {
  void operator()(float *p) const noexcept { alignedFree(p); }
};

using AlignedFloatBuffer = std::unique_ptr<float, AlignedFloatDeleter>;

AlignedFloatBuffer allocateAlignedFloats(std::size_t count) {
  const std::size_t bytes = ((count * sizeof(float) + 63U) / 64U) * 64U;
  void *raw = alignedAlloc(bytes, 64U);
  if (raw == nullptr) {
    std::abort();
  }
  return AlignedFloatBuffer{static_cast<float *>(raw)};
}

void deterministicFill(float *p, std::size_t count,
                       std::uint32_t seed) noexcept {
  std::mt19937 rng{seed};
  std::uniform_real_distribution<float> dist(-1.0F, 1.0F);
  for (std::size_t i = 0; i < count; ++i) {
    p[i] = dist(rng);
  }
}

/// Leaf multiply: ijk triple loop on a strided sub-matrix. `add` selects
/// between overwrite (R = A*B) and accumulate (R += A*B), matching the two
/// recursion phases above.
inline void leafMultiply(const float *A, const float *B, float *R,
                         std::size_t n, std::size_t stride, bool add) noexcept {
  for (std::size_t i = 0; i < n; ++i) {
    float *rRow = R + (i * stride);
    if (!add) {
      for (std::size_t j = 0; j < n; ++j) {
        rRow[j] = 0.0F;
      }
    }
    for (std::size_t k = 0; k < n; ++k) {
      const float aik = A[(i * stride) + k];
      const float *bRow = B + (k * stride);
      for (std::size_t j = 0; j < n; ++j) {
        rRow[j] += aik * bRow[j];
      }
    }
  }
}

/// Recursive matmul body. Pool reference is passed through so each level's
/// `recursiveSpawnN` can dispatch to the correct primitive specialization.
template <class Pool>
void matmulRec(Pool &pool, const float *A, const float *B, float *R,
               std::size_t n, std::size_t stride, bool add) {
  if (n <= kSeqCutoff) {
    leafMultiply(A, B, R, n, stride, add);
    return;
  }
  const std::size_t m = n / 2U;
  const std::size_t o00 = 0;
  const std::size_t o01 = m;
  const std::size_t o10 = m * stride;
  const std::size_t o11 = (m * stride) + m;

  // Phase 1: overwrite. Four independent sub-products into disjoint quadrants.
  recursiveSpawnN(pool, std::size_t{4}, [&](Pool &p, std::size_t idx) {
    switch (idx) {
    case 0:
      matmulRec(p, A + o00, B + o00, R + o00, m, stride, add);
      break;
    case 1:
      matmulRec(p, A + o00, B + o01, R + o01, m, stride, add);
      break;
    case 2:
      matmulRec(p, A + o10, B + o00, R + o10, m, stride, add);
      break;
    case 3:
      matmulRec(p, A + o10, B + o01, R + o11, m, stride, add);
      break;
    default:
      break;
    }
  });

  // Phase 2: accumulate. Four sub-products ADDING to the same quadrants the
  // first phase wrote. The serialization between phases is enforced by the
  // recursiveSpawnN's join.
  recursiveSpawnN(pool, std::size_t{4}, [&](Pool &p, std::size_t idx) {
    switch (idx) {
    case 0:
      matmulRec(p, A + o01, B + o10, R + o00, m, stride, true);
      break;
    case 1:
      matmulRec(p, A + o01, B + o11, R + o01, m, stride, true);
      break;
    case 2:
      matmulRec(p, A + o11, B + o10, R + o10, m, stride, true);
      break;
    case 3:
      matmulRec(p, A + o11, B + o11, R + o11, m, stride, true);
      break;
    default:
      break;
    }
  });
}

template <class PoolT>
[[nodiscard]] BenchRow measureMatmulDac(const char *name,
                                        std::size_t participants, std::size_t n,
                                        const CyclesPerNanosecond &cal) {
  static_assert(RecursiveForkJoinTraits<PoolT>::supportsRecursiveSpawn,
                "matmul-dac bench requires recursive-spawn-capable pool");
  using Traits = CompetitorTraits<PoolT>;
  auto pool = Traits::make(participants);

  const AlignedFloatBuffer aBuf = allocateAlignedFloats(n * n);
  const AlignedFloatBuffer bBuf = allocateAlignedFloats(n * n);
  const AlignedFloatBuffer cBuf = allocateAlignedFloats(n * n);
  const AlignedFloatBuffer refBuf = allocateAlignedFloats(n * n);
  deterministicFill(aBuf.get(), n * n, 0xc1701U);
  deterministicFill(bBuf.get(), n * n, 0xc1701U + 1U);

  // Reference: naive ijk matmul on the full matrix (no recursion).
  leafMultiply(aBuf.get(), bBuf.get(), refBuf.get(), n, n, /*add=*/false);

  // Per-iter overwrite tolerance. Float32 ijk matmul on operands in [-1, 1]
  // accumulates roundoff at O(N * eps); the recursive form composes the
  // same operations in a different tree shape so the bound is comparable.
  // Use a generous tolerance scaled by N to catch indexing/quadrant bugs
  // without firing on legitimate fp32 noise.
  const float tolerance = 1e-3F * static_cast<float>(n);

  auto verify = [&]() {
    float maxDiff = 0.0F;
    const std::size_t total = n * n;
    for (std::size_t i = 0; i < total; ++i) {
      const float diff = std::fabs(cBuf.get()[i] - refBuf.get()[i]);
      maxDiff = std::max(diff, maxDiff);
    }
    CITOR_ALWAYS_ASSERT(maxDiff <= tolerance);
  };

  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    matmulRec(*pool, aBuf.get(), bBuf.get(), cBuf.get(), n, n, /*add=*/false);
    verify();
  }

  std::vector<double> samples;
  samples.reserve(kIterations);
  for (std::size_t i = 0; i < kIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    matmulRec(*pool, aBuf.get(), bBuf.get(), cBuf.get(), n, n, /*add=*/false);
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    verify();
  }
  return finalizeRow(name, samples);
}

[[nodiscard]] BenchRow measureCitor(std::size_t participants, std::size_t n,
                                    const CyclesPerNanosecond &cal) {
  return measureMatmulDac<citor::ThreadPool>("citor::ThreadPool", participants,
                                             n, cal);
}

#ifdef CITOR_BENCH_HAS_TBB
[[nodiscard]] BenchRow measureTbb(std::size_t participants, std::size_t n,
                                  const CyclesPerNanosecond &cal) {
  return measureMatmulDac<::tbb::task_arena>("oneTBB", participants, n, cal);
}
#endif

#ifdef CITOR_BENCH_HAS_DISPENSO
[[nodiscard]] BenchRow measureDispenso(std::size_t participants, std::size_t n,
                                       const CyclesPerNanosecond &cal) {
  static_assert(
      RecursiveForkJoinTraits<::dispenso::ThreadPool>::supportsRecursiveSpawn,
      "dispenso must opt into recursive spawn for matmul-dac");
  return measureMatmulDac<::dispenso::ThreadPool>("dispenso::ThreadPool",
                                                  participants, n, cal);
}
#endif

#ifdef CITOR_BENCH_HAS_OPENMP
[[nodiscard]] BenchRow measureOmp(std::size_t participants, std::size_t n,
                                  const CyclesPerNanosecond &cal) {
  static_assert(RecursiveForkJoinTraits<OpenMpRunner>::supportsRecursiveSpawn,
                "OpenMP runner must opt into recursive spawn for matmul-dac");
  OpenMpRunner runner{participants};

  const AlignedFloatBuffer aBuf = allocateAlignedFloats(n * n);
  const AlignedFloatBuffer bBuf = allocateAlignedFloats(n * n);
  const AlignedFloatBuffer cBuf = allocateAlignedFloats(n * n);
  const AlignedFloatBuffer refBuf = allocateAlignedFloats(n * n);
  deterministicFill(aBuf.get(), n * n, 0xc1701U);
  deterministicFill(bBuf.get(), n * n, 0xc1701U + 1U);
  leafMultiply(aBuf.get(), bBuf.get(), refBuf.get(), n, n, /*add=*/false);

  const float tolerance = 1e-3F * static_cast<float>(n);
  auto verify = [&]() {
    float maxDiff = 0.0F;
    const std::size_t total = n * n;
    for (std::size_t i = 0; i < total; ++i) {
      const float diff = std::fabs(cBuf.get()[i] - refBuf.get()[i]);
      maxDiff = std::max(diff, maxDiff);
    }
    CITOR_ALWAYS_ASSERT(maxDiff <= tolerance);
  };

  auto runOnce = [&]() {
#pragma omp parallel num_threads(static_cast<int>(participants))
    {
#pragma omp single
      {
        matmulRec(runner, aBuf.get(), bBuf.get(), cBuf.get(), n, n,
                  /*add=*/false);
      }
    }
  };

  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    runOnce();
    verify();
  }
  std::vector<double> samples;
  samples.reserve(kIterations);
  for (std::size_t i = 0; i < kIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    runOnce();
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    verify();
  }
  return finalizeRow("OpenMP", samples);
}
#endif

// Taskflow Subflow is not wired here. Same reason as the
// Strassen cell: matmulRec issues TWO sequential `recursiveSpawnN(pool, 4)`
// calls on the same Pool reference, but `tf::Subflow::join()` is single-shot
// per Subflow. The second wave silently produces a dead-Subflow with no
// scheduled tasks and the verify step fires.

struct MatmulCell {
  std::size_t n;
  const char *suffix;
};

constexpr std::array<MatmulCell, 2> kCells{{
    {.n = 1024U, .suffix = "n1024"},
    {.n = 2048U, .suffix = "n2048"},
}};

BenchTable buildTable(std::size_t participants, MatmulCell cell,
                      const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload = std::string{"forkjoin_matmul_dac_j"} +
                   std::to_string(participants) + "_" + cell.suffix;
  table.rows.push_back(measureCitor(participants, cell.n, cal));
#ifdef CITOR_BENCH_HAS_TBB
  table.rows.push_back(measureTbb(participants, cell.n, cal));
#endif
#ifdef CITOR_BENCH_HAS_OPENMP
  table.rows.push_back(measureOmp(participants, cell.n, cal));
#endif
#ifdef CITOR_BENCH_HAS_DISPENSO
  table.rows.push_back(measureDispenso(participants, cell.n, cal));
#endif
#ifdef CITOR_BENCH_HAS_LIBFORK
  table.rows.push_back(runLibforkMatmulDac(participants, cell.n, cal));
#endif
#ifdef CITOR_BENCH_HAS_TMC
  table.rows.push_back(runTmcMatmulDac(participants, cell.n, cal));
#endif
  return table;
}

template <std::size_t CellIdx, std::size_t Participants>
BenchTable runMatmulDacCell(const CyclesPerNanosecond &cal) {
  constexpr MatmulCell cell = kCells[CellIdx];
  return buildTable(Participants, cell, cal);
}

struct MatmulDacRegistrar {
  MatmulDacRegistrar() {
    registerWorkload({.name = "forkjoin_matmul_dac_j8_n1024",
                      .run = &runMatmulDacCell<0, 8>});
    registerWorkload({.name = "forkjoin_matmul_dac_j16_n1024",
                      .run = &runMatmulDacCell<0, 16>});
    registerWorkload({.name = "forkjoin_matmul_dac_j8_n2048",
                      .run = &runMatmulDacCell<1, 8>});
    registerWorkload({.name = "forkjoin_matmul_dac_j16_n2048",
                      .run = &runMatmulDacCell<1, 16>});
  }
};

const MatmulDacRegistrar kRegistrar;

} // namespace
} // namespace citor::bench
