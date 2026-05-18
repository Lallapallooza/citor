// Recursive cilksort workload for the comparative pool bench.
//
// Sort phase fans out via forkJoin recursion. Root-level merge fans out via
// parallelScan + parallelFor (now with kMergeBuckets > kScanInlineThreshold so
// the scan actually dispatches). Sub-tree merges run sequentially because
// parallelScan / parallelFor invoked from inside a forkJoin worker fall through
// to inline-on-caller per the same-pool reentrancy guard.
//
// Primitive surface:
//   - `recursiveSpawn2` (forkJoin) drives the divide-and-conquer sort phase.
//   - `parallelScan` computes the merge-phase bucket-offset prefix.
//   - `parallelFor` writes per-bucket merged output.
//
// Sort phase: split the array in half, recursively cilksort each half via
// `recursiveSpawn2(left_sort, right_sort)` until the sub-array is below the
// `kSeqCutoff` threshold; below that, drop into `std::sort`.
//
// Merge phase: given two sorted halves of length `nA + nB`, choose `K`
// evenly-spaced splitter positions in the larger half, binary-search each
// splitter's key in the smaller half to derive bucket boundaries, run
// `parallelScan` over the bucket-size array to compute the inclusive
// output-offset prefix, and run `parallelFor` to merge each bucket
// independently into the destination.
//
// Pool eligibility: only pools whose
// `RecursiveForkJoinTraits::supportsRecursiveSpawn` is `true` participate.
// citor::ThreadPool, oneTBB, and OpenMP all qualify; BS / dp / task / riften /
// Eigen / Taskflow Executor are excluded at compile time via the helper's
// `static_assert`.
//
// Internal correctness gate (BEFORE timing): the cilksort output is compared
// element-wise against `std::sort` on a fresh copy of the same input;
// `CITOR_ALWAYS_ASSERT` aborts on mismatch.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
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
#include <oneapi/tbb/parallel_scan.h>
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

/// Per-cell sample budget. cilksort at n = 1M is a few milliseconds per
/// iteration on this host; n = 16M is tens of milliseconds. 25 samples keeps
/// a row under a few seconds wall.
constexpr std::size_t kIterations = 25;
constexpr std::size_t kWarmupIterations = 3;

/// Sub-array size below which the recursion drops into `std::sort`.
// kSeqCutoff between Cilk-5 canon (~100) and a value where std::sort overhead
// amortizes; 256 produces ~64K leaf invocations at n=16M.
constexpr std::size_t kSeqCutoff = 256;

/// Splitter count for the parallel merge. With 64 buckets the parallelFor has
/// enough work to amortize dispatch even at the smaller cell, and the scan
/// over a 64-entry array clears the engine's inline-fallback gate.
// kMergeBuckets must exceed thread_pool.h's kScanInlineThreshold (32) so the
// merge phase's parallelScan fans out instead of inline-capping.
constexpr std::size_t kMergeBuckets = 64;

/// Hint preset for the merge-phase `parallelFor`: cancellation polls disabled.
/// The merge-phase `parallelScan` uses bare `citor::HintsDefaults`.
struct CilksortForHints : citor::HintsDefaults {
  static constexpr bool cancellationChecks = false;
};

/// Build a deterministic input array of the requested length. Seeded with
/// `0xc1701` per the spec so every (pool, n) cell sees byte-identical input.
[[nodiscard]] std::vector<std::int32_t> buildInput(std::size_t n) {
  std::vector<std::int32_t> v(n);
  std::mt19937 rng{0xc1701U};
  std::uniform_int_distribution<std::int32_t> dist(
      std::numeric_limits<std::int32_t>::min(),
      std::numeric_limits<std::int32_t>::max());
  for (std::size_t i = 0; i < n; ++i) {
    v[i] = dist(rng);
  }
  return v;
}

