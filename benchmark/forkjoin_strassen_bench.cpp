// Recursive 7-multiply Strassen matrix multiplication for the comparative
// pool bench.
//
// `C = A * B` over `N x N` float matrices with the standard Strassen
// formulation: split each operand into 2x2 block matrices, compute seven
// sub-products via the algebraic identities below, combine them into the
// four output quadrants. The seven sub-products are independent and can
// run in parallel; the helper composes `recursiveSpawn2` (the binary
// fork-join sibling) over the sub-product groups.
//
// Sub-product groups (paired so each group contains two independent mults
// dispatched via one `recursiveSpawn2` invocation):
//   group 1: M1, M2  spawned in parallel, joined.
//   group 2: M3, M4  spawned in parallel, joined.
//   group 3: M5, M6  spawned in parallel, joined.
//   group 4: M7      runs alone (odd man out).
// At depth=0 the implementation issues TWO sequential `recursiveSpawn2`
// invocations: the first dispatches groups 1+2 (peak 4-way concurrency) and
// joins; the second dispatches groups 3+4 (peak 3-way concurrency, since M7
// has no sibling) and joins. Deeper levels serialize at kParallelDepth=1 to
// bound the scratch arena. Inner block matmul (sub-array `N <= kSeqCutoff`)
// drops into the seq ikj kernel.
//
// Pool eligibility: only pools whose
// `RecursiveForkJoinTraits::supportsRecursiveSpawn` is `true` participate.
// Compile-time gating mirrors the cilksort bench (and the helper's
// `static_assert` enforces the substitution).
//
// Internal correctness gate (BEFORE timing): the Strassen output is
// compared against a reference produced by the naive ijk matmul on the
// same input; the per-element absolute difference must be below
// `1e-3 * N` (Strassen has known numerical instability and the tolerance
// scales with the cumulative error introduced by recursive
// add/subtract steps). `CITOR_ALWAYS_ASSERT` aborts on mismatch.

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

#include "bench_format.h"
#include "bench_registry.h"
#include "competitor_traits.h"
#include "cycle_clock.h"
#include "recursive_forkjoin_helper.h"

#ifdef CITOR_BENCH_HAS_TBB
#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/task_arena.h>
#include <oneapi/tbb/task_group.h>
#endif

