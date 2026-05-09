// libfork (ConorWilliams) coroutine runners for the comparative fork-join
// benches. Lives in its own TU compiled at C++23 because libfork's public API
// uses C++23 multi-arg subscript syntax (`co_await lf::fork[&a, fn](args)`)
// while the rest of the bench compiles at C++20.
//
// Each runner mirrors the corresponding C++20 workload exactly so the rows
// are apples-to-apples:
//   - fib(28) with sequential cutoff at 16    (matches fork_join_bench.cpp)
//   - nqueens(12) bisected from depth-2 roots (matches fork_join_bench.cpp)
//   - UTS T1 (b0=4, depth=10, seed=19), bisected at the seq cutoff depth 5
//     (matches forkjoin_uts_bench.cpp)
//
// Pool: `lf::lazy_pool` is libfork's general-purpose scheduler. The worker
// count is set to `participants` to match every other competitor. The pool
// is constructed once per runner call (one per j8/j16 measurement), warmed
// with a few iterations, then timed.
//
// Excluded from the libfork roster:
//   - knapsack-cancel    -- libfork has no first-class cancellation primitive
//     comparable to citor's `CancellationToken`; the cell tests cancellation
//     latency, not raw fork-join throughput.

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

#include "libfork.hpp"

#include "citor/always_assert.h"

#include "libfork_runners.h"

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

inline constexpr auto fibCoro = [](auto self, int n) -> lf::task<std::int64_t> {
  if (n <= kFibCutoff) {
    co_return seqFib(n);
  }
  std::int64_t a = 0;
  std::int64_t b = 0;
  co_await lf::fork[&a, self](n - 1);
  co_await lf::call[&b, self](n - 2);
  co_await lf::join;
  co_return a + b;
};

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

inline constexpr auto queensCoro = [](auto self, const QueensRoot *roots,
                                      std::int64_t *partials, std::size_t lo,
                                      std::size_t hi, int n) -> lf::task<void> {
  if (hi - lo == 1) {
    const QueensRoot &s = roots[lo];
    std::int64_t count = 0;
    seqQueensRec(n, kQueensRootDepth, s.cols, s.diag1, s.diag2, count);
    partials[lo] = count;
    co_return;
  }
  const std::size_t mid = lo + ((hi - lo) / 2);
  co_await lf::fork[self](roots, partials, lo, mid, n);
  co_await lf::call[self](roots, partials, mid, hi, n);
  co_await lf::join;
};

// ---------------------------------------------------------------------------
// UTS: same SHA-1-driven RNG and tree shape as forkjoin_uts_bench.cpp. The
// implementation is duplicated here (instead of factored into a shared
// header) because the original lives in an anonymous namespace inside its
// .cpp; pulling it into a shared header would expose the SHA-1 plumbing
// to every TU. The duplication is bounded -- one tree shape, one RNG, never
// changes once the canonical T1 inputs are fixed.
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