/// Sequential merge of two sorted runs `[aLo, aHi)` and `[bLo, bHi)` from
/// `src` into `dst[outLo..]`. Returns the number of elements written.
inline std::size_t seqMerge(const std::int32_t *src, std::size_t aLo,
                            std::size_t aHi, std::size_t bLo, std::size_t bHi,
                            std::int32_t *dst, std::size_t outLo) noexcept {
  std::size_t i = aLo;
  std::size_t j = bLo;
  std::size_t k = outLo;
  while (i < aHi && j < bHi) {
    if (src[i] <= src[j]) {
      dst[k++] = src[i++];
    } else {
      dst[k++] = src[j++];
    }
  }
  while (i < aHi) {
    dst[k++] = src[i++];
  }
  while (j < bHi) {
    dst[k++] = src[j++];
  }
  return k - outLo;
}

/// Bucket descriptor for the parallel-merge phase. Each bucket carves a
/// contiguous slice of the first sorted run and a matching slice of the
/// second sorted run; the merged output lands at `outOffset`.
struct MergeBucket {
  std::size_t aLo;
  std::size_t aHi;
  std::size_t bLo;
  std::size_t bHi;
  std::size_t outOffset;
};

/// Parallel merge of two sorted runs `src[aLo..aHi)` and `src[bLo..bHi)`
/// into `dst[outLo..outLo + (aHi-aLo) + (bHi-bLo))`.
///
/// Splitter strategy: pick `kMergeBuckets - 1` evenly-spaced positions in
/// the larger of the two runs, binary-search each splitter's key in the
/// other run to derive each bucket's boundary in the smaller run. The
/// resulting `kMergeBuckets` buckets are independent; each merges its own
/// slice via `seqMerge` into the matching destination offset. Output
/// offsets come from a `parallelScan` over the bucket-size array.
template <class Pool>
void parallelMerge(Pool &pool, std::int32_t *src, std::size_t aLo,
                   std::size_t aHi, std::size_t bLo, std::size_t bHi,
                   std::int32_t *dst, std::size_t outLo) {
  const std::size_t nA = aHi - aLo;
  const std::size_t nB = bHi - bLo;
  const std::size_t total = nA + nB;
  if (total == 0) {
    return;
  }
  // Below the bucketing threshold the sequential merge wins: the splitter
  // setup + scan + parallelFor fixed cost overshoots the per-element merge
  // budget.
  if (total < kSeqCutoff) {
    seqMerge(src, aLo, aHi, bLo, bHi, dst, outLo);
    return;
  }

  // Make the larger run the splitter source so each bucket's primary slice
  // is balanced in size; the secondary run gets carved up by binary search.
  const bool aIsLarger = nA >= nB;
  const std::size_t primLo = aIsLarger ? aLo : bLo;
  const std::size_t primHi = aIsLarger ? aHi : bHi;
  const std::size_t secLo = aIsLarger ? bLo : aLo;
  const std::size_t secHi = aIsLarger ? bHi : aHi;
  const std::size_t nPrim = primHi - primLo;

  // Splitter offsets in the primary run, evenly spaced. Bucket k spans
  // [primSplit[k], primSplit[k+1]); secSplit[k] is the matching position in
  // the secondary run such that everything left of it is <= the splitter
  // key (or, for the boundary buckets, the secondary's own bounds).
  std::array<std::size_t, kMergeBuckets + 1U> primSplit{};
  std::array<std::size_t, kMergeBuckets + 1U> secSplit{};
  primSplit[0] = primLo;
  primSplit[kMergeBuckets] = primHi;
  secSplit[0] = secLo;
  secSplit[kMergeBuckets] = secHi;
  for (std::size_t k = 1; k < kMergeBuckets; ++k) {
    primSplit[k] = primLo + ((nPrim * k) / kMergeBuckets);
    const std::int32_t key = src[primSplit[k]];
    // `lower_bound` finds the first position >= key; this is the partition
    // such that secondary[secLo..pos) is < key and secondary[pos..secHi) is
    // >= key. With duplicates, ties land on the secondary side of the cut,
    // matching the `<=` test in `seqMerge` above.
    secSplit[k] = static_cast<std::size_t>(
        std::lower_bound(src + secLo, src + secHi, key) - src);
  }

  // Build the bucket descriptors with the orientation undone (each bucket's
  // `aLo/aHi` always refers to the original A run, `bLo/bHi` to B).
  std::array<MergeBucket, kMergeBuckets> buckets{};
  std::array<std::size_t, kMergeBuckets> sizes{};
  for (std::size_t k = 0; k < kMergeBuckets; ++k) {
    if (aIsLarger) {
      buckets[k] = MergeBucket{.aLo = primSplit[k],
                               .aHi = primSplit[k + 1U],
                               .bLo = secSplit[k],
                               .bHi = secSplit[k + 1U],
                               .outOffset = 0U};
    } else {
      buckets[k] = MergeBucket{.aLo = secSplit[k],
                               .aHi = secSplit[k + 1U],
                               .bLo = primSplit[k],
                               .bHi = primSplit[k + 1U],
                               .outOffset = 0U};
    }
    sizes[k] =
        (buckets[k].aHi - buckets[k].aLo) + (buckets[k].bHi - buckets[k].bLo);
  }

  // Compute the bucket-offset prefix via `parallelScan`. Pass 1's body sums
  // the chunk's slice of `sizes`; the cross-chunk combiner is plain `+`;
  // Pass 2's body writes the per-bucket exclusive prefix into `offsets`.
  // The total `total` is returned and used as a sanity check below.
  std::array<std::size_t, kMergeBuckets> offsets{};
  if constexpr (std::is_same_v<Pool, ::citor::ThreadPool>) {
    auto body = [&sizes, &offsets](std::size_t /*chunkId*/, std::size_t lo,
                                   std::size_t hi, std::size_t initial,
                                   std::size_t * /*unused*/) -> std::size_t {
      // Both passes write `offsets[i]` unconditionally. Pass 1 writes
      // intermediate values that pass 2 overwrites with the correct
      // exclusive prefix; correctness depends on pass 2 running strictly
      // after pass 1 across every chunk (the bench dispatches them
      // sequentially via two parallelScan calls).
      //
      // The two-pass body uses the `running - initial` partial-sum
      // convention: pass 1 returns the chunk's local sum, pass 2 returns
      // the same value (so the inclusive accumulator at the right edge
      // equals `total`).
      std::size_t running = initial;
      for (std::size_t i = lo; i < hi; ++i) {
        offsets[i] = running;
        running += sizes[i];
      }
      return running - initial;
    };
    const std::size_t inclusiveTotal =
        pool.template parallelScan<citor::HintsDefaults>(
            kMergeBuckets, std::size_t{0}, body, std::plus<std::size_t>{});
    CITOR_ALWAYS_ASSERT(inclusiveTotal == total);
  }
#ifdef CITOR_BENCH_HAS_TBB
  else if constexpr (std::is_same_v<Pool, ::tbb::task_arena>) {
    pool.execute([&] {
      ::tbb::parallel_scan(
          ::tbb::blocked_range<std::size_t>{0U, kMergeBuckets}, std::size_t{0},
          [&](const ::tbb::blocked_range<std::size_t> &r, std::size_t initial,
              bool isFinalScan) -> std::size_t {
            std::size_t running = initial;
            for (std::size_t i = r.begin(); i < r.end(); ++i) {
              if (isFinalScan) {
                offsets[i] = running;
              }
              running += sizes[i];
            }
            return running;
          },
          [](std::size_t a, std::size_t b) { return a + b; });
    });
  }
#endif
#ifdef CITOR_BENCH_HAS_OPENMP
  else if constexpr (std::is_same_v<Pool, OpenMpRunner>) {
    // OpenMP 5.0 `scan` is uneven; serial prefix is correct and below the
    // measurement granularity for kMergeBuckets = 32.
    std::size_t running = 0;
    for (std::size_t i = 0; i < kMergeBuckets; ++i) {
      offsets[i] = running;
      running += sizes[i];
    }
    CITOR_ALWAYS_ASSERT(running == total);
  }
#endif
#ifdef CITOR_BENCH_HAS_DISPENSO
  else if constexpr (std::is_same_v<Pool, ::dispenso::ThreadPool>) {
    // dispenso has no first-class scan; mirror the OpenMP serial-prefix
    // shortcut. kMergeBuckets = 32 means the scan is below measurement
    // granularity, so this is honest rather than a substitution dodge.
    (void)pool;
    std::size_t running = 0;
    for (std::size_t i = 0; i < kMergeBuckets; ++i) {
      offsets[i] = running;
      running += sizes[i];
    }
    CITOR_ALWAYS_ASSERT(running == total);
  }
#endif
#ifdef CITOR_BENCH_HAS_TASKFLOW
  else if constexpr (std::is_same_v<Pool, ::tf::Subflow>) {
    // Taskflow Subflow: serial-prefix the offset scan; nesting another
    // taskflow + scan-emulation inside the recursing Subflow would just
    // schedule kMergeBuckets independent tasks and pay round-trip dispatch
    // for a 64-element prefix that completes in nanoseconds serially.
    (void)pool;
    std::size_t running = 0;
    for (std::size_t i = 0; i < kMergeBuckets; ++i) {
      offsets[i] = running;
      running += sizes[i];
    }
    CITOR_ALWAYS_ASSERT(running == total);
  }
#endif

  // Stamp the output offsets into each bucket descriptor and dispatch the
  // per-bucket merge via `parallelFor` (one bucket per slot).
  for (std::size_t k = 0; k < kMergeBuckets; ++k) {
    buckets[k].outOffset = outLo + offsets[k];
  }

  if constexpr (std::is_same_v<Pool, ::citor::ThreadPool>) {
    pool.template parallelFor<CilksortForHints>(
        std::size_t{0}, kMergeBuckets,
        [&buckets, src, dst](std::size_t lo, std::size_t hi) {
          for (std::size_t k = lo; k < hi; ++k) {
            const MergeBucket &bk = buckets[k];
            seqMerge(src, bk.aLo, bk.aHi, bk.bLo, bk.bHi, dst, bk.outOffset);
          }
        });
  }
#ifdef CITOR_BENCH_HAS_TBB
  else if constexpr (std::is_same_v<Pool, ::tbb::task_arena>) {
    pool.execute([&] {
      ::tbb::parallel_for(
          ::tbb::blocked_range<std::size_t>{0U, kMergeBuckets, 1U},
          [&buckets, src, dst](const ::tbb::blocked_range<std::size_t> &r) {
            for (std::size_t k = r.begin(); k < r.end(); ++k) {
              const MergeBucket &bk = buckets[k];
              seqMerge(src, bk.aLo, bk.aHi, bk.bLo, bk.bHi, dst, bk.outOffset);
            }
          },
          ::tbb::simple_partitioner{});
    });
  }
#endif
#ifdef CITOR_BENCH_HAS_OPENMP
  else if constexpr (std::is_same_v<Pool, OpenMpRunner>) {
    // `taskloop` joins the existing team opened by the outer `parallel`
    // region; a nested `parallel for` here would be serialized by the default
    // `OMP_MAX_ACTIVE_LEVELS=1` and silently degrade the merge to single-
    // threaded on the call site.
    (void)pool;
    /// OpenMP `taskloop` lowers the loop counter to a signed `long` in the
    /// generated runtime call, which then converts back to `std::size_t`
    /// when indexing `buckets`. The conversion warning is intrinsic to the
    /// pragma's lowering; suppress locally rather than rewriting the loop.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma omp taskloop grainsize(1) shared(buckets, src, dst)
    for (std::size_t k = 0; k < kMergeBuckets; ++k) {
      const MergeBucket &bk = buckets[k];
      seqMerge(src, bk.aLo, bk.aHi, bk.bLo, bk.bHi, dst, bk.outOffset);
    }
#pragma GCC diagnostic pop
  }
#endif
#ifdef CITOR_BENCH_HAS_DISPENSO
  else if constexpr (std::is_same_v<Pool, ::dispenso::ThreadPool>) {
    // dispenso path: serial fallback for the per-bucket merge. dispenso's
    // TaskSet / parallel_for nested inside the outer recursiveSpawn2 TaskSet
    // crashes the worker on `parentTaskSet()` TLS access. kMergeBuckets = 32
    // so the merge step is fast enough that single-threaded execution is
    // honest at this scale; the substitution shows on the row label.
    (void)pool;
    for (std::size_t k = 0; k < kMergeBuckets; ++k) {
      const MergeBucket &bk = buckets[k];
      seqMerge(src, bk.aLo, bk.aHi, bk.bLo, bk.bHi, dst, bk.outOffset);
    }
  }
#endif
#ifdef CITOR_BENCH_HAS_TASKFLOW
  else if constexpr (std::is_same_v<Pool, ::tf::Subflow>) {
    // Taskflow Subflow: serial fallback for the per-bucket merge. The outer
    // recursiveSpawn2(pool, ...) already calls pool.join() before returning;
    // tf::Subflow may only be joined once, so we cannot emplace another
    // wave on the parent Subflow here. kMergeBuckets is small enough that
    // serial execution is honest at this scale.
    (void)pool;
    for (std::size_t k = 0; k < kMergeBuckets; ++k) {
      const MergeBucket &bk = buckets[k];
      seqMerge(src, bk.aLo, bk.aHi, bk.bLo, bk.bHi, dst, bk.outOffset);
    }
  }
#endif
}

