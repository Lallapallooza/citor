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

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "citor/always_assert.h"

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

void seqQueensRec(int n, int row, std::uint64_t cols, std::uint64_t diag1, std::uint64_t diag2,
                  std::int64_t &count) noexcept {
  if (row == n) {
    ++count;
    return;
  }
  std::uint64_t bits =
      ~(cols | diag1 | diag2) & ((std::uint64_t{1} << static_cast<unsigned>(n)) - 1);
  while (bits != 0U) {
    const std::uint64_t pick = bits & (~bits + 1U);
    bits ^= pick;
    seqQueensRec(n, row + 1, cols | pick, (diag1 | pick) << 1U, (diag2 | pick) >> 1U, count);
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
      std::uint64_t bits =
          ~(s.cols | s.diag1 | s.diag2) & ((std::uint64_t{1} << static_cast<unsigned>(n)) - 1);
      while (bits != 0U) {
        const std::uint64_t pick = bits & (~bits + 1U);
        bits ^= pick;
        next.push_back({s.cols | pick, (s.diag1 | pick) << 1U, (s.diag2 | pick) >> 1U});
      }
    }
    frontier = std::move(next);
  }
  return frontier;
}

tmc::task<std::int64_t> queensRangeCoro(const QueensRoot *roots, std::size_t lo, std::size_t hi,
                                        int n) {
  if (hi - lo == 1) {
    const QueensRoot &s = roots[lo];
    std::int64_t count = 0;
    seqQueensRec(n, kQueensRootDepth, s.cols, s.diag1, s.diag2, count);
    co_return count;
  }
  const std::size_t mid = lo + ((hi - lo) / 2);
  auto [left, right] =
      co_await tmc::spawn_tuple(queensRangeCoro(roots, lo, mid, n), queensRangeCoro(roots, mid, hi, n));
  co_return left + right;
}

// ---------------------------------------------------------------------------
// UTS T1: SHA-1-driven RNG and tree shape duplicated from
// forkjoin_uts_bench.cpp (whose anonymous-namespace impl can't be exposed).
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

} // namespace citor::bench
