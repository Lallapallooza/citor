#pragma once

// Spin-loop CPU hint used by every busy-wait in the engine.
//
// Factored out of `worker_loop.h` so headers that only need the hint
// (`lookback_scan.h`, `coherence_probe.h`, ...) avoid pulling in the
// full worker dispatch state.

#include <atomic>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#endif

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace citor::detail {

/// Insert a single PAUSE / YIELD hint to back off without de-scheduling.
///
/// `_mm_pause` on x86-64 is the spin-loop hint of choice (P0514R4); it
/// lets the CPU drop hyper-thread issue slots without yielding the
/// scheduler quantum. Non-x86 builds fall through to a compiler barrier.
inline void cpuRelax() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
  _mm_pause();
#else
  std::atomic_signal_fence(std::memory_order_acq_rel);
#endif
}

/// Index of the least-significant set bit in `x`. Behavior on `x == 0` is
/// undefined (matches `__builtin_ctzll`); every call site already gates on
/// a non-zero scan word.
inline unsigned ctzll(std::uint64_t x) noexcept {
#if defined(_MSC_VER) && !defined(__clang__)
  unsigned long idx = 0;
  _BitScanForward64(&idx, x);
  return static_cast<unsigned>(idx);
#else
  return static_cast<unsigned>(__builtin_ctzll(x));
#endif
}

/// Compute `a * b / c` through a 128-bit intermediate so the
/// multiplication cannot overflow when both operands fill 64 bits.
/// Caller guarantees the quotient fits in 64 bits; every site has
/// `slot < participants`, so `(n*slot)/participants <= n`.
inline std::uint64_t mulDiv64(std::uint64_t a, std::uint64_t b,
                              std::uint64_t c) noexcept {
#if defined(__SIZEOF_INT128__)
  __extension__ using u128 = unsigned __int128;
  return static_cast<std::uint64_t>((static_cast<u128>(a) * b) / c);
#elif defined(_M_X64) && defined(_MSC_VER)
  std::uint64_t hi = 0;
  const std::uint64_t lo = _umul128(a, b, &hi);
  std::uint64_t rem = 0;
  return _udiv128(hi, lo, c, &rem);
#else
#error "mulDiv64 needs either __int128 or x64 MSVC _umul128/_udiv128"
#endif
}

} // namespace citor::detail