/// Recursive cilksort: split `[lo, hi)` in half, recursively sort each half
/// via `recursiveSpawn2`, then `parallelMerge` the two halves from `tmp`
/// back into `data`. The `tmp` buffer mirrors `data`'s layout and serves as
/// the merge scratch. The active orientation flips at every recursion
/// level: at depth d odd, the children sort into `data` and the merge
/// destination is `tmp`; at depth d even, the opposite. The driver below
/// detects the parity and copies if needed.
template <class Pool>
void cilksortRec(Pool &pool, std::int32_t *data, std::int32_t *tmp,
                 std::size_t lo, std::size_t hi) {
  const std::size_t n = hi - lo;
  if (n <= kSeqCutoff) {
    std::sort(data + lo, data + hi);
    return;
  }
  const std::size_t mid = lo + (n / 2U);
  recursiveSpawn2(
      pool, [&](Pool &p) { cilksortRec(p, data, tmp, lo, mid); },
      [&](Pool &p) { cilksortRec(p, data, tmp, mid, hi); });
  // Merge the two sorted halves of `data[lo..hi)` into `tmp[lo..hi)`, then
  // copy back. The copy keeps the algorithm in-place from the caller's
  // perspective at the cost of one extra pass per merge level. The pure
  // alternative -- ping-ponging between `data` and `tmp` across recursion
  // levels -- doubles the splitter logic without adding measurable speed,
  // so the explicit copy keeps the bucket-merge code straightforward.
  parallelMerge(pool, data, lo, mid, mid, hi, tmp, lo);
  std::copy(tmp + lo, tmp + hi, data + lo);
}

