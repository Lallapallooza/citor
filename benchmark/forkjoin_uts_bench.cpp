// Unbalanced Tree Search (UTS) workload for the comparative pool bench.
//
// Tree shape: canonical UTS T1 input (cilkbench / BOTS / upstream UTS
// `sample_trees.sh`) - geometric distribution with FIXED shape, root seed
// `19`, `b_0 = 4`, `gen_mx = 10`, expected total node count = 4'130'071.
// The FIXED shape uses a depth-independent target branching factor `b_i =
// b_0` for `depth < gen_mx`, then `b_i = 0`. Per-node child count is drawn
// from a geometric distribution with mean `b_i` via inverse-CDF sampling
// against a SHA-1-derived RNG state. The recursive descent runs on each
// pool's native fork-join primitive via `recursiveSpawn2<Pool>`; the
// compile-time `RecursiveForkJoinTraits` gate excludes pools whose `.get()`
// deadlocks under recursive spawn.
//
// RNG: per-node 20-byte SHA-1 digest. The root state is `SHA1(zeros[16] ||
// be32(seed))`; each child state is `SHA1(parent_state || be32(child_idx))`.
// The bench draws one 32-bit positive word from bytes [16..19] of the state
// (big-endian) per `rng_rand` call, masked to 31 bits, divided by 2^31 to
// land in [0, 1). This matches Brian Gladman's brg_sha1 reference + the
// Olivier 2006 UTS RNG harness verbatim.
//
// Source attribution: SHA-1 implementation derived from Brian Gladman's
// public-domain brg_sha1 (Issue Date: 01/08/2005). UTS tree shape and RNG
// harness derived from the Olivier 2006 UTS reference distribution
// (https://sourceforge.net/p/uts-benchmark/, also mirrored in cilkbench /
// BOTS). Both are embedded verbatim into this anonymous namespace because
// the bench TUs cannot pull a third-party crypto dependency for one pure
// function.

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "citor/always_assert.h"

#include "bench_format.h"
#include "bench_registry.h"
#include "competitor_traits.h"
#include "cycle_clock.h"
#include "libfork_runners.h"
#include "recursive_forkjoin_helper.h"
#include "tmc_runners.h"

#ifdef CITOR_BENCH_HAS_TBB
#include <oneapi/tbb/task_arena.h>
#include <oneapi/tbb/task_group.h>
#endif

#ifdef CITOR_BENCH_HAS_TASKFLOW
#include <taskflow/taskflow.hpp>
#endif