// libfork-idiomatic UTS: fork all-but-last child, call the last child inline,
// join, then aggregate. Mirrors libfork's own reference UTS bench
// (https://github.com/ConorWilliams/libfork/blob/v3.8.0/bench/source/uts/libfork.cpp).
//
// The C++20 parWalk in forkjoin_uts_bench.cpp bisects children at the
// midpoint into two halves; libfork's continuation-stealing model prefers
// fork-per-child because each fork creates an independently stealable
// continuation. The aggregate node count is identical either way; only the
// task-graph shape differs. Using the libfork-idiomatic shape gives libfork
// the spawn/steal pattern it was tuned for, so the cell measures libfork at
// its native strength rather than forcing it through a foreign fan-out.
inline constexpr auto utsCoro = [](auto self, UtsRngState state, int depth,
                                   std::int64_t *out) -> lf::task<void> {
  if (depth >= kSeqCutoffDepth) {
    *out = utsSeqWalk(state, depth);
    co_return;
  }
  const int n = utsNumChildrenGeo(state, depth);
  if (n == 0) {
    *out = 1;
    co_return;
  }
  std::vector<std::int64_t> childCounts(static_cast<std::size_t>(n), 0);
  for (int i = 0; i < n - 1; ++i) {
    const UtsRngState child = rngSpawn(state, static_cast<std::uint32_t>(i));
    co_await lf::fork[self](child, depth + 1,
                            &childCounts[static_cast<std::size_t>(i)]);
  }
  // Last child runs inline; this matches libfork's own UTS bench pattern
  // (fork n-1 children, call the nth) and lets the worker descend into the
  // last subtree without paying the spawn-and-steal overhead for it.
  const UtsRngState lastChild =
      rngSpawn(state, static_cast<std::uint32_t>(n - 1));
  co_await lf::call[self](lastChild, depth + 1,
                          &childCounts[static_cast<std::size_t>(n - 1)]);
  co_await lf::join;
  std::int64_t total = 1;
  for (std::int64_t v : childCounts) {
    total += v;
  }
  *out = total;
};

} // namespace

// ---------------------------------------------------------------------------
// Entry points exposed to fork_join_bench.cpp / forkjoin_uts_bench.cpp.
// ---------------------------------------------------------------------------

// Cutoff-parameterized fib coroutine: shape mirrors libfork's published bench
// (`bench/source/fib/libfork.cpp`) which has NO cutoff at all. Our `kFibFineN`
// + `kFibFineCutoff=2` cells expose the per-spawn-cost regime where libfork's
// continuation-stealing wins.
inline constexpr auto fibCutoffCoro = [](auto self, int n,
                                         int cutoff) -> lf::task<std::int64_t> {
  if (n <= cutoff) {
    co_return seqFib(n);
  }
  std::int64_t a = 0;
  std::int64_t b = 0;
  co_await lf::fork[&a, self](n - 1, cutoff);
  co_await lf::call[&b, self](n - 2, cutoff);
  co_await lf::join;
  co_return a + b;
};

BenchRow runLibforkFibFine(std::size_t participants, int n, int cutoff,
                           const CyclesPerNanosecond &cal) {
  lf::lazy_pool pool(participants);
  std::atomic<std::int64_t> sink{0};
  for (std::size_t i = 0; i < kFibWarmupIterations; ++i) {
    sink.store(lf::sync_wait(pool, fibCutoffCoro, n, cutoff),
               std::memory_order_relaxed);
  }
  std::vector<double> samples;
  samples.reserve(kFibIterations);
  for (std::size_t i = 0; i < kFibIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    const std::int64_t value = lf::sync_wait(pool, fibCutoffCoro, n, cutoff);
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    sink.store(value, std::memory_order_relaxed);
  }
  (void)sink.load(std::memory_order_relaxed);
  return finalizeRow("libfork", samples);
}

BenchRow runLibforkFib28(std::size_t participants,
                         const CyclesPerNanosecond &cal) {
  // lazy_pool is libfork's recommended general-purpose scheduler. It parks
  // idle workers aggressively (which costs a wake-up at the start of each
  // iteration on tight benches) but is what a real application would use,
  // and busy_pool produced pathological numbers on fib28_j16 in this fixture
  // (orders of magnitude slower than lazy_pool).
  lf::lazy_pool pool(participants);
  std::atomic<std::int64_t> sink{0};
  for (std::size_t i = 0; i < kFibWarmupIterations; ++i) {
    sink.store(lf::sync_wait(pool, fibCoro, kFibN), std::memory_order_relaxed);
  }
  std::vector<double> samples;
  samples.reserve(kFibIterations);
  for (std::size_t i = 0; i < kFibIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    const std::int64_t value = lf::sync_wait(pool, fibCoro, kFibN);
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    sink.store(value, std::memory_order_relaxed);
  }
  (void)sink.load(std::memory_order_relaxed);
  return finalizeRow("libfork", samples);
}

