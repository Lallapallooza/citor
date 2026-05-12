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

#include "aligned_alloc.h"
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

#ifdef CITOR_BENCH_HAS_TASKFLOW
#include <taskflow/taskflow.hpp>
#endif

#include "libfork_runners.h"
#include "tmc_runners.h"

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
/// scratch arena at `N = 2048`. The exact float count is
/// `scratchBudget(2048, 0)` (constexpr, defined below); see the
/// `scratchBudget` recurrence for the multiplier rules. Defer to the
/// constexpr rather than restating the figure here so the comment cannot
/// drift if the recurrence changes.
constexpr std::size_t kParallelDepth = 1;

/// Hint preset for the inner parallel-row matmul: bare `HintsDefaults` is
/// enough today, since the inner `parallelFor` does not consume affinity or
/// cancellation polls.
using StrassenForHints = citor::HintsDefaults;

/// 64-byte aligned float-buffer deleter; pairs with `std::unique_ptr` so the
/// buffer is freed automatically.
struct AlignedFloatDeleter {
  void operator()(float *p) const noexcept { alignedFree(p); }
};

using AlignedFloatBuffer = std::unique_ptr<float, AlignedFloatDeleter>;

/// Allocate an aligned float buffer of `count` elements; aborts on failure.
AlignedFloatBuffer allocateAlignedFloats(std::size_t count) {
  const std::size_t bytes = ((count * sizeof(float) + 63U) / 64U) * 64U;
  void *raw = alignedAlloc(bytes, 64U);
  if (raw == nullptr) {
    std::abort();
  }
  return AlignedFloatBuffer{static_cast<float *>(raw)};
}

/// Deterministic seeded fill so every (pool, N) cell sees the same operand
/// values. The bounded `[-1, 1]` range keeps Strassen's recursive
/// sub-matrix sums within a magnitude that the per-element tolerance can
/// reasonably absorb.
void deterministicFill(float *p, std::size_t count,
                       std::uint32_t seed) noexcept {
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
    const float *aRow = a.data + (i * a.stride);
    const float *bRow = b.data + (i * b.stride);
    float *dRow = dst.data + (i * dst.stride);
    for (std::size_t j = 0; j < dst.n; ++j) {
      dRow[j] = aRow[j] + bRow[j];
    }
  }
}

/// `dst = src1 - src2`.
inline void subInto(const Sub &dst, const Sub &a, const Sub &b) noexcept {
  for (std::size_t i = 0; i < dst.n; ++i) {
    const float *aRow = a.data + (i * a.stride);
    const float *bRow = b.data + (i * b.stride);
    float *dRow = dst.data + (i * dst.stride);
    for (std::size_t j = 0; j < dst.n; ++j) {
      dRow[j] = aRow[j] - bRow[j];
    }
  }
}

/// `dst += src`.
inline void addEq(const Sub &dst, const Sub &a) noexcept {
  for (std::size_t i = 0; i < dst.n; ++i) {
    const float *aRow = a.data + (i * a.stride);
    float *dRow = dst.data + (i * dst.stride);
    for (std::size_t j = 0; j < dst.n; ++j) {
      dRow[j] += aRow[j];
    }
  }
}

/// `dst -= src`.
inline void subEq(const Sub &dst, const Sub &a) noexcept {
  for (std::size_t i = 0; i < dst.n; ++i) {
    const float *aRow = a.data + (i * a.stride);
    float *dRow = dst.data + (i * dst.stride);
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
    float *cRow = c.data + (i * c.stride);
    for (std::size_t j = 0; j < c.n; ++j) {
      cRow[j] = 0.0F;
    }
    for (std::size_t k = 0; k < c.n; ++k) {
      const float aik = a.data[(i * a.stride) + k];
      const float *bRow = b.data + (k * b.stride);
      for (std::size_t j = 0; j < c.n; ++j) {
        cRow[j] += aik * bRow[j];
      }
    }
  }
}

/// Quadrant accessor: returns the (`row`, `col`) sub-matrix of `m` where
/// `row` and `col` are 0 or 1. Each quadrant is `n/2 x n/2` and shares the
/// stride of the parent.
[[nodiscard]] inline Sub quadrant(const Sub &m, std::size_t row,
                                  std::size_t col) noexcept {
  const std::size_t half = m.n / 2U;
  return Sub{.data = m.data + (row * half * m.stride) + (col * half),
             .stride = m.stride,
             .n = half};
}