namespace citor::bench {
namespace {

// ---------------------------------------------------------------------------
// SHA-1 hash for the UTS RNG (Brian Gladman's brg_sha1 reference, byte
// oriented). Only the one-shot `sha1` entry point is used; the streaming
// API was elided since every UTS RNG call hashes a single contiguous
// buffer (20 bytes for `rng_init`, 24 bytes for `rng_spawn`).
// ---------------------------------------------------------------------------

[[nodiscard]] constexpr std::uint32_t rotl32(std::uint32_t x, unsigned n) noexcept {
  return (x << n) | (x >> (32U - n));
}

inline void sha1Compress(std::array<std::uint32_t, 5> &h,
                         const std::array<std::uint8_t, 64> &block) noexcept {
  std::array<std::uint32_t, 80> w{};
  for (std::size_t i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(block[(i * 4U)]) << 24U) |
           (static_cast<std::uint32_t>(block[(i * 4U) + 1U]) << 16U) |
           (static_cast<std::uint32_t>(block[(i * 4U) + 2U]) << 8U) |
           static_cast<std::uint32_t>(block[(i * 4U) + 3U]);
  }
  for (std::size_t i = 16; i < 80; ++i) {
    w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1U);
  }
  std::uint32_t a = h[0];
  std::uint32_t b = h[1];
  std::uint32_t c = h[2];
  std::uint32_t d = h[3];
  std::uint32_t e = h[4];
  for (std::size_t i = 0; i < 80; ++i) {
    std::uint32_t f = 0;
    std::uint32_t k = 0;
    if (i < 20) {
      f = (b & c) | ((~b) & d);
      k = 0x5A827999U;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1U;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDCU;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6U;
    }
    const std::uint32_t t = rotl32(a, 5U) + f + e + k + w[i];
    e = d;
    d = c;
    c = rotl32(b, 30U);
    b = a;
    a = t;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
}

/// One-shot SHA-1 over a contiguous byte buffer. Output is written as 20
/// big-endian bytes into |out|. The bench only ever hashes up to 24-byte
/// inputs (rng_init: 20 bytes, rng_spawn: 24 bytes), but the multi-block
/// loop is preserved for general correctness against the reference.
inline void sha1Compute(const std::uint8_t *data, std::size_t len,
                        std::array<std::uint8_t, 20> &out) noexcept {
  std::array<std::uint32_t, 5> h = {0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U,
                                    0xC3D2E1F0U};
  const std::uint64_t totalBits = static_cast<std::uint64_t>(len) * 8U;
  std::array<std::uint8_t, 64> block{};
  std::size_t pos = 0;
  while (len - pos >= 64) {
    std::memcpy(block.data(), data + pos, 64);
    sha1Compress(h, block);
    pos += 64;
  }
  block.fill(0);
  const std::size_t rem = len - pos;
  if (rem != 0U) {
    std::memcpy(block.data(), data + pos, rem);
  }
  block[rem] = 0x80U;
  if (rem >= 56) {
    sha1Compress(h, block);
    block.fill(0);
  }
  // Big-endian 64-bit length terminator.
  block[56] = static_cast<std::uint8_t>((totalBits >> 56U) & 0xFFU);
  block[57] = static_cast<std::uint8_t>((totalBits >> 48U) & 0xFFU);
  block[58] = static_cast<std::uint8_t>((totalBits >> 40U) & 0xFFU);
  block[59] = static_cast<std::uint8_t>((totalBits >> 32U) & 0xFFU);
  block[60] = static_cast<std::uint8_t>((totalBits >> 24U) & 0xFFU);
  block[61] = static_cast<std::uint8_t>((totalBits >> 16U) & 0xFFU);
  block[62] = static_cast<std::uint8_t>((totalBits >> 8U) & 0xFFU);
  block[63] = static_cast<std::uint8_t>(totalBits & 0xFFU);
  sha1Compress(h, block);
  for (std::size_t i = 0; i < 5; ++i) {
    out[(i * 4U)] = static_cast<std::uint8_t>((h[i] >> 24U) & 0xFFU);
    out[(i * 4U) + 1U] = static_cast<std::uint8_t>((h[i] >> 16U) & 0xFFU);
    out[(i * 4U) + 2U] = static_cast<std::uint8_t>((h[i] >> 8U) & 0xFFU);
    out[(i * 4U) + 3U] = static_cast<std::uint8_t>(h[i] & 0xFFU);
  }
}

// ---------------------------------------------------------------------------
// UTS per-node RNG state and harness operations (Olivier 2006 reference).
// ---------------------------------------------------------------------------

/// Per-node RNG state: a 20-byte SHA-1 digest. Each child has an
/// independent state derived from the parent's state and the child's
/// position among siblings.
struct RngState {
  std::array<std::uint8_t, 20> bytes;
};

/// Build the root RNG state from a 32-bit seed. Layout of the input
/// buffer matches the upstream `rng_init`: 16 zero bytes followed by the
/// big-endian seed, hashed into a 20-byte digest.
[[nodiscard]] inline RngState rngInit(std::uint32_t seed) noexcept {
  std::array<std::uint8_t, 20> buf{};
  buf[16] = static_cast<std::uint8_t>((seed >> 24U) & 0xFFU);
  buf[17] = static_cast<std::uint8_t>((seed >> 16U) & 0xFFU);
  buf[18] = static_cast<std::uint8_t>((seed >> 8U) & 0xFFU);
  buf[19] = static_cast<std::uint8_t>(seed & 0xFFU);
  RngState out{};
  sha1Compute(buf.data(), buf.size(), out.bytes);
  return out;
}

/// Spawn a child RNG state. The input is the parent's 20-byte state
/// followed by the big-endian 32-bit child index; the digest is the
/// child's state. Matches upstream `rng_spawn` byte-for-byte.
[[nodiscard]] inline RngState rngSpawn(const RngState &parent, std::uint32_t childIdx) noexcept {
  std::array<std::uint8_t, 24> buf{};
  std::memcpy(buf.data(), parent.bytes.data(), 20);
  buf[20] = static_cast<std::uint8_t>((childIdx >> 24U) & 0xFFU);
  buf[21] = static_cast<std::uint8_t>((childIdx >> 16U) & 0xFFU);
  buf[22] = static_cast<std::uint8_t>((childIdx >> 8U) & 0xFFU);
  buf[23] = static_cast<std::uint8_t>(childIdx & 0xFFU);
  RngState out{};
  sha1Compute(buf.data(), buf.size(), out.bytes);
  return out;
}

/// Read one 31-bit positive integer from the state. Matches upstream
/// `rng_rand`: bytes [16..19] interpreted big-endian, masked with
/// `0x7FFFFFFF` (`POS_MASK`).
[[nodiscard]] inline std::int32_t rngRand(const RngState &s) noexcept {
  const std::uint32_t b = (static_cast<std::uint32_t>(s.bytes[16]) << 24U) |
                          (static_cast<std::uint32_t>(s.bytes[17]) << 16U) |
                          (static_cast<std::uint32_t>(s.bytes[18]) << 8U) |
                          static_cast<std::uint32_t>(s.bytes[19]);
  return static_cast<std::int32_t>(b & 0x7FFFFFFFU);
}

/// Map a 31-bit positive integer to a uniform value on [0, 1). Matches
/// upstream `rng_toProb` (divide by 2^31).
[[nodiscard]] inline double rngToProb(std::int32_t n) noexcept {
  return n < 0 ? 0.0 : static_cast<double>(n) / 2147483648.0;
}

// ---------------------------------------------------------------------------
// Canonical UTS T1 parameters (sample_trees.sh: `-t 1 -a 3 -d 10 -b 4 -r 19`).
// ---------------------------------------------------------------------------

/// Root branching factor (b_0).
constexpr int kRootB0 = 4;
/// Maximum tree depth (gen_mx). FIXED shape returns `b_i = b_0` for depths
/// strictly less than this and `b_i = 0` at this depth and below.
constexpr int kMaxDepth = 10;
/// Root seed (-r 19 in the canonical T1 invocation).
constexpr std::uint32_t kRootSeed = 19U;
/// Expected total node count for the canonical T1 tree shape. Documented
/// in upstream `sample_trees.sh` as the verification target for
/// `T1="-t 1 -a 3 -d 10 -b 4 -r 19"`.
constexpr std::int64_t kExpectedNodes = 4'130'071;

/// Number of children at a node with RNG state |state| at depth |depth|,
/// using the FIXED-shape geometric tree distribution (UTS T1):
///
///   - For `depth < gen_mx`, target branching factor `b_i = b_0`.
///   - For `depth >= gen_mx`, `b_i = 0` (leaf).
///   - Sample child count from a geometric distribution with mean `b_i`
///     via the inverse-CDF: `numChildren = floor(log(1-u) / log(1-p))`
///     where `p = 1 / (1 + b_i)` and `u ~ U[0, 1)` is drawn from the
///     state's `rng_rand` word.
[[nodiscard]] inline int utsNumChildrenGeo(const RngState &state, int depth) noexcept {
  // For depth == 0 the upstream code also enters this branch with `b_i =
  // b_0`; the FIXED shape function happens to coincide with the depth-0
  // case so no special-casing is needed.
  const double bI = (depth < kMaxDepth) ? static_cast<double>(kRootB0) : 0.0;
  const double p = 1.0 / (1.0 + bI);
  const std::int32_t h = rngRand(state);
  const double u = rngToProb(h);
  if (bI <= 0.0) {
    return 0;
  }
  return static_cast<int>(std::floor(std::log(1.0 - u) / std::log(1.0 - p)));
}

/// Sequential walker; returns total node count rooted at |state| at the
/// given depth. Used for the warm-up / correctness gate before timing and
/// for the expected-count cross-reference at TU init.
[[nodiscard]] std::int64_t seqWalk(const RngState &state, int depth) noexcept {
  std::int64_t count = 1;
  const int n = utsNumChildrenGeo(state, depth);
  for (int i = 0; i < n; ++i) {
    const RngState child = rngSpawn(state, static_cast<std::uint32_t>(i));
    count += seqWalk(child, depth + 1);
  }
  return count;
}

/// Sequential cutoff depth: below this depth the recursion runs inline on
/// the calling worker. The cutoff bounds the per-task overhead; without it
/// each leaf would be its own fork-join descriptor and dispatch cost would
/// dominate. Depth 5 keeps the parallel region well populated for
/// j={8,16} on the canonical T1 tree (which has gen_mx = 10).
constexpr int kSeqCutoffDepth = 5;

/// Parallel walker: spawns recursively until |depth| crosses the cutoff,
/// then drops into `seqWalk`. Each child of the current node becomes its own
/// stealable task via `recursiveSpawnN`, mirroring the libfork / TBB / TMC
/// idiomatic shape. The earlier bisection variant capped the per-node fan-out
/// at 2 stealable units (`2^depth` total decomposition over the recursion's
/// `kSeqCutoffDepth` levels); for canonical UTS T1 (`b0=4`) that is 4x less
/// task-graph parallelism than the underlying tree exposes. Per-child counts
/// land in `partials` and merge after the join.
template <class Pool> std::int64_t parWalk(Pool &pool, const RngState &state, int depth) {
  if (depth >= kSeqCutoffDepth) {
    return seqWalk(state, depth);
  }
  const int n = utsNumChildrenGeo(state, depth);
  if (n == 0) {
    return 1;
  }
  if (n == 1) {
    const RngState child = rngSpawn(state, 0U);
    return 1 + parWalk(pool, child, depth + 1);
  }
  std::vector<std::int64_t> partials(static_cast<std::size_t>(n), 0);
  recursiveSpawnN(pool, static_cast<std::size_t>(n), [&](Pool &p, std::size_t i) {
    const RngState child = rngSpawn(state, static_cast<std::uint32_t>(i));
    partials[i] = parWalk(p, child, depth + 1);
  });
  std::int64_t total = 1;
  for (const std::int64_t v : partials) {
    total += v;
  }
  return total;
}

// ---------------------------------------------------------------------------
// Per-pool measurement.
// ---------------------------------------------------------------------------

/// Per-cell sample budget. UTS T1 takes a few tens of milliseconds per
/// iteration depending on pool, so 25 samples keeps a row under a few
/// seconds wall.
constexpr std::size_t kIterations = 25;
constexpr std::size_t kWarmupIterations = 3;

template <class PoolT>
[[nodiscard]] BenchRow measureUts(const char *name, std::size_t participants,
                                  const CyclesPerNanosecond &cal) {
  static_assert(RecursiveForkJoinTraits<PoolT>::supportsRecursiveSpawn,
                "UTS bench requires recursive-spawn-capable pool; the trait gate excludes "
                "BS / dp / task_thread_pool / riften / Eigen / Taskflow Executor at compile time.");
  using Traits = CompetitorTraits<PoolT>;
  auto pool = Traits::make(participants);

  const RngState root = rngInit(kRootSeed);

  // Warmup + correctness gate before timing. Verify the parallel walker
  // returns the canonical T1 node count exactly.
  std::int64_t parallelCount = 0;
  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    parallelCount = parWalk(*pool, root, 0);
  }
  CITOR_ALWAYS_ASSERT(parallelCount == kExpectedNodes);