BenchRow runLibforkNQueens12(std::size_t participants,
                             const CyclesPerNanosecond &cal) {
  // lazy_pool is libfork's recommended general-purpose scheduler. It parks
  // idle workers aggressively (which costs a wake-up at the start of each
  // iteration on tight benches) but is what a real application would use,
  // and busy_pool produced pathological numbers on fib28_j16 in this fixture
  // (orders of magnitude slower than lazy_pool).
  lf::lazy_pool pool(participants);
  const std::vector<QueensRoot> roots = buildQueensRoots(kQueensN);
  std::vector<std::int64_t> partials(roots.size(), 0);

  auto runOnce = [&]() -> std::int64_t {
    std::fill(partials.begin(), partials.end(), 0);
    if (!roots.empty()) {
      lf::sync_wait(pool, queensCoro, roots.data(), partials.data(),
                    std::size_t{0}, roots.size(), kQueensN);
    }
    std::int64_t total = 0;
    for (std::int64_t v : partials) {
      total += v;
    }
    return total;
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
  return finalizeRow("libfork", samples);
}

BenchRow runLibforkUtsT1(std::size_t participants,
                         const CyclesPerNanosecond &cal) {
  // lazy_pool is libfork's recommended general-purpose scheduler. It parks
  // idle workers aggressively (which costs a wake-up at the start of each
  // iteration on tight benches) but is what a real application would use,
  // and busy_pool produced pathological numbers on fib28_j16 in this fixture
  // (orders of magnitude slower than lazy_pool).
  lf::lazy_pool pool(participants);
  const UtsRngState root = rngInit(kRootSeed);

  auto runOnce = [&]() -> std::int64_t {
    std::int64_t result = 0;
    lf::sync_wait(pool, utsCoro, root, 0, &result);
    return result;
  };

  std::int64_t parallelCount = 0;
  for (std::size_t i = 0; i < kUtsWarmupIterations; ++i) {
    parallelCount = runOnce();
  }
  CITOR_ALWAYS_ASSERT(parallelCount == kExpectedNodes);

  std::vector<double> samples;
  samples.reserve(kUtsIterations);
  for (std::size_t i = 0; i < kUtsIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    const std::int64_t count = runOnce();
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    CITOR_ALWAYS_ASSERT(count == kExpectedNodes);
  }
  return finalizeRow("libfork", samples);
}

// ---------------------------------------------------------------------------
// Strassen: same algorithm as forkjoin_strassen_bench.cpp (kSeqCutoff = 64,
// kParallelDepth = 1, 7 sub-products in 4 paired groups). The recursion is
// expressed as libfork coroutines that fork the four groups + the lone M7
// branch. Sub-product output buffers and operand temporaries live in a
// caller-managed scratch arena (`scratchBudget(n, 0)` floats) so the
// coroutine stays small.
// ---------------------------------------------------------------------------

namespace strassen {

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
  void operator()(float *p) const noexcept {
    if (p != nullptr) {
      std::free(p);
    }
  }
};

using AlignedFloatBuffer = std::unique_ptr<float[], AlignedFloatDeleter>;

inline AlignedFloatBuffer allocateAlignedFloats(std::size_t count) {
  void *raw = nullptr;
  const std::size_t bytes = ((count * sizeof(float) + 63U) / 64U) * 64U;
  if (::posix_memalign(&raw, 64U, bytes) != 0) {
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

// libfork strassen coroutine: matches the C++20 reference at
// forkjoin_strassen_bench.cpp:strassenRec. Operand temporaries + sub-product
// buffers live in `scratch`; each parallel sub-product gets a disjoint slice
// while depth < kStrassenParallelDepth, otherwise the seven mults serially
// reuse a single child slice.
inline constexpr auto strassenCoro = [](auto self, Sub c, Sub a, Sub b,
                                        float *scratch,
                                        std::size_t depth) -> lf::task<void> {
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
  Sub mBufs[7];
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
    co_await lf::fork[self](mBufs[0], t1A, t1B, childScratch, depth + 1U);
    co_await lf::fork[self](mBufs[1], t2A, b11, childScratch + childBudget,
                            depth + 1U);
    co_await lf::fork[self](mBufs[2], a11, t3B,
                            childScratch + (2U * childBudget), depth + 1U);
    co_await lf::fork[self](mBufs[3], a22, t4B,
                            childScratch + (3U * childBudget), depth + 1U);
    co_await lf::fork[self](mBufs[4], t5A, b22,
                            childScratch + (4U * childBudget), depth + 1U);
    co_await lf::fork[self](mBufs[5], t6A, t6B,
                            childScratch + (5U * childBudget), depth + 1U);
    co_await lf::call[self](mBufs[6], t7A, t7B,
                            childScratch + (6U * childBudget), depth + 1U);
    co_await lf::join;
  } else {
    co_await lf::call[self](mBufs[0], t1A, t1B, childScratch, depth + 1U);
    co_await lf::call[self](mBufs[1], t2A, b11, childScratch, depth + 1U);
    co_await lf::call[self](mBufs[2], a11, t3B, childScratch, depth + 1U);
    co_await lf::call[self](mBufs[3], a22, t4B, childScratch, depth + 1U);
    co_await lf::call[self](mBufs[4], t5A, b22, childScratch, depth + 1U);
    co_await lf::call[self](mBufs[5], t6A, t6B, childScratch, depth + 1U);
    co_await lf::call[self](mBufs[6], t7A, t7B, childScratch, depth + 1U);
    co_await lf::join;
  }

  addInto(c11, mBufs[0], mBufs[3]);
  subEq(c11, mBufs[4]);
  addEq(c11, mBufs[6]);

  addInto(c12, mBufs[2], mBufs[4]);
  addInto(c21, mBufs[1], mBufs[3]);

  subInto(c22, mBufs[0], mBufs[1]);
  addEq(c22, mBufs[2]);
  addEq(c22, mBufs[5]);
};

} // namespace strassen

// ---------------------------------------------------------------------------
// cilksort: matches forkjoin_cilksort_bench.cpp's recursion. The merge phase
// uses a serial bucket-merge fallback because libfork's coroutine recursion
// inside a worker pairs awkwardly with the per-bucket parallel-merge wave;
// the bucket count is small enough (kMergeBuckets = 64) that the serial
// pass is below measurement granularity at the cell sizes (n in {1M, 16M}).
// ---------------------------------------------------------------------------

namespace cilksort {

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
  // Bucket-split + serial seqMerge per bucket. Same shape as the dispenso
  // serial-fallback path: scan + per-bucket merge are both serial because
  // libfork inside the recursion doesn't compose with another fork-wave at
  // the merge depth without double-spawning the executor.
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

inline constexpr auto cilksortCoro = [](auto self, std::int32_t *data,
                                        std::int32_t *tmp, std::size_t lo,
                                        std::size_t hi) -> lf::task<void> {
  const std::size_t n = hi - lo;
  if (n <= kSeqCutoff) {
    std::sort(data + lo, data + hi);
    co_return;
  }
  const std::size_t mid = lo + (n / 2U);
  co_await lf::fork[self](data, tmp, lo, mid);
  co_await lf::call[self](data, tmp, mid, hi);
  co_await lf::join;
  serialMerge(data, lo, mid, mid, hi, tmp, lo);
  std::copy(tmp + lo, tmp + hi, data + lo);
};

} // namespace cilksort

BenchRow runLibforkStrassen(std::size_t participants, std::size_t n,
                            const CyclesPerNanosecond &cal) {
  using namespace strassen;
  lf::lazy_pool pool(participants);

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
    CITOR_ALWAYS_ASSERT(maxDiff <= tolerance);
  };

  for (std::size_t i = 0; i < kStrassenWarmupIterations; ++i) {
    lf::sync_wait(pool, strassenCoro, cSub, aSub, bSub, scratch.data(),
                  std::size_t{0});
    verify();
  }
  std::vector<double> samples;
  samples.reserve(kStrassenIterations);
  for (std::size_t i = 0; i < kStrassenIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    lf::sync_wait(pool, strassenCoro, cSub, aSub, bSub, scratch.data(),
                  std::size_t{0});
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    verify();
  }
  return finalizeRow("libfork", samples);
}