template <class PoolT>
[[nodiscard]] BenchRow measureCilksort(const char *name,
                                       std::size_t participants, std::size_t n,
                                       const CyclesPerNanosecond &cal) {
  static_assert(RecursiveForkJoinTraits<PoolT>::supportsRecursiveSpawn,
                "cilksort bench requires recursive-spawn-capable pool; the "
                "trait gate excludes "
                "BS / dp / task_thread_pool / riften / Eigen / Taskflow "
                "Executor at compile time.");
  using Traits = CompetitorTraits<PoolT>;
  auto pool = Traits::make(participants);

  const std::vector<std::int32_t> input = buildInput(n);
  std::vector<std::int32_t> reference = input;
  std::sort(reference.begin(), reference.end());

  // Per-iteration scratch: a fresh copy of `input` lands in `data`, the
  // cilksort runs in place, and the result is verified against `reference`.
  std::vector<std::int32_t> data = input;
  std::vector<std::int32_t> tmp(n, std::int32_t{0});

  // Warmup + correctness gate. Run the sort once and verify the output
  // matches `std::sort` element-wise before any timing iteration.
  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    data = input;
    cilksortRec(*pool, data.data(), tmp.data(), std::size_t{0}, n);
    CITOR_ALWAYS_ASSERT(data == reference);
  }

  std::vector<double> samples;
  samples.reserve(kIterations);
  for (std::size_t i = 0; i < kIterations; ++i) {
    data = input;
    const std::uint64_t startCycles = readCyclesStart();
    cilksortRec(*pool, data.data(), tmp.data(), std::size_t{0}, n);
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    CITOR_ALWAYS_ASSERT(data == reference);
  }

  return finalizeRow(name, samples);
}

