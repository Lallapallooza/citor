#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <utility>

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#endif

namespace citor {

namespace detail {
/// One-time TSC-frequency calibration (cycles per nanosecond). Measures wall time vs `__rdtsc()`
/// over a 5 ms window on first call; cached for the process lifetime. Used by
/// `Deadline::fromMillis` / `Deadline::fromMicros`. Returns 0.0 on architectures without a TSC
/// (any factory call collapses to a never-expires deadline in that case).
[[nodiscard]] inline double tscCyclesPerNs() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
  static const double kCyclesPerNs = []() noexcept {
    using clock = std::chrono::steady_clock;
    constexpr auto kWindow = std::chrono::milliseconds(5);
    const auto wallStart = clock::now();
    const std::uint64_t tscStart = __rdtsc();
    while (clock::now() - wallStart < kWindow) {
      // Tight wait; the overhead is dominated by the wall-clock query, not the loop body.
    }
    const std::uint64_t tscEnd = __rdtsc();
    const auto wallEnd = clock::now();
    const auto wallNs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(wallEnd - wallStart).count();
    if (wallNs <= 0 || tscEnd <= tscStart) {
      return 0.0;
    }
    return static_cast<double>(tscEnd - tscStart) / static_cast<double>(wallNs);
  }();
  return kCyclesPerNs;
#else
  return 0.0;
#endif
}
} // namespace detail

/// Cooperative cancellation handle shared by the producer and pool workers.
///
/// `CancellationToken` wraps a heap-allocated atomic state word managed by `std::shared_ptr`, so
/// copies share the same cancellation signal. Workers poll `stop_requested()` at chunk
/// boundaries; the producer or any other party may call `request_stop()` to signal cooperative
/// cancellation. The state word is published via release / observed via acquire so any writes the
/// stopper performed before signalling are visible to a worker that observes the stop flag.
///
/// The default-constructed token shares a single allocated state word (created lazily in the
/// default constructor) so `stop_requested()` is well-defined on every code path. Constructing a
/// token is a one-time heap allocation; copy / move / dtor are pointer-arithmetic only and do not
/// allocate. `stop_requested()` and `request_stop()` are `noexcept` and allocation-free.
class CancellationToken {
public:
  /// Construct a "never-stopped" sentinel token without heap allocation.
  ///
  /// The default-constructed token holds no shared state: `stop_requested()` always returns
  /// `false`, `request_stop()` is a no-op (returns `false`). This is the always-on default for
  /// call sites that do not need cancellation; the per-dispatch heap allocation and
  /// `shared_ptr` dispose/destroy chain go away when the caller does not pass an explicit token.
  ///
  /// Callers that need to actually call `request_stop()` from elsewhere must construct via
  /// `makeOwned`, which allocates the shared atomic. Constructing a default-`{}` token and
  /// then calling `request_stop()` will not signal anything.
  constexpr CancellationToken() noexcept = default;

  /// Construct a token that owns a shared, heap-allocated stop flag.
  ///
  /// Use this when the caller intends to call `request_stop` on a copy held elsewhere.
  /// Allocates a single 32-bit atomic on the heap; copies share the atomic via shared_ptr.
  [[nodiscard]] static CancellationToken makeOwned() {
    CancellationToken t;
    t.m_state = std::make_shared<std::atomic<std::uint32_t>>(0U);
    return t;
  }

  /// Test whether `request_stop()` has been called on any copy of this token.
  ///
  /// `true` if any holder has signalled cancellation; `false` for the never-stopped
  ///         sentinel and for any owned token whose flag is still clear.
  [[nodiscard]] bool stop_requested() const noexcept {
    auto *state = m_state.get();
    if (state == nullptr) {
      return false;
    }
    return state->load(std::memory_order_acquire) != 0U;
  }

  /// Report whether this token has shared state that can be stopped.
  ///
  /// The default-constructed sentinel has no state and therefore can never
  /// observe a stop request. Dispatch hot paths use this to select a no-poll
  /// worker entry for the common "no token supplied" case while preserving
  /// polling for tokens created via `makeOwned`.
  ///
  /// `true` for owned tokens; `false` for the never-stopped sentinel.
  [[nodiscard]] bool canStop() const noexcept { return m_state != nullptr; }

  /// Equality on the underlying control-block pointer. Used by descriptor write elision:
  /// two tokens compare equal when they share the same control block (or both are the
  /// no-state sentinel). Steady-state bench loops re-bind the same default sentinel each
  /// call, so producers skip the redundant store.
  bool operator==(const CancellationToken &other) const noexcept {
    return m_state.get() == other.m_state.get();
  }
  bool operator!=(const CancellationToken &other) const noexcept { return !(*this == other); }

  /// Signal cancellation to every holder of this token.
  ///
  /// The release-store synchronizes with `stop_requested`'s acquire-load: any data the caller
  /// wrote before invoking `request_stop` is visible to a worker that observes the stop flag.
  ///
  /// `true` if this call transitioned the token from non-stopped to stopped; `false` if
  ///         the token was already stopped, or if this is the `neverStopped` sentinel.
  bool request_stop() noexcept {
    auto *state = m_state.get();
    if (state == nullptr) {
      return false;
    }
    std::uint32_t expected = 0U;
    return state->compare_exchange_strong(expected, 1U, std::memory_order_release,
                                          std::memory_order_relaxed);
  }

private:
  /// Shared atomic state; bit 0 is the stop flag. Higher bits reserved for future use.
  std::shared_ptr<std::atomic<std::uint32_t>> m_state;
};