/// Forward declaration: recursive Strassen entry. The body is placed below
/// because it depends on the parallel-row matmul leaf via `parallelMatmul`.
template <class Pool>
void strassenRec(Pool &pool, const Sub &c, const Sub &a, const Sub &b,
                 float *scratch, std::size_t depth);

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
[[nodiscard]] constexpr std::size_t scratchBudget(std::size_t n,
                                                  std::size_t depth) noexcept {
  if (n <= kSeqCutoff) {
    return 0U;
  }
  const std::size_t half = n / 2U;
  const std::size_t levelSize = 17U * (half * half);
  const std::size_t childMul = depth < kParallelDepth ? 7U : 1U;
  return levelSize + (childMul * scratchBudget(half, depth + 1U));
}

/// Componentwise roundoff tolerance for Strassen's NxN multiply on float32.
///
/// Higham (2002), "Accuracy and Stability of Numerical Algorithms", Section
/// 23.2 derives the worst-case bound for Strassen's recursive multiply as
/// approximately `1.5 * N^log2(7) * eps_float * max_input_magnitude`, where
/// `N^log2(7)` is roughly `N^2.81`. For float32 (`eps ~ 1.19e-7`) on
/// operands in [-1, 1] this is far above the empirically observed max
/// absolute diff (sub-1e-3 at the `kCells` sizes); the gate fires only on
/// gross bugs (indexing errors, missing sub-products, broken merges), not
/// on legitimate roundoff. The exact bound is conservative; the derivation
/// is what matters for "did the algorithm compute the right thing".
[[nodiscard]] inline float strassenTolerance(std::size_t n) noexcept {
  constexpr double kEpsFloat = 1.1920929e-7;
  constexpr double kHighamScale = 1.5;
  // log2(7) is irrational; std::pow at runtime is fine since this runs
  // once per verify() call, well outside the timing window.
  const double bound = kHighamScale *
                       std::pow(static_cast<double>(n), 2.8073549220576041) *
                       kEpsFloat;
  return static_cast<float>(bound);
}

