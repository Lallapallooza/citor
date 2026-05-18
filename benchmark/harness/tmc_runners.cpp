// TooManyCooks (tzcnt/TooManyCooks v1.4.0) coroutine runners for the
// comparative fork-join benches. Stays at the project's default C++20 -- no
// per-TU language override needed (unlike libfork which requires C++23).
//
// tmc has a process-global executor (`tmc::cpu_executor()`); the runners
// init/teardown it per measurement to size the worker count to `participants`
// and to keep the bench's pool-construction-per-cell shape consistent with
// every other competitor (each pool is constructed at runner entry and torn
// down on exit).
//
// Each runner mirrors the corresponding C++20 workload exactly (same N, same
// cutoff where applicable, same iteration / warmup counts) so the rows are
// apples-to-apples with citor / TBB / dispenso / libfork.
//
// TMC_IMPL must be defined in exactly one TU; this is that TU. The bench
// links a single tmc_headers INTERFACE target across the executable; no
// other TU includes tmc.

#define TMC_IMPL
#include "tmc/all_headers.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "citor/always_assert.h"

#include "aligned_alloc.h"
#include "tmc_runners.h"

namespace citor::bench {
namespace {

constexpr std::size_t kFibIterations = 50;
constexpr std::size_t kFibWarmupIterations = 5;

constexpr int kFibN = 28;
constexpr int kFibCutoff = 16;

constexpr int kQueensN = 12;
constexpr int kQueensRootDepth = 2;

constexpr std::size_t kUtsIterations = 25;
constexpr std::size_t kUtsWarmupIterations = 3;

[[nodiscard]] std::int64_t seqFib(int n) noexcept {
  if (n < 2) {
    return n;
  }
  std::int64_t a = 0;
  std::int64_t b = 1;
  for (int i = 2; i <= n; ++i) {
    const std::int64_t c = a + b;
    a = b;
    b = c;
  }
  return b;
}

tmc::task<std::int64_t> fibCoro(int n) {
  if (n <= kFibCutoff) {
    co_return seqFib(n);
  }
  auto [a, b] = co_await tmc::spawn_tuple(fibCoro(n - 1), fibCoro(n - 2));
  co_return a + b;
}

void seqQueensRec(int n, int row, std::uint64_t cols, std::uint64_t diag1,
                  std::uint64_t diag2, std::int64_t &count) noexcept {
  if (row == n) {
    ++count;
    return;
  }
  std::uint64_t bits = ~(cols | diag1 | diag2) &
                       ((std::uint64_t{1} << static_cast<unsigned>(n)) - 1);
  while (bits != 0U) {
    const std::uint64_t pick = bits & (~bits + 1U);
    bits ^= pick;
    seqQueensRec(n, row + 1, cols | pick, (diag1 | pick) << 1U,
                 (diag2 | pick) >> 1U, count);
  }
}

struct QueensRoot {
  std::uint64_t cols;
  std::uint64_t diag1;
  std::uint64_t diag2;
};

[[nodiscard]] std::vector<QueensRoot> buildQueensRoots(int n) {
  std::vector<QueensRoot> frontier{QueensRoot{0U, 0U, 0U}};
  for (int depth = 0; depth < kQueensRootDepth && !frontier.empty(); ++depth) {
    std::vector<QueensRoot> next;
    next.reserve(frontier.size() * static_cast<std::size_t>(n));
    for (const QueensRoot &s : frontier) {
      std::uint64_t bits = ~(s.cols | s.diag1 | s.diag2) &
                           ((std::uint64_t{1} << static_cast<unsigned>(n)) - 1);
      while (bits != 0U) {
        const std::uint64_t pick = bits & (~bits + 1U);
        bits ^= pick;
        next.push_back(
            {s.cols | pick, (s.diag1 | pick) << 1U, (s.diag2 | pick) >> 1U});
      }
    }
    frontier = std::move(next);
  }
  return frontier;
}

tmc::task<std::int64_t> queensRangeCoro(const QueensRoot *roots, std::size_t lo,
                                        std::size_t hi, int n) {
  if (hi - lo == 1) {
    const QueensRoot &s = roots[lo];
    std::int64_t count = 0;
    seqQueensRec(n, kQueensRootDepth, s.cols, s.diag1, s.diag2, count);
    co_return count;
  }
  const std::size_t mid = lo + ((hi - lo) / 2);
  auto [left, right] = co_await tmc::spawn_tuple(
      queensRangeCoro(roots, lo, mid, n), queensRangeCoro(roots, mid, hi, n));
  co_return left + right;
}

// ---------------------------------------------------------------------------
// UTS T1: SHA-1-driven RNG and tree shape duplicated from
// forkjoin_uts_bench.cpp (whose anonymous-namespace impl can't be exposed).
// ---------------------------------------------------------------------------

[[nodiscard]] constexpr std::uint32_t rotl32(std::uint32_t x,
                                             unsigned n) noexcept {
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

inline void sha1Compute(const std::uint8_t *data, std::size_t len,
                        std::array<std::uint8_t, 20> &out) noexcept {
  std::array<std::uint32_t, 5> h = {0x67452301U, 0xEFCDAB89U, 0x98BADCFEU,
                                    0x10325476U, 0xC3D2E1F0U};
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

struct UtsRngState {
  std::array<std::uint8_t, 20> bytes;
};

[[nodiscard]] inline UtsRngState rngInit(std::uint32_t seed) noexcept {
  std::array<std::uint8_t, 20> buf{};
  buf[16] = static_cast<std::uint8_t>((seed >> 24U) & 0xFFU);
  buf[17] = static_cast<std::uint8_t>((seed >> 16U) & 0xFFU);
  buf[18] = static_cast<std::uint8_t>((seed >> 8U) & 0xFFU);
  buf[19] = static_cast<std::uint8_t>(seed & 0xFFU);
  UtsRngState out{};
  sha1Compute(buf.data(), buf.size(), out.bytes);
  return out;
}

[[nodiscard]] inline UtsRngState rngSpawn(const UtsRngState &parent,
                                          std::uint32_t childIdx) noexcept {
  std::array<std::uint8_t, 24> buf{};
  std::memcpy(buf.data(), parent.bytes.data(), 20);
  buf[20] = static_cast<std::uint8_t>((childIdx >> 24U) & 0xFFU);
  buf[21] = static_cast<std::uint8_t>((childIdx >> 16U) & 0xFFU);
  buf[22] = static_cast<std::uint8_t>((childIdx >> 8U) & 0xFFU);
  buf[23] = static_cast<std::uint8_t>(childIdx & 0xFFU);
  UtsRngState out{};
  sha1Compute(buf.data(), buf.size(), out.bytes);
  return out;
}

[[nodiscard]] inline std::int32_t rngRand(const UtsRngState &s) noexcept {
  const std::uint32_t b = (static_cast<std::uint32_t>(s.bytes[16]) << 24U) |
                          (static_cast<std::uint32_t>(s.bytes[17]) << 16U) |
                          (static_cast<std::uint32_t>(s.bytes[18]) << 8U) |
                          static_cast<std::uint32_t>(s.bytes[19]);
  return static_cast<std::int32_t>(b & 0x7FFFFFFFU);
}

[[nodiscard]] inline double rngToProb(std::int32_t n) noexcept {
  return n < 0 ? 0.0 : static_cast<double>(n) / 2147483648.0;
}

constexpr int kRootB0 = 4;
constexpr int kMaxDepth = 10;
constexpr std::uint32_t kRootSeed = 19U;
constexpr std::int64_t kExpectedNodes = 4'130'071;
constexpr int kSeqCutoffDepth = 5;

[[nodiscard]] inline int utsNumChildrenGeo(const UtsRngState &state,
                                           int depth) noexcept {
  const double bI = (depth < kMaxDepth) ? static_cast<double>(kRootB0) : 0.0;
  const double p = 1.0 / (1.0 + bI);
  const std::int32_t h = rngRand(state);
  const double u = rngToProb(h);
  if (bI <= 0.0) {
    return 0;
  }
  return static_cast<int>(std::floor(std::log(1.0 - u) / std::log(1.0 - p)));
}

[[nodiscard]] std::int64_t utsSeqWalk(const UtsRngState &state,
                                      int depth) noexcept {
  std::int64_t count = 1;
  const int n = utsNumChildrenGeo(state, depth);
  for (int i = 0; i < n; ++i) {
    const UtsRngState child = rngSpawn(state, static_cast<std::uint32_t>(i));
    count += utsSeqWalk(child, depth + 1);
  }
  return count;
}

tmc::task<std::int64_t> utsCoro(UtsRngState state, int depth) {
  if (depth >= kSeqCutoffDepth) {
    co_return utsSeqWalk(state, depth);
  }
  const int n = utsNumChildrenGeo(state, depth);
  if (n == 0) {
    co_return 1;
  }
  if (n == 1) {
    const UtsRngState child = rngSpawn(state, 0U);
    std::int64_t childCount = co_await utsCoro(child, depth + 1);
    co_return 1 + childCount;
  }
  // N-way fan-out via spawn_many, matching libfork's published TMC bench
  // (build/_deps/libfork-src/bench/source/uts/tmc.cpp:46) and citor's own
  // forkJoinAll-driven N-way parWalk. Bisecting at the midpoint caps task-
  // graph parallelism at 2^depth, while N-way exposes b0^depth.
  std::vector<tmc::task<std::int64_t>> children;
  children.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    const UtsRngState child = rngSpawn(state, static_cast<std::uint32_t>(i));
    children.emplace_back(utsCoro(child, depth + 1));
  }
  std::vector<std::int64_t> results =
      co_await tmc::spawn_many(children.data(), static_cast<std::size_t>(n));
  std::int64_t total = 1;
  for (std::int64_t v : results) {
    total += v;
  }
  co_return total;
}

// ---------------------------------------------------------------------------
// Pool lifecycle helper. tmc's `cpu_executor()` is a process-wide singleton;
// the runner inits it with a specific worker count, runs all measurements,
// and tears it down on exit so the next runner sees a clean state.
// ---------------------------------------------------------------------------

class TmcExecutorScope {
public:
  explicit TmcExecutorScope(std::size_t participants) {
    // HIERARCHY_MATRIX is TMC's default and is the right choice when there
    // is no hwloc-backed topology (without TMC_USE_HWLOC every thread lands
    // in one flat CacheGroup, so HIERARCHY and LATTICE both degenerate to
    // the same Latin-square). Empirically LATTICE's fallback is bimodal at
    // j16 (median 349us vs HIERARCHY's 106us). Diagnosed via perf stat:
    // tmc::post_waitable + std::future::get() parks the producer on a futex
    // each iteration; at j=16 producer + workers race for 16 logical CPUs
    // and we see 26x more context switches (91748 vs 3480 at j8). HIERARCHY
    // recovers most of the slowdown; the structural fix would be to call
    // tmc::external::sync_await rather than post_waitable+future.get().
    tmc::cpu_executor()
        .set_thread_count(participants)
        .set_work_stealing_strategy(
            tmc::work_stealing_strategy::HIERARCHY_MATRIX);
    tmc::cpu_executor().init();
  }
  ~TmcExecutorScope() { tmc::cpu_executor().teardown(); }

  TmcExecutorScope(const TmcExecutorScope &) = delete;
  TmcExecutorScope(TmcExecutorScope &&) = delete;
  TmcExecutorScope &operator=(const TmcExecutorScope &) = delete;
  TmcExecutorScope &operator=(TmcExecutorScope &&) = delete;
};

} // namespace

tmc::task<std::int64_t> fibCutoffCoro(int n, int cutoff) {
  if (n <= cutoff) {
    co_return seqFib(n);
  }
  // Mirror tzcnt's published shape (bench/source/fib/tmc.cpp:18-21):
  // fork() one branch (returns an already-submitted awaitable), serially
  // run the other branch via co_await on the calling coroutine, then await
  // the fork. tzcnt's bench uses `run_early()` against an older TMC API;
  // the v1.4 equivalent on `aw_spawn` is `.fork()` (`spawn.hpp:351`).
  auto xt = tmc::spawn(fibCutoffCoro(n - 1, cutoff)).fork();
  std::int64_t y = co_await fibCutoffCoro(n - 2, cutoff);
  std::int64_t x = co_await std::move(xt);
  co_return x + y;
}

BenchRow runTmcFibFine(std::size_t participants, int n, int cutoff,
                       const CyclesPerNanosecond &cal) {
  TmcExecutorScope scope{participants};
  std::atomic<std::int64_t> sink{0};
  for (std::size_t i = 0; i < kFibWarmupIterations; ++i) {
    sink.store(
        tmc::post_waitable(tmc::cpu_executor(), fibCutoffCoro(n, cutoff)).get(),
        std::memory_order_relaxed);
  }
  std::vector<double> samples;
  samples.reserve(kFibIterations);
  for (std::size_t i = 0; i < kFibIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    const std::int64_t value =
        tmc::post_waitable(tmc::cpu_executor(), fibCutoffCoro(n, cutoff)).get();
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    sink.store(value, std::memory_order_relaxed);
  }
  (void)sink.load(std::memory_order_relaxed);
  return finalizeRow("tmc::cpu_executor", samples);
}

BenchRow runTmcFib28(std::size_t participants, const CyclesPerNanosecond &cal) {
  TmcExecutorScope scope{participants};
  std::atomic<std::int64_t> sink{0};
  for (std::size_t i = 0; i < kFibWarmupIterations; ++i) {
    sink.store(tmc::post_waitable(tmc::cpu_executor(), fibCoro(kFibN)).get(),
               std::memory_order_relaxed);
  }
  std::vector<double> samples;
  samples.reserve(kFibIterations);
  for (std::size_t i = 0; i < kFibIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    const std::int64_t value =
        tmc::post_waitable(tmc::cpu_executor(), fibCoro(kFibN)).get();
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    sink.store(value, std::memory_order_relaxed);
  }
  (void)sink.load(std::memory_order_relaxed);
  return finalizeRow("tmc::cpu_executor", samples);
}

BenchRow runTmcNQueens12(std::size_t participants,
                         const CyclesPerNanosecond &cal) {
  TmcExecutorScope scope{participants};
  const std::vector<QueensRoot> roots = buildQueensRoots(kQueensN);

  auto runOnce = [&]() -> std::int64_t {
    if (roots.empty()) {
      return 0;
    }
    return tmc::post_waitable(
               tmc::cpu_executor(),
               queensRangeCoro(roots.data(), 0, roots.size(), kQueensN))
        .get();
  };

  std::atomic<std::int64_t> sink{0};
  for (std::size_t i = 0; i < kFibWarmupIterations; ++i) {
    sink.store(runOnce(), std::memory_order_relaxed);
  }
  std::vector<double> samples;
  samples.reserve(kFibIterations);
  for (std::size_t i = 0; i < kFibIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    const std::int64_t value = runOnce();
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    sink.store(value, std::memory_order_relaxed);
  }
  (void)sink.load(std::memory_order_relaxed);
  return finalizeRow("tmc::cpu_executor", samples);
}

BenchRow runTmcUtsT1(std::size_t participants, const CyclesPerNanosecond &cal) {
  TmcExecutorScope scope{participants};
  const UtsRngState root = rngInit(kRootSeed);

  auto runOnce = [&]() -> std::int64_t {
    return tmc::post_waitable(tmc::cpu_executor(), utsCoro(root, 0)).get();
  };

  std::int64_t parallelCount = 0;
  for (std::size_t i = 0; i < kUtsWarmupIterations; ++i) {
    parallelCount = runOnce();
  }
  BENCH_CHECK_OR_THROW(parallelCount == kExpectedNodes, "tmc_runners.cpp");

  std::vector<double> samples;
  samples.reserve(kUtsIterations);
  for (std::size_t i = 0; i < kUtsIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    const std::int64_t count = runOnce();
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    BENCH_CHECK_OR_THROW(count == kExpectedNodes, "tmc_runners.cpp");
  }
  return finalizeRow("tmc::cpu_executor", samples);
}

// ---------------------------------------------------------------------------
// Strassen and cilksort: same algorithms as the C++20 references in
// forkjoin_strassen_bench.cpp / forkjoin_cilksort_bench.cpp. Sub-product /
// merge state types are duplicated here (anonymous-namespace impl details
// in the canonical TUs cannot be exposed); the duplication is bounded
// (one algorithm shape, no data-dependent drift) and keeps the TMC TU
// self-contained.
// ---------------------------------------------------------------------------

namespace strassen_tmc {

constexpr std::size_t kSeqCutoff = 64;
constexpr std::size_t kStrassenParallelDepth = 1;
constexpr std::size_t kStrassenIterations = 10;
constexpr std::size_t kStrassenWarmupIterations = 2;

struct Sub {
  float *data;
  std::size_t stride;
  std::size_t n;
};

struct AlignedFloatDeleter {
  void operator()(float *p) const noexcept { alignedFree(p); }
};

using AlignedFloatBuffer = std::unique_ptr<float[], AlignedFloatDeleter>;

inline AlignedFloatBuffer allocateAlignedFloats(std::size_t count) {
  const std::size_t bytes = ((count * sizeof(float) + 63U) / 64U) * 64U;
  void *raw = alignedAlloc(bytes, 64U);
  if (raw == nullptr) {
    std::abort();
  }
  return AlignedFloatBuffer{static_cast<float *>(raw)};
}

inline void deterministicFill(float *p, std::size_t count,
                              std::uint32_t seed) noexcept {
  std::mt19937 rng{seed};
  std::uniform_real_distribution<float> dist(-1.0F, 1.0F);
  for (std::size_t i = 0; i < count; ++i) {
    p[i] = dist(rng);
  }
}

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

inline void addEq(const Sub &dst, const Sub &a) noexcept {
  for (std::size_t i = 0; i < dst.n; ++i) {
    const float *aRow = a.data + (i * a.stride);
    float *dRow = dst.data + (i * dst.stride);
    for (std::size_t j = 0; j < dst.n; ++j) {
      dRow[j] += aRow[j];
    }
  }
}

inline void subEq(const Sub &dst, const Sub &a) noexcept {
  for (std::size_t i = 0; i < dst.n; ++i) {
    const float *aRow = a.data + (i * a.stride);
    float *dRow = dst.data + (i * dst.stride);
    for (std::size_t j = 0; j < dst.n; ++j) {
      dRow[j] -= aRow[j];
    }
  }
}

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

[[nodiscard]] inline Sub quadrant(const Sub &m, std::size_t row,
                                  std::size_t col) noexcept {
  const std::size_t half = m.n / 2U;
  return Sub{.data = m.data + (row * half * m.stride) + (col * half),
             .stride = m.stride,
             .n = half};
}

[[nodiscard]] constexpr std::size_t scratchBudget(std::size_t n,
                                                  std::size_t depth) noexcept {
  if (n <= kSeqCutoff) {
    return 0U;
  }
  const std::size_t half = n / 2U;
  const std::size_t levelSize = 17U * (half * half);
  const std::size_t childMul = depth < kStrassenParallelDepth ? 7U : 1U;
  return levelSize + (childMul * scratchBudget(half, depth + 1U));
}

[[nodiscard]] inline float strassenTolerance(std::size_t n) noexcept {
  constexpr double kEpsFloat = 1.1920929e-7;
  constexpr double kHighamScale = 1.5;
  const double bound = kHighamScale *
                       std::pow(static_cast<double>(n), 2.8073549220576041) *
                       kEpsFloat;
  return static_cast<float>(bound);
}

tmc::task<void> strassenCoro(Sub c, Sub a, Sub b, float *scratch,
                             std::size_t depth) {
  if (c.n <= kSeqCutoff) {
    seqMatmul(c, a, b);
    co_return;
  }
  const std::size_t half = c.n / 2U;
  const std::size_t halfSize = half * half;

  float *cursor = scratch;
  auto take = [&cursor, halfSize]() {
    float *out = cursor;
    cursor += halfSize;
    return out;
  };
  std::array<Sub, 7> mBufs{};
  for (std::size_t k = 0; k < 7; ++k) {
    mBufs[k] = Sub{.data = take(), .stride = half, .n = half};
  }
  Sub t1A{take(), half, half};
  Sub t1B{take(), half, half};
  Sub t2A{take(), half, half};
  Sub t3B{take(), half, half};
  Sub t4B{take(), half, half};
  Sub t5A{take(), half, half};
  Sub t6A{take(), half, half};
  Sub t6B{take(), half, half};
  Sub t7A{take(), half, half};
  Sub t7B{take(), half, half};
  float *childScratch = cursor;

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

  const std::size_t childBudget = scratchBudget(half, depth + 1U);

  if (depth < kStrassenParallelDepth) {
    co_await tmc::spawn_tuple(
        strassenCoro(mBufs[0], t1A, t1B, childScratch, depth + 1U),
        strassenCoro(mBufs[1], t2A, b11, childScratch + childBudget,
                     depth + 1U),
        strassenCoro(mBufs[2], a11, t3B, childScratch + (2U * childBudget),
                     depth + 1U),
        strassenCoro(mBufs[3], a22, t4B, childScratch + (3U * childBudget),
                     depth + 1U),
        strassenCoro(mBufs[4], t5A, b22, childScratch + (4U * childBudget),
                     depth + 1U),
        strassenCoro(mBufs[5], t6A, t6B, childScratch + (5U * childBudget),
                     depth + 1U),
        strassenCoro(mBufs[6], t7A, t7B, childScratch + (6U * childBudget),
                     depth + 1U));
  } else {
    co_await strassenCoro(mBufs[0], t1A, t1B, childScratch, depth + 1U);
    co_await strassenCoro(mBufs[1], t2A, b11, childScratch, depth + 1U);
    co_await strassenCoro(mBufs[2], a11, t3B, childScratch, depth + 1U);
    co_await strassenCoro(mBufs[3], a22, t4B, childScratch, depth + 1U);
    co_await strassenCoro(mBufs[4], t5A, b22, childScratch, depth + 1U);
    co_await strassenCoro(mBufs[5], t6A, t6B, childScratch, depth + 1U);
    co_await strassenCoro(mBufs[6], t7A, t7B, childScratch, depth + 1U);
  }

  addInto(c11, mBufs[0], mBufs[3]);
  subEq(c11, mBufs[4]);
  addEq(c11, mBufs[6]);

  addInto(c12, mBufs[2], mBufs[4]);
  addInto(c21, mBufs[1], mBufs[3]);

  subInto(c22, mBufs[0], mBufs[1]);
  addEq(c22, mBufs[2]);
  addEq(c22, mBufs[5]);
}

} // namespace strassen_tmc

namespace cilksort_tmc {

constexpr std::size_t kSeqCutoff = 256;
constexpr std::size_t kMergeBuckets = 64;
constexpr std::size_t kCilksortIterations = 25;
constexpr std::size_t kCilksortWarmupIterations = 3;

[[nodiscard]] inline std::vector<std::int32_t> buildInput(std::size_t n) {
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

inline void serialMerge(std::int32_t *src, std::size_t aLo, std::size_t aHi,
                        std::size_t bLo, std::size_t bHi, std::int32_t *dst,
                        std::size_t outLo) {
  const std::size_t nA = aHi - aLo;
  const std::size_t nB = bHi - bLo;
  const std::size_t total = nA + nB;
  if (total == 0) {
    return;
  }
  if (total < kSeqCutoff) {
    seqMerge(src, aLo, aHi, bLo, bHi, dst, outLo);
    return;
  }
  const bool aIsLarger = nA >= nB;
  const std::size_t primLo = aIsLarger ? aLo : bLo;
  const std::size_t primHi = aIsLarger ? aHi : bHi;
  const std::size_t secLo = aIsLarger ? bLo : aLo;
  const std::size_t secHi = aIsLarger ? bHi : aHi;
  const std::size_t nPrim = primHi - primLo;
  std::array<std::size_t, kMergeBuckets + 1U> primSplit{};
  std::array<std::size_t, kMergeBuckets + 1U> secSplit{};
  primSplit[0] = primLo;
  primSplit[kMergeBuckets] = primHi;
  secSplit[0] = secLo;
  secSplit[kMergeBuckets] = secHi;
  for (std::size_t k = 1; k < kMergeBuckets; ++k) {
    primSplit[k] = primLo + ((nPrim * k) / kMergeBuckets);
    const std::int32_t key = src[primSplit[k]];
    secSplit[k] = static_cast<std::size_t>(
        std::lower_bound(src + secLo, src + secHi, key) - src);
  }
  std::size_t running = outLo;
  for (std::size_t k = 0; k < kMergeBuckets; ++k) {
    const std::size_t aLoK = aIsLarger ? primSplit[k] : secSplit[k];
    const std::size_t aHiK = aIsLarger ? primSplit[k + 1U] : secSplit[k + 1U];
    const std::size_t bLoK = aIsLarger ? secSplit[k] : primSplit[k];
    const std::size_t bHiK = aIsLarger ? secSplit[k + 1U] : primSplit[k + 1U];
    running += seqMerge(src, aLoK, aHiK, bLoK, bHiK, dst, running);
  }
}

tmc::task<void> cilksortCoro(std::int32_t *data, std::int32_t *tmp,
                             std::size_t lo, std::size_t hi) {
  const std::size_t n = hi - lo;
  if (n <= kSeqCutoff) {
    std::sort(data + lo, data + hi);
    co_return;
  }
  const std::size_t mid = lo + (n / 2U);
  co_await tmc::spawn_tuple(cilksortCoro(data, tmp, lo, mid),
                            cilksortCoro(data, tmp, mid, hi));
  serialMerge(data, lo, mid, mid, hi, tmp, lo);
  std::copy(tmp + lo, tmp + hi, data + lo);
}

} // namespace cilksort_tmc

BenchRow runTmcStrassen(std::size_t participants, std::size_t n,
                        const CyclesPerNanosecond &cal) {
  using namespace strassen_tmc;
  TmcExecutorScope scope{participants};

  AlignedFloatBuffer aBuf = allocateAlignedFloats(n * n);
  AlignedFloatBuffer bBuf = allocateAlignedFloats(n * n);
  AlignedFloatBuffer cBuf = allocateAlignedFloats(n * n);
  AlignedFloatBuffer refBuf = allocateAlignedFloats(n * n);
  deterministicFill(aBuf.get(), n * n, 0xc1701U);
  deterministicFill(bBuf.get(), n * n, 0xc1701U + 1U);
  const Sub aSub{aBuf.get(), n, n};
  const Sub bSub{bBuf.get(), n, n};
  const Sub cSub{cBuf.get(), n, n};
  const Sub refSub{refBuf.get(), n, n};
  seqMatmul(refSub, aSub, bSub);
  const std::size_t scratchN = scratchBudget(n, 0U);
  std::vector<float> scratch(scratchN, 0.0F);

  auto verify = [&]() {
    float maxDiff = 0.0F;
    const std::size_t total = n * n;
    for (std::size_t i = 0; i < total; ++i) {
      const float diff = std::fabs(cBuf.get()[i] - refBuf.get()[i]);
      if (diff > maxDiff) {
        maxDiff = diff;
      }
    }
    const float tolerance = strassenTolerance(n);
    BENCH_CHECK_OR_THROW(maxDiff <= tolerance, "tmc_runners.cpp");
  };

  for (std::size_t i = 0; i < kStrassenWarmupIterations; ++i) {
    tmc::post_waitable(tmc::cpu_executor(),
                       strassenCoro(cSub, aSub, bSub, scratch.data(), 0U))
        .wait();
    verify();
  }
  std::vector<double> samples;
  samples.reserve(kStrassenIterations);
  for (std::size_t i = 0; i < kStrassenIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    tmc::post_waitable(tmc::cpu_executor(),
                       strassenCoro(cSub, aSub, bSub, scratch.data(), 0U))
        .wait();
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    verify();
  }
  return finalizeRow("tmc::cpu_executor", samples);
}

BenchRow runTmcCilksort(std::size_t participants, std::size_t n,
                        const CyclesPerNanosecond &cal) {
  using namespace cilksort_tmc;
  TmcExecutorScope scope{participants};
  const std::vector<std::int32_t> input = buildInput(n);
  std::vector<std::int32_t> reference = input;
  std::sort(reference.begin(), reference.end());
  std::vector<std::int32_t> data(n, std::int32_t{0});
  std::vector<std::int32_t> tmp(n, std::int32_t{0});

  for (std::size_t i = 0; i < kCilksortWarmupIterations; ++i) {
    data = input;
    tmc::post_waitable(tmc::cpu_executor(),
                       cilksortCoro(data.data(), tmp.data(), 0U, n))
        .wait();
    BENCH_CHECK_OR_THROW(data == reference, "tmc_runners.cpp");
  }

  std::vector<double> samples;
  samples.reserve(kCilksortIterations);
  for (std::size_t i = 0; i < kCilksortIterations; ++i) {
    data = input;
    const std::uint64_t startCycles = readCyclesStart();
    tmc::post_waitable(tmc::cpu_executor(),
                       cilksortCoro(data.data(), tmp.data(), 0U, n))
        .wait();
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    BENCH_CHECK_OR_THROW(data == reference, "tmc_runners.cpp");
  }
  return finalizeRow("tmc::cpu_executor", samples);
}

// ---------------------------------------------------------------------------
// matmul DAC: same shape as forkjoin_matmul_dac_bench.cpp / libfork's
// reference. Two phases per level (overwrite + accumulate), 4-way fan-out
// per phase via spawn_tuple.
// ---------------------------------------------------------------------------

namespace matmul_dac_tmc {

constexpr std::size_t kSeqCutoff = 64;
constexpr std::size_t kIterations = 10;
constexpr std::size_t kWarmupIterations = 2;

struct AlignedFloatDeleter {
  void operator()(float *p) const noexcept { alignedFree(p); }
};

using AlignedFloatBuffer = std::unique_ptr<float[], AlignedFloatDeleter>;

inline AlignedFloatBuffer allocateAlignedFloats(std::size_t count) {
  const std::size_t bytes = ((count * sizeof(float) + 63U) / 64U) * 64U;
  void *raw = alignedAlloc(bytes, 64U);
  if (raw == nullptr) {
    std::abort();
  }
  return AlignedFloatBuffer{static_cast<float *>(raw)};
}

inline void deterministicFill(float *p, std::size_t count,
                              std::uint32_t seed) noexcept {
  std::mt19937 rng{seed};
  std::uniform_real_distribution<float> dist(-1.0F, 1.0F);
  for (std::size_t i = 0; i < count; ++i) {
    p[i] = dist(rng);
  }
}

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

tmc::task<void> matmulCoro(const float *A, const float *B, float *R,
                           std::size_t n, std::size_t stride, bool add) {
  if (n <= kSeqCutoff) {
    leafMultiply(A, B, R, n, stride, add);
    co_return;
  }
  const std::size_t m = n / 2U;
  const std::size_t o00 = 0;
  const std::size_t o01 = m;
  const std::size_t o10 = m * stride;
  const std::size_t o11 = (m * stride) + m;

  // Phase 1: overwrite, 4 sub-products into disjoint quadrants.
  co_await tmc::spawn_tuple(
      matmulCoro(A + o00, B + o00, R + o00, m, stride, add),
      matmulCoro(A + o00, B + o01, R + o01, m, stride, add),
      matmulCoro(A + o10, B + o00, R + o10, m, stride, add),
      matmulCoro(A + o10, B + o01, R + o11, m, stride, add));

  // Phase 2: accumulate into the same quadrants.
  co_await tmc::spawn_tuple(
      matmulCoro(A + o01, B + o10, R + o00, m, stride, true),
      matmulCoro(A + o01, B + o11, R + o01, m, stride, true),
      matmulCoro(A + o11, B + o10, R + o10, m, stride, true),
      matmulCoro(A + o11, B + o11, R + o11, m, stride, true));
}

} // namespace matmul_dac_tmc

BenchRow runTmcMatmulDac(std::size_t participants, std::size_t n,
                         const CyclesPerNanosecond &cal) {
  using namespace matmul_dac_tmc;
  TmcExecutorScope scope{participants};

  AlignedFloatBuffer aBuf = allocateAlignedFloats(n * n);
  AlignedFloatBuffer bBuf = allocateAlignedFloats(n * n);
  AlignedFloatBuffer cBuf = allocateAlignedFloats(n * n);
  AlignedFloatBuffer refBuf = allocateAlignedFloats(n * n);
  deterministicFill(aBuf.get(), n * n, 0xc1701U);
  deterministicFill(bBuf.get(), n * n, 0xc1701U + 1U);
  leafMultiply(aBuf.get(), bBuf.get(), refBuf.get(), n, n, /*add=*/false);

  const float tolerance = 1e-3F * static_cast<float>(n);
  auto verify = [&]() {
    float maxDiff = 0.0F;
    const std::size_t total = n * n;
    for (std::size_t i = 0; i < total; ++i) {
      const float diff = std::fabs(cBuf.get()[i] - refBuf.get()[i]);
      if (diff > maxDiff) {
        maxDiff = diff;
      }
    }
    BENCH_CHECK_OR_THROW(maxDiff <= tolerance, "tmc_runners.cpp");
  };

  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    tmc::post_waitable(
        tmc::cpu_executor(),
        matmulCoro(aBuf.get(), bBuf.get(), cBuf.get(), n, n, /*add=*/false))
        .wait();
    verify();
  }
  std::vector<double> samples;
  samples.reserve(kIterations);
  for (std::size_t i = 0; i < kIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    tmc::post_waitable(
        tmc::cpu_executor(),
        matmulCoro(aBuf.get(), bBuf.get(), cBuf.get(), n, n, /*add=*/false))
        .wait();
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    verify();
  }
  return finalizeRow("tmc::cpu_executor", samples);
}

// ---------------------------------------------------------------------------
// Skynet: 10-way fanout, depth 6. Uses spawn_many over a vector of N tasks
// to mirror tzcnt's own bench shape.
// ---------------------------------------------------------------------------

namespace skynet_tmc {

constexpr int kFanout = 10;
constexpr int kDepth = 6;
constexpr std::int64_t kLeafCount = 1'000'000;
constexpr std::int64_t kExpectedSum = (kLeafCount * (kLeafCount - 1)) / 2;
constexpr std::size_t kIterations = 25;
constexpr std::size_t kWarmupIterations = 3;

tmc::task<std::int64_t> skynetCoro(std::int64_t label, int depth) {
  if (depth == 0) {
    co_return label;
  }
  const std::int64_t base = label * kFanout;
  std::vector<tmc::task<std::int64_t>> children;
  children.reserve(static_cast<std::size_t>(kFanout));
  for (int i = 0; i < kFanout; ++i) {
    children.emplace_back(
        skynetCoro(base + static_cast<std::int64_t>(i), depth - 1));
  }
  std::vector<std::int64_t> results =
      co_await tmc::spawn_many(children.data(), kFanout);
  std::int64_t total = 0;
  for (const std::int64_t v : results) {
    total += v;
  }
  co_return total;
}

} // namespace skynet_tmc

BenchRow runTmcSkynet(std::size_t participants,
                      const CyclesPerNanosecond &cal) {
  using namespace skynet_tmc;
  TmcExecutorScope scope{participants};

  auto runOnce = [&]() -> std::int64_t {
    return tmc::post_waitable(tmc::cpu_executor(),
                              skynetCoro(std::int64_t{0}, kDepth))
        .get();
  };

  std::int64_t result = 0;
  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    result = runOnce();
  }
  BENCH_CHECK_OR_THROW(result == kExpectedSum, "tmc_runners.cpp");

  std::vector<double> samples;
  samples.reserve(kIterations);
  for (std::size_t i = 0; i < kIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    result = runOnce();
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    BENCH_CHECK_OR_THROW(result == kExpectedSum, "tmc_runners.cpp");
  }
  return finalizeRow("tmc::cpu_executor", samples);
}

} // namespace citor::bench