  std::vector<double> samples;
  samples.reserve(kIterations);
  for (std::size_t i = 0; i < kIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    const std::int64_t count = parWalk(*pool, root, 0);
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    CITOR_ALWAYS_ASSERT(count == kExpectedNodes);
  }

  return finalizeRow(name, samples);
}

// citor uses its `ThreadPool` directly. oneTBB and OpenMP are routed
// through the `recursiveSpawn` helper just like citor; Taskflow Subflow
// has a different entry-point signature (the body owns a `tf::Subflow&`
// rather than a `tf::Executor&`) and is wired separately.

[[nodiscard]] BenchRow measureCitor(std::size_t participants, const CyclesPerNanosecond &cal) {
  return measureUts<citor::ThreadPool>("citor::ThreadPool", participants, cal);
}

#ifdef CITOR_BENCH_HAS_TBB
[[nodiscard]] BenchRow measureTbb(std::size_t participants, const CyclesPerNanosecond &cal) {
  return measureUts<::tbb::task_arena>("oneTBB", participants, cal);
}
#endif

#ifdef CITOR_BENCH_HAS_DISPENSO
[[nodiscard]] BenchRow measureDispenso(std::size_t participants, const CyclesPerNanosecond &cal) {
  static_assert(RecursiveForkJoinTraits<::dispenso::ThreadPool>::supportsRecursiveSpawn,
                "dispenso must opt into recursive spawn for the UTS bench");
  return measureUts<::dispenso::ThreadPool>("dispenso::ThreadPool", participants, cal);
}
#endif