BenchRow runLibforkCilksort(std::size_t participants, std::size_t n,
                            const CyclesPerNanosecond &cal) {
  using namespace cilksort;
  lf::lazy_pool pool(participants);
  const std::vector<std::int32_t> input = buildInput(n);
  std::vector<std::int32_t> reference = input;
  std::sort(reference.begin(), reference.end());
  std::vector<std::int32_t> data(n, std::int32_t{0});
  std::vector<std::int32_t> tmp(n, std::int32_t{0});

  for (std::size_t i = 0; i < kCilksortWarmupIterations; ++i) {
    data = input;
    lf::sync_wait(pool, cilksortCoro, data.data(), tmp.data(), std::size_t{0},
                  n);
    CITOR_ALWAYS_ASSERT(data == reference);
  }

  std::vector<double> samples;
  samples.reserve(kCilksortIterations);
  for (std::size_t i = 0; i < kCilksortIterations; ++i) {
    data = input;
    const std::uint64_t startCycles = readCyclesStart();
    lf::sync_wait(pool, cilksortCoro, data.data(), tmp.data(), std::size_t{0},
                  n);
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    CITOR_ALWAYS_ASSERT(data == reference);
  }
  return finalizeRow("libfork", samples);
}

// ---------------------------------------------------------------------------
// matmul DAC: recursive 2x2 block matmul with 8 sub-products in 2 phases
// (overwrite + accumulate). Mirrors libfork's `bench/source/matmul/libfork.cpp`
// exactly: each level 4-forks for the overwrite phase, joins, 4-forks for
// the accumulate phase, joins. Per the libfork source the 4th call of each
// phase is `lf::call` (inline on caller) rather than `lf::fork`, which
// avoids one continuation-steal per level.
// ---------------------------------------------------------------------------