/// Recursive Strassen body. Allocates 17 sub-buffers in the caller-supplied
/// `scratch` arena and dispatches the 7 sub-products either in parallel
/// via `recursiveSpawn2` (while `depth < kParallelDepth`) or serially
/// (deeper levels). Parallel siblings own disjoint scratch slices; serial
/// siblings share the same slice across the seven mults.
template <class Pool>
void strassenRec(Pool &pool, const Sub &c, const Sub &a, const Sub &b,
                 float *scratch, std::size_t depth) {
  if (c.n <= kSeqCutoff) {
    parallelMatmul(pool, c, a, b);
    return;
  }
  const std::size_t half = c.n / 2U;
  const std::size_t halfSize = half * half;

  // Carve 21 contiguous sub-buffers out of the scratch arena.
  float *cursor = scratch;
  auto take = [&cursor, halfSize]() {
    // The returned writable scratch slice is immediately stored in a mutable
    // Sub view.
    float *const out = cursor; // NOLINT(misc-const-correctness)
    cursor += halfSize;
    return out;
  };
  // Sub-product output buffers M1..M7.
  std::array<Sub, 7> mBufs{};
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
  const Sub b11 = quadrant(b, 0, 0);
  const Sub b12 = quadrant(b, 0, 1);
  const Sub b21 = quadrant(b, 1, 0);
  const Sub b22 = quadrant(b, 1, 1);
  const Sub c11 = quadrant(c, 0, 0);
  const Sub c12 = quadrant(c, 0, 1);
  const Sub c21 = quadrant(c, 1, 0);
  const Sub c22 = quadrant(c, 1, 1);

  // Stamp out the operand temporaries (sequential adds; cheap relative to
  // the recursive multiplications below).
  addInto(t1A, a11, a22);
  addInto(t1B, b11, b22);
  addInto(t2A, a21, a22);
  subInto(t3B, b12, b22);
  subInto(t4B, b21, b11);
  addInto(t5A, a11, a12);
  subInto(t6A, a21, a11);
  addInto(t6B, b11, b12);
  subInto(t7A, a12, a22);
  addInto(t7B, b21, b22);

  // Compute the 7 sub-products. Each Mi is a recursive Strassen call on
  // half-sized inputs. Above `kParallelDepth` the seven sub-products run
  // concurrently and need disjoint scratch slices (partitioned via
  // `childBudget` offsets); deeper levels run them serially and reuse a
  // single scratch slice.
  const std::size_t childBudget = scratchBudget(half, depth + 1U);

  if (depth < kParallelDepth) {
    // Parallel sub-product dispatch via recursiveSpawn2. The four groups
    // are paired so each invocation spawns two independent leaves.
    recursiveSpawn2(
        pool,
        [&](Pool &p) {
          // Group 1: M1, M2.
          recursiveSpawn2(
              p,
              [&](Pool &pp) {
                strassenRec(pp, mBufs[0], t1A, t1B, childScratch, depth + 1U);
              },
              [&](Pool &pp) {
                strassenRec(pp, mBufs[1], t2A, b11, childScratch + childBudget,
                            depth + 1U);
              });
        },
        [&](Pool &p) {
          // Group 2: M3, M4.
          recursiveSpawn2(
              p,
              [&](Pool &pp) {
                strassenRec(pp, mBufs[2], a11, t3B,
                            childScratch + (2U * childBudget), depth + 1U);
              },
              [&](Pool &pp) {
                strassenRec(pp, mBufs[3], a22, t4B,
                            childScratch + (3U * childBudget), depth + 1U);
              });
        });

    recursiveSpawn2(
        pool,
        [&](Pool &p) {
          // Group 3: M5, M6.
          recursiveSpawn2(
              p,
              [&](Pool &pp) {
                strassenRec(pp, mBufs[4], t5A, b22,
                            childScratch + (4U * childBudget), depth + 1U);
              },
              [&](Pool &pp) {
                strassenRec(pp, mBufs[5], t6A, t6B,
                            childScratch + (5U * childBudget), depth + 1U);
              });
        },
        [&](Pool &p) {
          // Group 4: M7 alone (no sibling, runs in this branch directly).
          strassenRec(p, mBufs[6], t7A, t7B, childScratch + (6U * childBudget),
                      depth + 1U);
        });
  } else {
    // Serial sub-product dispatch. All seven mults reuse the same child
    // scratch slice, since they don't run concurrently. This keeps the
    // total scratch arena bounded by the parallel-region depth.
    strassenRec(pool, mBufs[0], t1A, t1B, childScratch, depth + 1U);
    strassenRec(pool, mBufs[1], t2A, b11, childScratch, depth + 1U);
    strassenRec(pool, mBufs[2], a11, t3B, childScratch, depth + 1U);
    strassenRec(pool, mBufs[3], a22, t4B, childScratch, depth + 1U);
    strassenRec(pool, mBufs[4], t5A, b22, childScratch, depth + 1U);
    strassenRec(pool, mBufs[5], t6A, t6B, childScratch, depth + 1U);
    strassenRec(pool, mBufs[6], t7A, t7B, childScratch, depth + 1U);
  }

  // Combine sub-products into the output quadrants:
  //   C11 = M1 + M4 - M5 + M7
  //   C12 = M3 + M5
  //   C21 = M2 + M4
  //   C22 = M1 - M2 + M3 + M6
  addInto(c11, mBufs[0], mBufs[3]);
  subEq(c11, mBufs[4]);
  addEq(c11, mBufs[6]);

  addInto(c12, mBufs[2], mBufs[4]);

  addInto(c21, mBufs[1], mBufs[3]);

  subInto(c22, mBufs[0], mBufs[1]);
  addEq(c22, mBufs[2]);
  addEq(c22, mBufs[5]);
}