[[nodiscard]] BenchRow measureCitor(std::size_t participants, std::size_t n,
                                    const CyclesPerNanosecond &cal) {
  return measureCilksort<citor::ThreadPool>("citor::ThreadPool", participants,
                                            n, cal);
}

#ifdef CITOR_BENCH_HAS_TBB
[[nodiscard]] BenchRow measureTbb(std::size_t participants, std::size_t n,
                                  const CyclesPerNanosecond &cal) {
  return measureCilksort<::tbb::task_arena>("oneTBB", participants, n, cal);
}
#endif

#ifdef CITOR_BENCH_HAS_TASKFLOW
[[nodiscard]] BenchRow measureTaskflow(std::size_t participants, std::size_t n,
                                       const CyclesPerNanosecond &cal) {
  static_assert(
      RecursiveForkJoinTraits<::tf::Subflow>::supportsRecursiveSpawn,
      "Taskflow Subflow must opt into recursive spawn for the cilksort bench");
  ::tf::Executor exec(participants);
  const std::vector<std::int32_t> input = buildInput(n);
  std::vector<std::int32_t> reference = input;
  std::sort(reference.begin(), reference.end());
  std::vector<std::int32_t> data(n, std::int32_t{0});
  std::vector<std::int32_t> tmp(n, std::int32_t{0});

  auto runOnce = [&]() {
    data = input;
    ::tf::Taskflow flow;
    flow.emplace([&](::tf::Subflow &rootSub) {
      cilksortRec(rootSub, data.data(), tmp.data(), std::size_t{0}, n);
    });
    exec.run(flow).wait();
  };

  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    runOnce();
    CITOR_ALWAYS_ASSERT(data == reference);
  }

  std::vector<double> samples;
  samples.reserve(kIterations);
  for (std::size_t i = 0; i < kIterations; ++i) {
    data = input;
    const std::uint64_t startCycles = readCyclesStart();
    runOnce();
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    CITOR_ALWAYS_ASSERT(data == reference);
  }
  return finalizeRow("Taskflow::Subflow", samples);
}
#endif