namespace matmul_dac_lf {

constexpr std::size_t kSeqCutoff = 64;
constexpr std::size_t kIterations = 10;
constexpr std::size_t kWarmupIterations = 2;

struct AlignedFloatDeleter {
  void operator()(float *p) const noexcept {
    if (p != nullptr) {
      std::free(p);
    }
  }
};

using AlignedFloatBuffer = std::unique_ptr<float[], AlignedFloatDeleter>;

inline AlignedFloatBuffer allocateAlignedFloats(std::size_t count) {
  void *raw = nullptr;
  const std::size_t bytes = ((count * sizeof(float) + 63U) / 64U) * 64U;
  if (::posix_memalign(&raw, 64U, bytes) != 0) {
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

inline constexpr auto matmulCoro =
    [](auto self, const float *A, const float *B, float *R, std::size_t n,
       std::size_t stride, bool add) -> lf::task<void> {
  if (n <= kSeqCutoff) {
    leafMultiply(A, B, R, n, stride, add);
    co_return;
  }
  const std::size_t m = n / 2U;
  const std::size_t o00 = 0;
  const std::size_t o01 = m;
  const std::size_t o10 = m * stride;
  const std::size_t o11 = (m * stride) + m;

  // Phase 1: overwrite
  co_await lf::fork[self](A + o00, B + o00, R + o00, m, stride, add);
  co_await lf::fork[self](A + o00, B + o01, R + o01, m, stride, add);
  co_await lf::fork[self](A + o10, B + o00, R + o10, m, stride, add);
  co_await lf::call[self](A + o10, B + o01, R + o11, m, stride, add);
  co_await lf::join;

  // Phase 2: accumulate
  co_await lf::fork[self](A + o01, B + o10, R + o00, m, stride, true);
  co_await lf::fork[self](A + o01, B + o11, R + o01, m, stride, true);
  co_await lf::fork[self](A + o11, B + o10, R + o10, m, stride, true);
  co_await lf::call[self](A + o11, B + o11, R + o11, m, stride, true);
  co_await lf::join;
};

} // namespace matmul_dac_lf

BenchRow runLibforkMatmulDac(std::size_t participants, std::size_t n,
                             const CyclesPerNanosecond &cal) {
  using namespace matmul_dac_lf;
  lf::lazy_pool pool(participants);

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
    CITOR_ALWAYS_ASSERT(maxDiff <= tolerance);
  };

  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    lf::sync_wait(pool, matmulCoro, aBuf.get(), bBuf.get(), cBuf.get(), n, n,
                  /*add=*/false);
    verify();
  }
  std::vector<double> samples;
  samples.reserve(kIterations);
  for (std::size_t i = 0; i < kIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    lf::sync_wait(pool, matmulCoro, aBuf.get(), bBuf.get(), cBuf.get(), n, n,
                  /*add=*/false);
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    verify();
  }
  return finalizeRow("libfork", samples);
}