#ifdef CITOR_BENCH_HAS_OPENMP
[[nodiscard]] BenchRow measureOmp(std::size_t participants, const CyclesPerNanosecond &cal) {
  static_assert(RecursiveForkJoinTraits<OpenMpRunner>::supportsRecursiveSpawn,
                "OpenMP runner must opt into recursive spawn for the UTS bench");
  // OpenMP's `task` directive must be inside an open `parallel` region;
  // the helper's `taskwait` works at any task scope, but the surrounding
  // region must exist. Open the region once per iteration and let the
  // root task descend.
  OpenMpRunner runner{participants};
  const RngState root = rngInit(kRootSeed);

  // Warmup + correctness.
  std::int64_t parallelCount = 0;
  auto runOnce = [&]() {
    std::int64_t result = 0;
#pragma omp parallel num_threads(static_cast<int>(participants))
    {
#pragma omp single
      {
        result = parWalk(runner, root, 0);
      }
    }
    return result;
  };
  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    parallelCount = runOnce();
  }
  CITOR_ALWAYS_ASSERT(parallelCount == kExpectedNodes);

  std::vector<double> samples;
  samples.reserve(kIterations);
  for (std::size_t i = 0; i < kIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    const std::int64_t count = runOnce();
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    CITOR_ALWAYS_ASSERT(count == kExpectedNodes);
  }
  return finalizeRow("OpenMP", samples);
}
#endif

