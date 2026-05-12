#pragma once

#include <atomic>
#include <cstdint>

#ifdef __linux__
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <ctime>
#elif defined(_WIN32)
// `WaitOnAddress` / `WakeByAddress[Single|All]` model the same parking
// protocol as Linux's `FUTEX_WAIT_PRIVATE`: the kernel reads `*addr` and
// suspends iff it still equals the caller-supplied compare value. Available
// since Windows 8. The symbols live in `api-ms-win-core-synch-l1-2-0.dll`
// (linker resolves through `Synchronization.lib`).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <synchapi.h>
#pragma comment(lib, "Synchronization.lib")
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

#elif defined(_WIN32)

/// Windows peer of `FUTEX_WAIT_PRIVATE`. `WaitOnAddress` reads `*addr`,
/// compares against `*expected`, and suspends only when they match;
/// per-address wait queue, no process-global contention. Returns 0 on
/// success (wake or mismatch), `-1` on failure. Caller re-checks the
/// source-of-truth atomic after wake.
inline long futexWaitPrivate(std::atomic<std::uint32_t> *addr,
                             std::uint32_t expected,
                             const void *timeout) noexcept {
  (void)timeout;
  std::uint32_t compare = expected;
  const BOOL ok = ::WaitOnAddress(static_cast<volatile VOID *>(addr), &compare,
                                  sizeof(std::uint32_t), INFINITE);
  return ok ? 0 : -1;
}

/// Windows peer of `FUTEX_WAKE_PRIVATE`. `n >= kBroadcastThreshold`
/// (shutdown's `INT_MAX`) routes to `WakeByAddressAll`; bounded `n`
/// loops `WakeByAddressSingle` so the chain-wake-2 protocol does not
/// degenerate into a broadcast at every hop. Caller must publish the
/// source-of-truth state before invoking, so a freshly-parked waiter
/// either observes the update or is woken.
inline long futexWakePrivate(std::atomic<std::uint32_t> *addr, int n) noexcept {
  // Treat INT_MAX (or any value exceeding the practical worker fan-out) as
  // a broadcast; otherwise wake exactly `n` waiters to mirror Linux's
  // `FUTEX_WAKE_PRIVATE` semantics.
  constexpr int kBroadcastThreshold = 1024;
  if (n >= kBroadcastThreshold) {
    ::WakeByAddressAll(static_cast<PVOID>(addr));
    return 0;
  }
  for (int i = 0; i < n; ++i) {
    ::WakeByAddressSingle(static_cast<PVOID>(addr));
  }
  return 0;
}

#else

/// Portable parking fallback used outside Linux and Windows.
///
/// The Linux build relies on raw futex syscalls; the Windows build uses
/// `WaitOnAddress`. Every other target (macOS, BSDs, ...) emulates the same
/// wait/wake contract with a process-local `std::condition_variable`. The
/// fallback does not promise sub-microsecond wakeup latency; it exists so
/// the headers compile and tests run.
struct FutexFallbackState {
  /// Serialises the wait/wake handshake on the shared condition variable.
  std::mutex mtx;
  /// Condition variable used by both the wait and the wake side.
  std::condition_variable cv;
};

/// Access the singleton fallback condvar shared by every pool on the
/// non-Linux / non-Windows hosts.
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

/// Generic fallback that mirrors the Linux `FUTEX_WAIT_PRIVATE` contract.
///
/// addr     Atomic word checked against |expected| before parking.
/// expected Expected value; the function returns immediately when the load
/// disagrees. timeout  Ignored on the fallback; the wait is effectively
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

/// Generic fallback that mirrors the Linux `FUTEX_WAKE_PRIVATE` contract.
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
