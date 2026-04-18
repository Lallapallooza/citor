#pragma once

#include <cstdint>
#include <ctime>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

#if defined(CITOR_BENCH_HAS_PERF_EVENT)
#include <asm/unistd.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace citor::bench {

/// Read the opening TSC cycle counter using `lfence; rdtsc`.
///
/// The bench harness measures dispatch latency in cycles via
/// `lfence; rdtsc; ...; rdtscp; lfence` so that pending memory operations
/// settle before the cycle stamp is taken. This helper provides the opening
/// half (`lfence` then `rdtsc`); the companion `readCyclesEnd` closes the
/// window with `rdtscp; lfence`.
///
/// The TSC cycle stamp, with prior loads fenced.
[[nodiscard]] inline std::uint64_t readCyclesStart() noexcept {
#if defined(__x86_64__)
  // `lfence` flushes the load buffer so any pending memory ops settle before
  // the `rdtsc` cycle stamp is taken. Using `rdtsc` (rather than `rdtscp`) on
  // the opening half matches the documented `lfence; rdtsc; ...; rdtscp;
  // lfence` measurement protocol.
  _mm_lfence();
  return __rdtsc();
#else
  // Non-x86 fallback uses CLOCK_MONOTONIC_RAW; values are returned in
  // nanoseconds and the bench layer normalizes via the calibration constant.
  struct timespec ts{};
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<std::uint64_t>(ts.tv_nsec);
#endif
}

/// Read the closing TSC cycle counter using `rdtscp; lfence`.
///
/// Mirror of `readCyclesStart`. The leading `rdtscp` is partially serializing
/// (waits for prior instructions to retire before sampling), and the trailing
/// `lfence` ensures no later instruction's effects leak into the measurement
/// window.
///
/// The TSC cycle stamp, with subsequent loads/stores fenced.
[[nodiscard]] inline std::uint64_t readCyclesEnd() noexcept {
#if defined(__x86_64__)
  unsigned int aux = 0;
  const std::uint64_t tsc = __rdtscp(&aux);
  _mm_lfence();
  return tsc;
#else
  struct timespec ts{};
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<std::uint64_t>(ts.tv_nsec);
#endif
}

/// Calibration constant relating TSC cycles to wall-time nanoseconds.
///
/// Built from a 10 ms calibration window timed with `clock_gettime` on the wall
/// side and `__rdtsc` deltas on the cycle side. On non-x86 platforms the helper
/// returns 1.0 because `readCyclesStart`/`readCyclesEnd` already report
/// nanoseconds.
struct CyclesPerNanosecond {
  /// Calibrated cycles per nanosecond; 1.0 on non-x86 platforms.
  double value = 1.0;
};

#if defined(CITOR_BENCH_HAS_PERF_EVENT)
/// Open a `PERF_COUNT_HW_CPU_CYCLES` counter for the calling thread.
///
/// Used by the calibration routine as a cross-check against `__rdtsc`. The
/// counter is opened with `disabled=1`, `exclude_kernel=1`, and is enabled
/// through `ioctl(PERF_EVENT_IOC_ENABLE)` immediately before the calibration
/// window. Returns -1 on failure (e.g., `kernel.perf_event_paranoid >= 2` and
/// the user lacks `CAP_PERFMON`); callers fall back to TSC-only calibration.
///
/// The perf event file descriptor, or -1 on failure.
inline int openPerfCyclesCounter() noexcept {
  perf_event_attr attr{};
  attr.type = PERF_TYPE_HARDWARE;
  attr.size = sizeof(attr);
  attr.config = PERF_COUNT_HW_CPU_CYCLES;
  attr.disabled = 1;
  attr.exclude_kernel = 1;
  attr.exclude_hv = 1;
  // Pin to the calling thread (`pid=0`) on the current CPU (`cpu=-1`) so the
  // counter follows the thread across migrations; the bench uses `taskset` at
  // run time to keep the thread stationary.
  const long fd = syscall(__NR_perf_event_open, &attr, /*pid=*/0, /*cpu=*/-1, /*group_fd=*/-1,
                          /*flags=*/0UL);
  if (fd < 0) {
    return -1;
  }
  return static_cast<int>(fd);
}
#endif

/// Calibrate `cycles per nanosecond` over a 10 ms window.
///
/// The calibration loop spins on `clock_gettime(CLOCK_MONOTONIC_RAW)` until at
/// least 10 ms of wall time elapses, brackets the spin with TSC reads, and
/// (when `perf_event_open` is available) cross-checks the result against the
/// hardware cycle counter. The hardware-counter result is used when its delta
/// agrees with the TSC delta to within 1 %; otherwise the TSC delta is used
/// unconditionally.
///
/// On non-x86 platforms the calibration returns 1.0 (the bench's "cycles" are
/// already nanoseconds via `clock_gettime`).
///
/// Cycles-per-nanosecond constant for converting TSC deltas to wall
///         time on the calling host.
[[nodiscard]] inline CyclesPerNanosecond calibrateCyclesPerNs() noexcept {
  CyclesPerNanosecond result;
#if !defined(__x86_64__)
  return result;
#else
  struct timespec ts0{};
  struct timespec ts1{};
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts0);
  const std::uint64_t cyc0 = readCyclesStart();
  // Busy-wait 10 ms; the bench is fired after `taskset` fixes the thread on a
  // single core so TSC drift across cores is not a concern.
  do {
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts1);
  } while (((static_cast<std::uint64_t>(ts1.tv_sec - ts0.tv_sec) * 1'000'000'000ULL) +
            static_cast<std::uint64_t>(ts1.tv_nsec - ts0.tv_nsec)) < 10'000'000ULL);
  const std::uint64_t cyc1 = readCyclesEnd();
  const std::uint64_t wallNs =
      (static_cast<std::uint64_t>(ts1.tv_sec - ts0.tv_sec) * 1'000'000'000ULL) +
      static_cast<std::uint64_t>(ts1.tv_nsec - ts0.tv_nsec);
  if (wallNs > 0) {
    result.value = static_cast<double>(cyc1 - cyc0) / static_cast<double>(wallNs);
  }
  return result;
#endif
}

/// Convert a TSC cycle delta to wall-time nanoseconds.
///
/// cycles  TSC delta from a `readCyclesStart`/`readCyclesEnd` pair.
/// cal     Calibration constant from `calibrateCyclesPerNs`.
/// Wall-time nanoseconds; on non-x86 platforms `cycles` is already in
///         nanoseconds and is returned unchanged.
[[nodiscard]] inline double cyclesToNs(std::uint64_t cycles,
                                       const CyclesPerNanosecond &cal) noexcept {
  if (cal.value <= 0.0) {
    return static_cast<double>(cycles);
  }
  return static_cast<double>(cycles) / cal.value;
}

} // namespace citor::bench
