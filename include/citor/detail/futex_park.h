#pragma once

#include <atomic>
#include <cstdint>

#ifdef __linux__
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <ctime>
#else
#include <condition_variable> // IWYU pragma: keep
#include <mutex>              // IWYU pragma: keep
#endif

namespace citor::detail {

#ifdef __linux__

/// Wrap `FUTEX_WAIT_PRIVATE` so the caller goes to sleep when |addr| still
/// equals |expected|.
///
/// The pool drives parking through a raw `syscall(SYS_futex, ...,
/// FUTEX_WAIT_PRIVATE, ...)` call so we control the protocol end-to-end.
/// `std::atomic::wait` is unusable: libc++ PR146267 (lost-wakeup) and libstdc++
/// PR122878 (`__spin_until_impl` regression) compromise its reliability across
/// the libc versions our consumers ship.
///
/// Lost-wakeup defense lives at the call site: re-check the source-of-truth
/// `generation` counter before invoking this function and again after it
/// returns. The 32-bit `futexWord` is a parking token only; correctness is
/// anchored on the 64-bit `generation`.
///
/// addr     Atomic word the kernel monitors for changes; must outlive the
/// caller's wait. expected Expected value at the time of suspension; the kernel
/// returns immediately with
///                 `EAGAIN` if `*addr != expected` to avoid the classic
///                 lost-wakeup race.
/// timeout  Optional relative timeout; when `nullptr` the call blocks
/// indefinitely. The raw `syscall` return value. Negative on error (errno set),
/// zero on a successful wake,
///         positive otherwise.
inline long futexWaitPrivate(std::atomic<std::uint32_t> *addr,
                             std::uint32_t expected,
                             const struct timespec *timeout) noexcept {
  return syscall(SYS_futex, reinterpret_cast<std::uint32_t *>(addr),
                 FUTEX_WAIT_PRIVATE, expected, timeout, nullptr, 0);
}

/// Wrap `FUTEX_WAKE_PRIVATE` to wake up to |n| parked waiters on |addr|.
///
/// Pass `INT_MAX` for |n| to broadcast (the shutdown path does this). Every
/// wake on the pool's `futexWord` is paired with a release-store on the
/// source-of-truth state the parked thread is about to re-check; `futexWord`
/// itself remains relaxed.
///
/// addr Atomic word the kernel uses to identify the wait queue.
/// n    Maximum number of waiters to wake.
/// The number of waiters actually woken, or a negative value on error.
inline long futexWakePrivate(std::atomic<std::uint32_t> *addr, int n) noexcept {
  return syscall(SYS_futex, reinterpret_cast<std::uint32_t *>(addr),
                 FUTEX_WAKE_PRIVATE, n, nullptr, nullptr, 0);
}

#else

/// Portable parking fallback used outside Linux.
///
/// The Linux build relies on raw futex syscalls; on macOS, Windows, and other
/// targets we emulate the same wait/wake contract with a process-local
/// `std::condition_variable`. The fallback does not promise sub-microsecond
/// wakeup latency; it exists so the headers compile and tests run.
struct FutexFallbackState {
  std::mutex mtx;
  std::condition_variable cv;
};

/// Access the singleton fallback condvar shared by every pool on non-Linux
/// hosts.
///
/// The state is process-global; pool wake protocols always re-check the
/// source-of-truth atomic after returning from a wait, so spurious wakeups
/// across pools are safe.
///
/// Reference to the lazily-initialized fallback state.
inline FutexFallbackState &futexFallbackState() noexcept {
  static FutexFallbackState s;
  return s;
}

/// Non-Linux fallback that mirrors the Linux `FUTEX_WAIT_PRIVATE` contract.
///
/// addr     Atomic word checked against |expected| before parking.
/// expected Expected value; the function returns immediately when the load
/// disagrees. timeout  Ignored on non-Linux fallback; the wait is effectively
/// unbounded. Always zero; callers re-check the source-of-truth atomic after
/// wake.
inline long futexWaitPrivate(std::atomic<std::uint32_t> *addr,
                             std::uint32_t expected,
                             const void *timeout) noexcept {
  (void)timeout;
  auto &state = futexFallbackState();
  std::unique_lock<std::mutex> lock(state.mtx);
  if (addr->load(std::memory_order_acquire) != expected) {
    return 0;
  }
  state.cv.wait(lock);
  return 0;
}

/// Non-Linux fallback that mirrors the Linux `FUTEX_WAKE_PRIVATE` contract.
///
/// addr Atomic word identifying the wait queue (unused on the fallback).
/// n    Hint for how many waiters to wake; the fallback always broadcasts.
/// Always zero; the fallback does not report exact wake counts.
inline long futexWakePrivate(std::atomic<std::uint32_t> *addr, int n) noexcept {
  (void)addr;
  (void)n;
  auto &state = futexFallbackState();
  {
    const std::lock_guard<std::mutex> lock(state.mtx);
  }
  state.cv.notify_all();
  return 0;
}

#endif

} // namespace citor::detail
