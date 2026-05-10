#pragma once

// Spin-loop CPU hint used by every busy-wait in the engine.
//
// Factored out of `worker_loop.h` so headers that only need the hint
// (`lookback_scan.h`, `coherence_probe.h`, ...) avoid pulling in the
// full worker dispatch state.

#include <atomic>

#if defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
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

} // namespace citor::detail