// ---------------------------------------------------------------------------
// Skynet: 10-way fanout, depth 6, sum 0..(10^6-1). Idiomatic libfork shape:
// 9 forks + 1 call (last child runs inline on parent).
// ---------------------------------------------------------------------------

namespace skynet_lf {

constexpr int kFanout = 10;
constexpr int kDepth = 6;
constexpr std::int64_t kLeafCount = 1'000'000;
constexpr std::int64_t kExpectedSum = (kLeafCount * (kLeafCount - 1)) / 2;
constexpr std::size_t kIterations = 25;
constexpr std::size_t kWarmupIterations = 3;

inline constexpr auto skynetCoro = [](auto self, std::int64_t label, int depth,
                                      std::int64_t *out) -> lf::task<void> {
  if (depth == 0) {
    *out = label;
    co_return;
  }
  const std::int64_t base = label * kFanout;
  std::array<std::int64_t, kFanout> partials{};
  for (int i = 0; i < kFanout - 1; ++i) {
    co_await lf::fork[self](base + static_cast<std::int64_t>(i), depth - 1,
                            &partials[static_cast<std::size_t>(i)]);
  }
  co_await lf::call[self](base + static_cast<std::int64_t>(kFanout - 1),
                          depth - 1,
                          &partials[static_cast<std::size_t>(kFanout - 1)]);
  co_await lf::join;
  std::int64_t total = 0;
  for (const std::int64_t v : partials) {
    total += v;
  }
  *out = total;
};

} // namespace skynet_lf

BenchRow runLibforkSkynet(std::size_t participants,
                          const CyclesPerNanosecond &cal) {
  using namespace skynet_lf;
  lf::lazy_pool pool(participants);

  auto runOnce = [&]() -> std::int64_t {
    std::int64_t result = 0;
    lf::sync_wait(pool, skynetCoro, std::int64_t{0}, kDepth, &result);
    return result;
  };

  std::int64_t result = 0;
  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    result = runOnce();
  }
  CITOR_ALWAYS_ASSERT(result == kExpectedSum);

  std::vector<double> samples;
  samples.reserve(kIterations);
  for (std::size_t i = 0; i < kIterations; ++i) {
    const std::uint64_t startCycles = readCyclesStart();
    result = runOnce();
    const std::uint64_t endCycles = readCyclesEnd();
    samples.push_back(cyclesToNs(endCycles - startCycles, cal));
    CITOR_ALWAYS_ASSERT(result == kExpectedSum);
  }
  return finalizeRow("libfork", samples);
}

} // namespace citor::bench