// ---------------------------------------------------------------------------
// Tables.
// ---------------------------------------------------------------------------

BenchTable buildTable(std::size_t participants, const char *suffix,
                      const CyclesPerNanosecond &cal) {
  // Lazy first-call cross-check of the canonical T1 node count via the
  // sequential walker; if T1's tree shape parameters drift, the bench
  // aborts rather than silently publishing wrong rows. Function-local
  // static is thread-safe in C++11+, so the 4.13M-node walk runs at most
  // once per process and is gated to actual UTS workload invocations
  // (not every parallel_bench startup).
  static const bool checked = []() {
    const RngState root = rngInit(kRootSeed);
    const std::int64_t seq = seqWalk(root, 0);
    CITOR_ALWAYS_ASSERT(seq == kExpectedNodes);
    return true;
  }();
  (void)checked;

  BenchTable table;
  table.workload = std::string{"forkjoin_uts_t1_"} + suffix;
  table.rows.push_back(measureCitor(participants, cal));
#ifdef CITOR_BENCH_HAS_TBB
  table.rows.push_back(measureTbb(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_OPENMP
  table.rows.push_back(measureOmp(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_DISPENSO
  table.rows.push_back(measureDispenso(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_LIBFORK
  table.rows.push_back(runLibforkUtsT1(participants, cal));
#endif
#ifdef CITOR_BENCH_HAS_TMC
  table.rows.push_back(runTmcUtsT1(participants, cal));
#endif
  return table;
}

BenchTable runUtsJ8(const CyclesPerNanosecond &cal) {
  return buildTable(/*participants=*/8, "j8", cal);
}

BenchTable runUtsJ16(const CyclesPerNanosecond &cal) {
  return buildTable(/*participants=*/16, "j16", cal);
}

struct UtsRegistrar {
  UtsRegistrar() {
    registerWorkload({.name = "forkjoin_uts_t1_j8", .run = &runUtsJ8});
    registerWorkload({.name = "forkjoin_uts_t1_j16", .run = &runUtsJ16});
  }
};

const UtsRegistrar kRegistrar;

} // namespace
} // namespace citor::bench