namespace citor::bench {
namespace {

/// Per-cell sample budget. Strassen at N = 1024 is tens of milliseconds per
/// iteration; N = 2048 is a few hundred. 10 samples keeps a row under a
/// few seconds wall.
constexpr std::size_t kIterations = 10;
constexpr std::size_t kWarmupIterations = 2;

/// Sub-matrix size below which the recursion drops into the naive ijk
/// matmul (parallelized over rows via `parallelFor`). 64 is the canonical
/// Strassen cutoff -- below that, the recursion's add/subtract overhead
/// dominates the multiplicative savings.
constexpr std::size_t kSeqCutoff = 64;

/// Depth above which the seven Strassen sub-products are dispatched in
/// parallel via `recursiveSpawn2`. Below this depth the recursion runs
/// sequentially: the 17 operand / sub-product temporaries are reused
/// across the seven mults, so the scratch arena footprint stays bounded.
/// Top-level parallelism is sufficient to expose seven concurrent leaves
/// -- enough to saturate j={8, 16} participants without blowing the
/// scratch arena at `N = 2048` (~234 MiB of floats at depth=1;
/// scratchBudget(2048,0) = 17M + 7*scratchBudget(1024,1) ~= 58.4M floats).
constexpr std::size_t kParallelDepth = 1;

/// Hint preset for the inner parallel-row matmul.
struct StrassenForHints {
  static constexpr citor::Balance balance = citor::Balance::StaticUniform;
  static constexpr citor::Determinism determinism = citor::Determinism::FixedBlockOrder;
  static constexpr citor::Affinity affinity = citor::Affinity::PhysicalCores;
  static constexpr citor::Priority priority = citor::Priority::Throughput;
  static constexpr citor::Partition partition = citor::Partition::ContiguousRanges;
  static constexpr double estimatedItemNs = 0.0;
  static constexpr double minTaskUs = 0.0;
  static constexpr std::size_t chunk = 0;
  static constexpr bool tlsRequired = false;
  static constexpr bool allowProducer = true;
  static constexpr bool allowWorkerSteal = false;
  static constexpr bool allowNestedParallelism = false;
  static constexpr bool fpDeterministicTree = true;
  static constexpr bool cancellationChecks = false;
  static constexpr bool pipelineSameChunk = false;
};

/// 64-byte aligned `float[]` deleter; pairs with `std::unique_ptr` so the
/// buffer is freed automatically.
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

/// Deterministic seeded fill so every (pool, N) cell sees the same operand
/// values. The bounded `[-1, 1]` range keeps Strassen's recursive
/// sub-matrix sums within a magnitude that the per-element tolerance can
/// reasonably absorb.
void deterministicFill(float *p, std::size_t count, std::uint32_t seed) noexcept {
  std::mt19937 rng{seed};
  std::uniform_real_distribution<float> dist(-1.0F, 1.0F);
  for (std::size_t i = 0; i < count; ++i) {
    p[i] = dist(rng);
  }
}

/// Strided sub-matrix view: a logical `n x n` matrix backed by a base
/// pointer and a row stride. Strassen splits each operand into 2x2 block
/// quadrants; each quadrant is a `Sub` view referencing the same backing
/// buffer with a smaller `n` and the original stride.
struct Sub {
  float *data;
  std::size_t stride;
  std::size_t n;
};

/// `dst = src1 + src2`, all `n x n` strided. Sequential; the add/subtract
/// helpers are the per-recursion-level constant overhead Strassen pays.
inline void addInto(const Sub &dst, const Sub &a, const Sub &b) noexcept {
  for (std::size_t i = 0; i < dst.n; ++i) {
    const float *aRow = a.data + i * a.stride;
    const float *bRow = b.data + i * b.stride;
    float *dRow = dst.data + i * dst.stride;
    for (std::size_t j = 0; j < dst.n; ++j) {
      dRow[j] = aRow[j] + bRow[j];
    }
  }
}

/// `dst = src1 - src2`.
inline void subInto(const Sub &dst, const Sub &a, const Sub &b) noexcept {
  for (std::size_t i = 0; i < dst.n; ++i) {
    const float *aRow = a.data + i * a.stride;
    const float *bRow = b.data + i * b.stride;
    float *dRow = dst.data + i * dst.stride;
    for (std::size_t j = 0; j < dst.n; ++j) {
      dRow[j] = aRow[j] - bRow[j];
    }
  }
}

/// `dst += src`.
inline void addEq(const Sub &dst, const Sub &a) noexcept {
  for (std::size_t i = 0; i < dst.n; ++i) {
    const float *aRow = a.data + i * a.stride;
    float *dRow = dst.data + i * dst.stride;
    for (std::size_t j = 0; j < dst.n; ++j) {
      dRow[j] += aRow[j];
    }
  }
}

/// `dst -= src`.
inline void subEq(const Sub &dst, const Sub &a) noexcept {
  for (std::size_t i = 0; i < dst.n; ++i) {
    const float *aRow = a.data + i * a.stride;
    float *dRow = dst.data + i * dst.stride;
    for (std::size_t j = 0; j < dst.n; ++j) {
      dRow[j] -= aRow[j];
    }
  }
}

/// Sequential ijk matmul `c = a * b`, all `n x n` strided. Used at the
/// recursion leaf when a sub-matrix is below `kSeqCutoff`. Output is
/// initialized to zero before the accumulating loop.
inline void seqMatmul(const Sub &c, const Sub &a, const Sub &b) noexcept {
  for (std::size_t i = 0; i < c.n; ++i) {
    float *cRow = c.data + i * c.stride;
    for (std::size_t j = 0; j < c.n; ++j) {
      cRow[j] = 0.0F;
    }
    for (std::size_t k = 0; k < c.n; ++k) {
      const float aik = a.data[i * a.stride + k];
      const float *bRow = b.data + k * b.stride;
      for (std::size_t j = 0; j < c.n; ++j) {
        cRow[j] += aik * bRow[j];
      }
    }
  }
}

/// Quadrant accessor: returns the (`row`, `col`) sub-matrix of `m` where
/// `row` and `col` are 0 or 1. Each quadrant is `n/2 x n/2` and shares the
/// stride of the parent.
[[nodiscard]] inline Sub quadrant(const Sub &m, std::size_t row, std::size_t col) noexcept {
  const std::size_t half = m.n / 2U;
  return Sub{
      .data = m.data + (row * half * m.stride) + (col * half), .stride = m.stride, .n = half};
}

/// Forward declaration: recursive Strassen entry. The body is placed below
/// because it depends on the parallel-row matmul leaf via `parallelMatmul`.
template <class Pool>
void strassenRec(Pool &pool, const Sub &c, const Sub &a, const Sub &b, float *scratch,
                 std::size_t depth);

/// Parallel-row matmul leaf: at the recursion's `kSeqCutoff` boundary the
/// 64x64 (or smaller) matmul runs serially because dispatching a
/// parallel-for at this size is dispatch-bound. The leaf uses `seqMatmul`
/// directly.
template <class Pool>
void parallelMatmul(Pool & /*pool*/, const Sub &c, const Sub &a, const Sub &b) {
  seqMatmul(c, a, b);
}

/// Compute the scratch-buffer requirement for one Strassen recursion at
/// size `n` and depth `depth`. While `depth < kParallelDepth`, the seven
/// sub-products run concurrently and therefore need disjoint scratch
/// slices: `scratchBudget(n, depth) = 17 * (n/2)^2 + 7 *
/// scratchBudget(n/2, depth + 1)`. Once `depth >= kParallelDepth`, the
/// sub-products run serially and reuse the same 17 sub-buffers; the
/// recurrence becomes `scratchBudget(n, depth) = 17 * (n/2)^2 +
/// scratchBudget(n/2, depth + 1)`. Both branches terminate at
/// `n <= kSeqCutoff` where the leaf seq-matmul needs no scratch.
[[nodiscard]] constexpr std::size_t scratchBudget(std::size_t n, std::size_t depth) noexcept {
  if (n <= kSeqCutoff) {
    return 0U;
  }
  const std::size_t half = n / 2U;
  const std::size_t levelSize = 17U * (half * half);
  const std::size_t childMul = depth < kParallelDepth ? 7U : 1U;
  return levelSize + childMul * scratchBudget(half, depth + 1U);
}

/// Recursive Strassen body. Allocates 17 sub-buffers in the caller-supplied
/// `scratch` arena and dispatches the 7 sub-products either in parallel
/// via `recursiveSpawn2` (while `depth < kParallelDepth`) or serially
/// (deeper levels). Parallel siblings own disjoint scratch slices; serial
/// siblings share the same slice across the seven mults.
template <class Pool>
void strassenRec(Pool &pool, const Sub &c, const Sub &a, const Sub &b, float *scratch,
                 std::size_t depth) {
  if (c.n <= kSeqCutoff) {
    parallelMatmul(pool, c, a, b);
    return;
  }
  const std::size_t half = c.n / 2U;
  const std::size_t halfSize = half * half;

  // Carve 21 contiguous sub-buffers out of the scratch arena.
  float *cursor = scratch;
  auto take = [&cursor, halfSize]() {
    float *out = cursor;
    cursor += halfSize;
    return out;
  };
  // Sub-product output buffers M1..M7.
  Sub mBufs[7];
  for (std::size_t k = 0; k < 7; ++k) {
    mBufs[k] = Sub{.data = take(), .stride = half, .n = half};
  }
  // Operand temporaries: each Mi has up to two operand sub-expressions.
  // Layout follows the algebraic identities below.
  Sub t1A = Sub{.data = take(), .stride = half, .n = half}; // A11 + A22
  Sub t1B = Sub{.data = take(), .stride = half, .n = half}; // B11 + B22
  Sub t2A = Sub{.data = take(), .stride = half, .n = half}; // A21 + A22
  Sub t3B = Sub{.data = take(), .stride = half, .n = half}; // B12 - B22
  Sub t4B = Sub{.data = take(), .stride = half, .n = half}; // B21 - B11
  Sub t5A = Sub{.data = take(), .stride = half, .n = half}; // A11 + A12
  Sub t6A = Sub{.data = take(), .stride = half, .n = half}; // A21 - A11
  Sub t6B = Sub{.data = take(), .stride = half, .n = half}; // B11 + B12
  Sub t7A = Sub{.data = take(), .stride = half, .n = half}; // A12 - A22
  Sub t7B = Sub{.data = take(), .stride = half, .n = half}; // B21 + B22
  // 4 unused slots remain in the 14-temporary budget (sub-products M2 and
  // M3 reuse single-operand inputs A11, A22 directly, so no temporary is
  // needed there). The cursor advanced past 17 sub-buffers; the next
  // recursion level starts where the cursor stops.
  float *childScratch = cursor;

  // Quadrant sub-views of A, B, C.
  const Sub a11 = quadrant(a, 0, 0);
  const Sub a12 = quadrant(a, 0, 1);
  const Sub a21 = quadrant(a, 1, 0);
  const Sub a22 = quadrant(a, 1, 1);

} // namespace
} // namespace citor::bench