// dispenso is not wired here. The cilksort shape combines
// recursiveSpawn2 (one outer TaskSet on the calling worker) with a per-bucket
// parallelMerge that on dispenso would either nest a second TaskSet or call
// dispenso::parallel_for from inside a worker. Empirically both forms
// segfault on dispenso's `parentTaskSet()` thread-local-stack access -- the
// dispenso worker's TLS slot for the per-thread task-stack appears to get
// corrupted under that nesting pattern. Reverting to a serial parallelMerge
// for dispenso also crashes (the recursiveSpawn2 alone is enough), so the
// exclusion is on the recursive-spawn-on-dispenso-from-cilksort path itself,
// not on the inner parallel section. The 4 simpler fork-join cells (fib /
// queens / UTS / Strassen) -- which use only one TaskSet level via
// recursiveSpawn2 -- run dispenso cleanly, so this is a depth-or-pattern
// issue in dispenso's TLS bookkeeping rather than a flat exclusion.

#ifdef CITOR_BENCH_HAS_OPENMP
[[nodiscard]] BenchRow measureOmp(std::size_t participants, std::size_t n,
                                  const CyclesPerNanosecond &cal) {
  static_assert(
      RecursiveForkJoinTraits<OpenMpRunner>::supportsRecursiveSpawn,
      "OpenMP runner must opt into recursive spawn for the cilksort bench");
  // OpenMP `task` requires an enclosing `parallel` region. Open the region
  // once per iteration, then funnel into the recursion via a `single`
  // construct so only one thread enters the root.
  OpenMpRunner runner{participants};
  const std::vector<std::int32_t> input = buildInput(n);
  std::vector<std::int32_t> reference = input;
  std::sort(reference.begin(), reference.end());
  std::vector<std::int32_t> data(n, std::int32_t{0});
  std::vector<std::int32_t> tmp(n, std::int32_t{0});

  auto runOnce = [&]() {
    data = input;
#pragma omp parallel num_threads(static_cast<int>(participants))
    {
#pragma omp single
      {
        cilksortRec(runner, data.data(), tmp.data(), std::size_t{0}, n);
      }
    }
  };

  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    runOnce();
    CITOR_ALWAYS_ASSERT(data == reference);
  }

  std::vector<double> samples;
  samples.reserve(kIterations);
  for (std::size_t i = 0; i < kIterations; ++i) {
    data = input;
    const std::uint64_t startCycles = readCyclesStart();
#pragma omp parallel num_threads(static_cast<int>(participants))
    {
#pragma omp single
      {
        cilksortRec(runner, data.data(), tmp.data(), std::size_t{0}, n);
      }
    }
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    CITOR_ALWAYS_ASSERT(data == reference);
  }

  return finalizeRow("OpenMP", samples);
}
#endif