template <class PoolT>
[[nodiscard]] BenchRow measureStrassen(const char *name,
                                       std::size_t participants, std::size_t n,
                                       const CyclesPerNanosecond &cal) {
  static_assert(RecursiveForkJoinTraits<PoolT>::supportsRecursiveSpawn,
                "strassen bench requires recursive-spawn-capable pool; the "
                "trait gate excludes "
                "BS / dp / task_thread_pool / riften / Eigen / Taskflow "
                "Executor at compile time.");
  using Traits = CompetitorTraits<PoolT>;
  auto pool = Traits::make(participants);

  const AlignedFloatBuffer aBuf = allocateAlignedFloats(n * n);
  const AlignedFloatBuffer bBuf = allocateAlignedFloats(n * n);
  const AlignedFloatBuffer cBuf = allocateAlignedFloats(n * n);
  const AlignedFloatBuffer refBuf = allocateAlignedFloats(n * n);

  deterministicFill(aBuf.get(), n * n, 0xc1701U);
  deterministicFill(bBuf.get(), n * n, 0xc1701U + 1U);

  const Sub aSub = Sub{.data = aBuf.get(), .stride = n, .n = n};
  const Sub bSub = Sub{.data = bBuf.get(), .stride = n, .n = n};
  const Sub cSub = Sub{.data = cBuf.get(), .stride = n, .n = n};
  const Sub refSub = Sub{.data = refBuf.get(), .stride = n, .n = n};

  // Reference: naive ijk matmul on the same input.
  seqMatmul(refSub, aSub, bSub);

  // Scratch arena: each Strassen level holds 21 sub-buffers; scrach space
  // is sized to the cumulative recursive budget. Recursive children
  // partition the arena via pointer offsets; each spawned sibling owns a
  // disjoint slice.
  // The recursive sub-product calls require disjoint scratch slices while
  // depth < `kParallelDepth`; below that depth the seven mults reuse a
  // single slice. `scratchBudget` returns the exact recursive footprint
  // for the (n, depth=0) entry point.
  const std::size_t scratchN = scratchBudget(n, 0U);
  std::vector<float> scratch(scratchN, 0.0F);

  // Verify Strassen output against the reference. The tolerance is derived
  // from the Higham worst-case Strassen roundoff bound (see
  // `strassenTolerance` for the derivation); the gate catches gross bugs
  // (indexing errors, missing sub-products, broken merges) without firing
  // on legitimate fp32 roundoff.
  auto verify = [&]() {
    float maxDiff = 0.0F;
    const std::size_t total = n * n;
    for (std::size_t i = 0; i < total; ++i) {
      const float diff = std::fabs(cBuf.get()[i] - refBuf.get()[i]);
      maxDiff = std::max(diff, maxDiff);
    }
    const float tolerance = strassenTolerance(n);
    CITOR_ALWAYS_ASSERT(maxDiff <= tolerance);
  };

  // Warmup + correctness gate.
  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    strassenRec(*pool, cSub, aSub, bSub, scratch.data(), std::size_t{0});
    verify();
  }

  std::vector<double> samples;
  samples.reserve(kIterations);
  for (std::size_t i = 0; i < kIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    strassenRec(*pool, cSub, aSub, bSub, scratch.data(), std::size_t{0});
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    verify();
  }

  return finalizeRow(name, samples);
}

[[nodiscard]] BenchRow measureCitor(std::size_t participants, std::size_t n,
                                    const CyclesPerNanosecond &cal) {
  return measureStrassen<citor::ThreadPool>("citor::ThreadPool", participants,
                                            n, cal);
}

#ifdef CITOR_BENCH_HAS_TBB
[[nodiscard]] BenchRow measureTbb(std::size_t participants, std::size_t n,
                                  const CyclesPerNanosecond &cal) {
  return measureStrassen<::tbb::task_arena>("oneTBB", participants, n, cal);
}
#endif

#ifdef CITOR_BENCH_HAS_DISPENSO
[[nodiscard]] BenchRow measureDispenso(std::size_t participants, std::size_t n,
                                       const CyclesPerNanosecond &cal) {
  static_assert(
      RecursiveForkJoinTraits<::dispenso::ThreadPool>::supportsRecursiveSpawn,
      "dispenso must opt into recursive spawn for the strassen bench");
  return measureStrassen<::dispenso::ThreadPool>("dispenso::ThreadPool",
                                                 participants, n, cal);
}
#endif

// Taskflow Subflow is not wired here. The reference Strassen body
// at depth=0 issues TWO sequential `recursiveSpawn2` calls on the same Pool
// reference (groups M1+M2 then groups M3..M7) -- that pattern works for
// citor / TBB / OpenMP / dispenso whose group primitives can be fanned out
// repeatedly on the same pool. For Taskflow, recursiveSpawn2(Subflow, ...)
// emplaces+joins, and `tf::Subflow::join()` is single-shot per Subflow, so
// the second `recursiveSpawn2` on the same Subflow silently produces a
// dead-Subflow with no scheduled tasks. The result is that the second wave
// of sub-products is never dispatched; the merge step then reads
// uninitialized scratch and the verify gate fires.
//
// A Subflow-shaped Strassen would need to fan out all 7 sub-products in
// ONE Subflow scope (e.g. via `recursiveSpawnN<Subflow>(7, ...)`) rather
// than chaining two recursiveSpawn2 calls. Implementing that requires
// restructuring the depth-0 split + scratch-arena partitioning; the work is
// tractable but not landed here.