/// Wall-clock deadline expressed as a TSC-cycle threshold.
///
/// `Deadline` stores an absolute `__rdtsc` reading at which the deadline expires. `expired()`
/// compares the current TSC against the threshold. The reading is taken once at construction and
/// never refreshed; the only call sites are inside hot worker bodies
/// where a syscall-based `clock_gettime` would dominate the budget.
///
/// A default-constructed deadline never expires (threshold is `UINT64_MAX`). `Deadline::expired`
/// is `noexcept` and allocation-free.
class Deadline {
public:
  /// Construct a deadline that never expires.
  ///
  /// Used by call sites that do not propagate a deadline. The threshold is set to the maximum
  /// representable cycle count so `expired()` always returns `false`.
  constexpr Deadline() noexcept = default;

  /// Construct a deadline at an absolute TSC threshold.
  ///
  /// The caller provides a precomputed `__rdtsc()` value; the deadline expires when the live TSC
  /// reaches or passes the threshold. This is low-level: callers can reuse a single
  /// `__rdtsc` reading taken at job-publish time across many primitives.
  ///
  /// tscThreshold Absolute TSC cycle count at which the deadline expires.
  constexpr explicit Deadline(std::uint64_t tscThreshold) noexcept : m_tscThreshold(tscThreshold) {}

  /// Construct a deadline at `now + ms` using a one-time TSC frequency calibration.
  ///
  /// Calibrates the TSC frequency on first call (5 ms wall-clock window) and caches the result for
  /// the process lifetime. Subsequent calls are a single `__rdtsc()` plus a multiplication. On
  /// non-x86 hosts the calibration returns 0 and this factory returns the never-expires sentinel.
  ///
  /// ms Wall-clock milliseconds from now until the deadline expires.
  [[nodiscard]] static Deadline fromMillis(std::uint64_t ms) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    const double cyclesPerNs = detail::tscCyclesPerNs();
    if (cyclesPerNs <= 0.0) {
      return Deadline{};
    }
    const auto delta = static_cast<std::uint64_t>(static_cast<double>(ms) * 1.0e6 * cyclesPerNs);
    return Deadline{__rdtsc() + delta};
#else
    (void)ms;
    return Deadline{};
#endif
  }

  /// Construct a deadline at `now + us`. See `fromMillis` for calibration details.
  ///
  /// us Wall-clock microseconds from now until the deadline expires.
  [[nodiscard]] static Deadline fromMicros(std::uint64_t us) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    const double cyclesPerNs = detail::tscCyclesPerNs();
    if (cyclesPerNs <= 0.0) {
      return Deadline{};
    }
    const auto delta = static_cast<std::uint64_t>(static_cast<double>(us) * 1.0e3 * cyclesPerNs);
    return Deadline{__rdtsc() + delta};
#else
    (void)us;
    return Deadline{};
#endif
  }

  /// Check whether the live TSC has reached the deadline's threshold.
  ///
  /// `true` if the deadline is in the past; `false` otherwise. Returns `false` for the
  ///         default-constructed never-expires sentinel.
  [[nodiscard]] bool expired() const noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return __rdtsc() >= m_tscThreshold;
#else
    return false;
#endif
  }

  /// Access the absolute TSC threshold this deadline was constructed with.
  /// The threshold value passed to the constructor.
  [[nodiscard]] constexpr std::uint64_t threshold() const noexcept { return m_tscThreshold; }

private:
  /// Absolute TSC cycle count at which the deadline expires; never-expires sentinel by default.
  std::uint64_t m_tscThreshold = UINT64_MAX;
};

/// Thrown from a void-returning primitive whose token / deadline cancelled mid-flight.
///
/// The producer rethrows this on the join path so callers can distinguish cancellation from a
/// worker exception. Inherits `std::exception` rather than `std::runtime_error` to keep the type
/// lightweight and free of heap allocation in the constructor.
class cancelled_exception : public std::exception {
public:
  /// Return the diagnostic string identifying the exception kind.
  /// A non-null, statically-stored C-string.
  [[nodiscard]] const char *what() const noexcept override { return "citor: cancelled"; }
};

/// Thrown from a value-producing primitive whose join was cancelled mid-flight.
///
/// Carries a `partial_value` field that holds the deterministic combine of all completed chunks
/// up to the cancellation. For `Determinism::FixedBlockOrder`, the partial result is well-defined
/// (combine of completed chunks `[0, completed)` in chunk-id order). For `OrderTolerant`, the
/// partial value is order-tolerant and reflects whatever workers happened to commit.
///
/// T The value type the cancelled primitive was producing.
template <class T> class cancelled_value_exception : public std::exception {
public:
  /// Construct with a deterministic-combine partial result.
  ///
  /// partial The combine-of-completed-chunks partial value at the moment of cancellation.
  explicit cancelled_value_exception(T partial) noexcept(std::is_nothrow_move_constructible_v<T>)
      : partial_value(std::move(partial)) {}

  /// Return the diagnostic string identifying the exception kind.
  /// A non-null, statically-stored C-string.
  [[nodiscard]] const char *what() const noexcept override {
    return "citor: cancelled with partial value";
  }

  /// Combined partial result of all chunks that completed before cancellation observed.
  T partial_value;
};

} // namespace citor