struct CilksortCell {
  std::size_t n;
  const char *suffix;
};

constexpr std::array<CilksortCell, 2> kCells{{
    {.n = 1U << 20U, .suffix = "n1m"},
    {.n = 1U << 24U, .suffix = "n16m"},
}};

BenchTable buildTable(std::size_t participants, CilksortCell cell,
                      const CyclesPerNanosecond &cal) {
  BenchTable table;
  table.workload = std::string{"forkjoin_cilksort_j"} +
                   std::to_string(participants) + "_" + cell.suffix;
  table.rows.push_back(measureCitor(participants, cell.n, cal));
#ifdef CITOR_BENCH_HAS_TBB
  table.rows.push_back(measureTbb(participants, cell.n, cal));
#endif
#ifdef CITOR_BENCH_HAS_OPENMP
  table.rows.push_back(measureOmp(participants, cell.n, cal));
#endif
#ifdef CITOR_BENCH_HAS_TASKFLOW
  table.rows.push_back(measureTaskflow(participants, cell.n, cal));
#endif
#ifdef CITOR_BENCH_HAS_LIBFORK
  table.rows.push_back(runLibforkCilksort(participants, cell.n, cal));
#endif
#ifdef CITOR_BENCH_HAS_TMC
  table.rows.push_back(runTmcCilksort(participants, cell.n, cal));
#endif
  return table;
}

template <std::size_t CellIdx, std::size_t Participants>
BenchTable runCilksortCell(const CyclesPerNanosecond &cal) {
  static_assert(Participants == 8 || Participants == 16 || Participants == 32 ||
                    Participants == 48 || Participants == 96,
                "unsupported j-value");
  if (!hasEnoughPhysicalCores(Participants)) {
    throw std::runtime_error("needs " + std::to_string(Participants) +
                             " physical cores");
  }
  constexpr CilksortCell cell = kCells[CellIdx];
  return buildTable(Participants, cell, cal);
}

struct CilksortRegistrar {
  CilksortRegistrar() {
    registerWorkload(
        {.name = "forkjoin_cilksort_j8_n1m", .run = &runCilksortCell<0, 8>});
    registerWorkload(
        {.name = "forkjoin_cilksort_j16_n1m", .run = &runCilksortCell<0, 16>});
    registerWorkload(
        {.name = "forkjoin_cilksort_j32_n1m", .run = &runCilksortCell<0, 32>});
    registerWorkload(
        {.name = "forkjoin_cilksort_j48_n1m", .run = &runCilksortCell<0, 48>});
    registerWorkload(
        {.name = "forkjoin_cilksort_j96_n1m", .run = &runCilksortCell<0, 96>});
    registerWorkload(
        {.name = "forkjoin_cilksort_j8_n16m", .run = &runCilksortCell<1, 8>});
    registerWorkload(
        {.name = "forkjoin_cilksort_j16_n16m", .run = &runCilksortCell<1, 16>});
    registerWorkload(
        {.name = "forkjoin_cilksort_j32_n16m", .run = &runCilksortCell<1, 32>});
    registerWorkload(
        {.name = "forkjoin_cilksort_j48_n16m", .run = &runCilksortCell<1, 48>});
    registerWorkload(
        {.name = "forkjoin_cilksort_j96_n16m", .run = &runCilksortCell<1, 96>});
  }
};

const CilksortRegistrar kRegistrar;

} // namespace
} // namespace citor::bench