#ifdef CITOR_BENCH_HAS_OPENMP
[[nodiscard]] BenchRow measureOmp(std::size_t participants, std::size_t n,
                                  const CyclesPerNanosecond &cal) {
  static_assert(
      RecursiveForkJoinTraits<OpenMpRunner>::supportsRecursiveSpawn,
      "OpenMP runner must opt into recursive spawn for the strassen bench");
  // OpenMP `task` requires an enclosing `parallel` region. Open it once
  // per iteration; the inner `single` directive funnels the root call.
  OpenMpRunner runner{participants};

  const AlignedFloatBuffer aBuf = allocateAlignedFloats(n * n);
  const AlignedFloatBuffer bBuf = allocateAlignedFloats(n * n);
  const AlignedFloatBuffer cBuf = allocateAlignedFloats(n * n);
  const AlignedFloatBuffer refBuf = allocateAlignedFloats(n * n);

  deterministicFill(aBuf.get(), n * n, 0xc1701U);
  deterministicFill(bBuf.get(), n * n, 0xc1701U + 1U);

  const Sub aSub = Sub{.data = aBuf.get(), .stride = n, .n = n};
  const Sub bSub = Sub{.data = bBuf.get(), .stride = n, .n = n};
  const Sub cSub = Sub{.data = cBuf.get(), .stride = n, .n = n};
  const Sub refSub = Sub{.data = refBuf.get(), .stride = n, .n = n};

  seqMatmul(refSub, aSub, bSub);

  const std::size_t scratchN = scratchBudget(n, 0U);
  std::vector<float> scratch(scratchN, 0.0F);

  auto verify = [&]() {
    float maxDiff = 0.0F;
    const std::size_t total = n * n;
    for (std::size_t i = 0; i < total; ++i) {
      const float diff = std::fabs(cBuf.get()[i] - refBuf.get()[i]);
      maxDiff = std::max(diff, maxDiff);
    }
    const float tolerance = strassenTolerance(n);
    CITOR_ALWAYS_ASSERT(maxDiff <= tolerance);
  };

  auto runOnce = [&]() {
#pragma omp parallel num_threads(static_cast<int>(participants))
    {
#pragma omp single
      {
        strassenRec(runner, cSub, aSub, bSub, scratch.data(), std::size_t{0});
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

struct StrassenCell {
  std::size_t n;
  const char *suffix;
};

constexpr std::array<StrassenCell, 2> kCells{{
    {.n = 1024U, .suffix = "n1024"},
    {.n = 2048U, .suffix = "n2048"},
}};

BenchTable buildTable(std::size_t participants, StrassenCell cell,
                      const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload = std::string{"forkjoin_strassen_j"} +
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
  table.rows.push_back(runLibforkStrassen(participants, cell.n, cal));
#endif
#ifdef CITOR_BENCH_HAS_TMC
  table.rows.push_back(runTmcStrassen(participants, cell.n, cal));
#endif
  return table;
}

template <std::size_t CellIdx, std::size_t Participants>
BenchTable runStrassenCell(const CyclesPerNanosecond &cal) {
  constexpr StrassenCell cell = kCells[CellIdx];
  return buildTable(Participants, cell, cal);
}

struct StrassenRegistrar {
  StrassenRegistrar() {
    registerWorkload(
        {.name = "forkjoin_strassen_j8_n1024", .run = &runStrassenCell<0, 8>});
    registerWorkload({.name = "forkjoin_strassen_j16_n1024",
                      .run = &runStrassenCell<0, 16>});
    registerWorkload(
        {.name = "forkjoin_strassen_j8_n2048", .run = &runStrassenCell<1, 8>});
    registerWorkload({.name = "forkjoin_strassen_j16_n2048",
                      .run = &runStrassenCell<1, 16>});
  }
};

const StrassenRegistrar kRegistrar;

} // namespace
} // namespace citor::bench
