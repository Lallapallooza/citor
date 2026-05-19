// SPDX-License-Identifier: MIT
//
// citor -- header-only C++20 thread pool
// version: 0.4.4
//
// GENERATED FILE -- DO NOT EDIT.
// Run `python tools/amalgamate.py` to regenerate.
// Modular sources live under `include/citor/`.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <numeric>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

// ===== citor/always_assert.h =====


namespace citor {

/// Emits a diagnostic to `stderr` and terminates via `std::abort`.
///
/// The format is fixed ("citor: always-assert failed: <cond> at
/// <file>:<line>\n") so death tests can match the output with a stable regex.
/// `std::abort` is chosen over `std::terminate` so GoogleTest's `EXPECT_DEATH`
/// catches the signal without routing through the terminate handler.
[[noreturn]] inline void alwaysAssertFail(const char *cond, const char *file,
                                          int line) noexcept {
  std::fprintf(stderr, "citor: always-assert failed: %s at %s:%d\n", cond, file,
               line);
  std::abort();
}

} // namespace citor

/// Release-active assertion: evaluates |cond| in every build configuration.
///
/// Unlike `assert`, this check survives -DNDEBUG. Use it at public API entry
/// points whose failure would corrupt memory or trigger undefined behavior past
/// the debug boundary -- writes through a read-only borrow, null output
/// buffers, etc.
#define CITOR_ALWAYS_ASSERT(cond)                                              \
  do {                                                                         \
    if (!(cond)) {                                                             \
      ::citor::alwaysAssertFail(#cond, __FILE__, __LINE__);                    \
    }                                                                          \
  } while (0)

// ===== citor/cancellation.h =====


#if defined(_M_X64) && defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace citor {

namespace detail {
/// One-time TSC-frequency calibration (cycles per nanosecond). Measures wall
/// time vs `__rdtsc()` over a 5 ms window on first call; cached for the process
/// lifetime. Used by `Deadline::fromMillis` / `Deadline::fromMicros`. Returns
/// 0.0 on architectures without a TSC (any factory call collapses to a
/// never-expires deadline in that case).
[[nodiscard]] inline double tscCyclesPerNs() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
  static const double kCyclesPerNs = []() noexcept {
    using clock = std::chrono::steady_clock;
    constexpr auto kWindow = std::chrono::milliseconds(5);
    const auto wallStart = clock::now();
    const std::uint64_t tscStart = __rdtsc();
    while (clock::now() - wallStart < kWindow) {
      // Tight wait; the overhead is dominated by the wall-clock query, not the
      // loop body.
    }
    const std::uint64_t tscEnd = __rdtsc();
    const auto wallEnd = clock::now();
    const auto wallNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            wallEnd - wallStart)
                            .count();
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
/// `CancellationToken` wraps a heap-allocated atomic state word managed by
/// `std::shared_ptr`, so copies share the same cancellation signal. Workers
/// poll `stop_requested()` at chunk boundaries; the producer or any other party
/// may call `request_stop()` to signal cooperative cancellation. The state word
/// is published via release / observed via acquire so any writes the stopper
/// performed before signalling are visible to a worker that observes the stop
/// flag.
///
/// The default-constructed token holds no shared state: it is the
/// "never-stopped" sentinel and `stop_requested()` always returns `false`.
/// `request_stop()` on a default-constructed token is a no-op. Construct a
/// signalable token via `CancellationToken::makeOwned()`; that variant
/// allocates one shared atomic on the heap and copies share it via
/// `shared_ptr`. `stop_requested()` and `request_stop()` are `noexcept` and
/// allocation-free regardless of construction path.
class CancellationToken {
public:
  /// Construct a "never-stopped" sentinel token without heap allocation.
  ///
  /// The default-constructed token holds no shared state: `stop_requested()`
  /// always returns `false`, `request_stop()` is a no-op (returns `false`).
  /// This is the always-on default for call sites that do not need
  /// cancellation; the per-dispatch heap allocation and `shared_ptr`
  /// dispose/destroy chain go away when the caller does not pass an explicit
  /// token.
  ///
  /// Callers that need to actually call `request_stop()` from elsewhere must
  /// construct via `makeOwned`, which allocates the shared atomic. Constructing
  /// a default-`{}` token and then calling `request_stop()` will not signal
  /// anything.
  constexpr CancellationToken() noexcept = default;

  /// Construct a token that owns a shared, heap-allocated stop flag.
  ///
  /// Use this when the caller intends to call `request_stop` on a copy held
  /// elsewhere. Allocates a single 32-bit atomic on the heap; copies share the
  /// atomic via shared_ptr.
  [[nodiscard]] static CancellationToken makeOwned() {
    CancellationToken t;
    t.m_state = std::make_shared<std::atomic<std::uint32_t>>(0U);
    return t;
  }

  /// Test whether `request_stop()` has been called on any copy of this token.
  ///
  /// `true` if any holder has signalled cancellation; `false` for the
  /// never-stopped
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

  /// Equality on the underlying control-block pointer. Used by descriptor write
  /// elision: two tokens compare equal when they share the same control block
  /// (or both are the no-state sentinel). Steady-state bench loops re-bind the
  /// same default sentinel each call, so producers skip the redundant store.
  bool operator==(const CancellationToken &other) const noexcept {
    return m_state.get() == other.m_state.get();
  }
  bool operator!=(const CancellationToken &other) const noexcept {
    return !(*this == other);
  }

  /// Signal cancellation to every holder of this token.
  ///
  /// The release-store synchronizes with `stop_requested`'s acquire-load: any
  /// data the caller wrote before invoking `request_stop` is visible to a
  /// worker that observes the stop flag.
  ///
  /// `true` if this call transitioned the token from non-stopped to stopped;
  /// `false` if
  ///         the token was already stopped, or if this is the `neverStopped`
  ///         sentinel.
  bool request_stop() noexcept {
    auto *state = m_state.get();
    if (state == nullptr) {
      return false;
    }
    std::uint32_t expected = 0U;
    return state->compare_exchange_strong(
        expected, 1U, std::memory_order_release, std::memory_order_relaxed);
  }

private:
  /// Shared atomic state; bit 0 is the stop flag. Higher bits reserved for
  /// future use.
  std::shared_ptr<std::atomic<std::uint32_t>> m_state;
};

/// Wall-clock deadline expressed as a TSC-cycle threshold.
///
/// `Deadline` stores an absolute `__rdtsc` reading at which the deadline
/// expires. `expired()` compares the current TSC against the threshold. The
/// reading is taken once at construction and never refreshed; the only call
/// sites are inside hot worker bodies where a syscall-based `clock_gettime`
/// would dominate the budget.
///
/// A default-constructed deadline never expires (threshold is `UINT64_MAX`).
/// `Deadline::expired` is `noexcept` and allocation-free.
class Deadline {
public:
  /// Construct a deadline that never expires.
  ///
  /// Used by call sites that do not propagate a deadline. The threshold is set
  /// to the maximum representable cycle count so `expired()` always returns
  /// `false`.
  constexpr Deadline() noexcept = default;

  /// Construct a deadline at an absolute TSC threshold.
  ///
  /// The caller provides a precomputed `__rdtsc()` value; the deadline expires
  /// when the live TSC reaches or passes the threshold. This is low-level:
  /// callers can reuse a single
  /// `__rdtsc` reading taken at job-publish time across many primitives.
  ///
  /// tscThreshold Absolute TSC cycle count at which the deadline expires.
  constexpr explicit Deadline(std::uint64_t tscThreshold) noexcept
      : m_tscThreshold(tscThreshold) {}

  /// Construct a deadline at `now + ms` using a one-time TSC frequency
  /// calibration.
  ///
  /// Calibrates the TSC frequency on first call (5 ms wall-clock window) and
  /// caches the result for the process lifetime. Subsequent calls are a single
  /// `__rdtsc()` plus a multiplication. On non-x86 hosts the calibration
  /// returns 0 and this factory returns the never-expires sentinel.
  ///
  /// ms Wall-clock milliseconds from now until the deadline expires.
  [[nodiscard]] static Deadline fromMillis(std::uint64_t ms) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    const double cyclesPerNs = detail::tscCyclesPerNs();
    if (cyclesPerNs <= 0.0) {
      return Deadline{};
    }
    const auto delta = static_cast<std::uint64_t>(static_cast<double>(ms) *
                                                  1.0e6 * cyclesPerNs);
    return Deadline{__rdtsc() + delta};
#else
    (void)ms;
    return Deadline{};
#endif
  }

  /// Construct a deadline at `now + us`. See `fromMillis` for calibration
  /// details.
  ///
  /// us Wall-clock microseconds from now until the deadline expires.
  [[nodiscard]] static Deadline fromMicros(std::uint64_t us) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    const double cyclesPerNs = detail::tscCyclesPerNs();
    if (cyclesPerNs <= 0.0) {
      return Deadline{};
    }
    const auto delta = static_cast<std::uint64_t>(static_cast<double>(us) *
                                                  1.0e3 * cyclesPerNs);
    return Deadline{__rdtsc() + delta};
#else
    (void)us;
    return Deadline{};
#endif
  }

  /// Check whether the live TSC has reached the deadline's threshold.
  ///
  /// `true` if the deadline is in the past; `false` otherwise. Returns `false`
  /// for the
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
  [[nodiscard]] constexpr std::uint64_t threshold() const noexcept {
    return m_tscThreshold;
  }

private:
  /// Absolute TSC cycle count at which the deadline expires; never-expires
  /// sentinel by default.
  std::uint64_t m_tscThreshold = UINT64_MAX;
};

/// Thrown from a void-returning primitive whose token / deadline cancelled
/// mid-flight.
///
/// The producer rethrows this on the join path so callers can distinguish
/// cancellation from a worker exception. Inherits `std::exception` rather than
/// `std::runtime_error` to keep the type lightweight and free of heap
/// allocation in the constructor.
class cancelled_exception : public std::exception {
public:
  /// Return the diagnostic string identifying the exception kind.
  /// A non-null, statically-stored C-string.
  [[nodiscard]] const char *what() const noexcept override {
    return "citor: cancelled";
  }
};

/// Thrown from a value-producing primitive whose join was cancelled mid-flight.
///
/// Carries a `partial_value` field that holds the deterministic combine of all
/// completed chunks up to the cancellation. For `Determinism::FixedBlockOrder`,
/// the partial result is well-defined (combine of completed chunks `[0,
/// completed)` in chunk-id order). For `OrderTolerant`, the partial value is
/// order-tolerant and reflects whatever workers happened to commit.
///
/// T The value type the cancelled primitive was producing.
template <class T>
class cancelled_value_exception : public std::exception {
public:
  /// Construct with a deterministic-combine partial result.
  ///
  /// partial The combine-of-completed-chunks partial value at the moment of
  /// cancellation.
  explicit cancelled_value_exception(T partial) noexcept(
      std::is_nothrow_move_constructible_v<T>)
      : partial_value(std::move(partial)) {}

  /// Return the diagnostic string identifying the exception kind.
  /// A non-null, statically-stored C-string.
  [[nodiscard]] const char *what() const noexcept override {
    return "citor: cancelled with partial value";
  }

  /// Combined partial result of all chunks that completed before cancellation
  /// observed.
  T partial_value;
};

} // namespace citor

// ===== citor/hints.h =====


namespace citor {

/// Cache-line size used for false-sharing avoidance.
///
/// Hardcoded to 128 bytes rather than
/// `std::hardware_destructive_interference_size` because the standard library
/// value is implementation-defined, varies by `-mtune`, and is therefore an ABI
/// hand grenade across translation units compiled with different flags
/// (P1119R0). 128 bytes is the conservative value for x86-64: Zen-family L1
/// lines are 64 bytes but the L2 / data-line fill prefetcher pulls in pairs, so
/// the smallest false-sharing-safe stride is 128.
///
/// Every contended atomic in the pool is aligned to this value via
/// `alignas(kCacheLine)`. AArch64 ports may want a different value; that path
/// is gated behind a future `#if defined(__x86_64__)` block.
inline constexpr std::size_t kCacheLine = 128;

/// Load-balancing strategy a primitive uses across its participants.
enum class Balance : std::uint8_t {
  /// Worker-strided block partition with no atomics on the hot path.
  /// Block ids are a stable function of `(n, site_tag)` and independent
  /// of worker count, so deterministic reductions get cross-`nJobs`
  /// bit-identity for free.
  StaticUniform,
  /// Workers race on a single relaxed counter (`nextBlock.fetch_add`).
  /// One atomic, no deque, no stealing. Used when per-block cost
  /// varies (kNN, threshold emit).
  DynamicChunked,
};

/// Floating-point determinism contract a reduction primitive promises.
enum class Determinism : std::uint8_t {
  /// Chunk-id pairwise tree combine; bit-identical across worker counts
  /// when `chunk_size = f(n, site_tag)`.
  FixedBlockOrder,
  /// Kahan / Neumaier compensated FP sum on top of the fixed-block
  /// tree.
  KahanCompensated,
};

/// Worker-placement policy. Controls how each worker thread's affinity
/// mask is configured at pool construction.
enum class Affinity : std::uint8_t {
  /// Workers inherit the producer's process affinity mask without
  /// further pinning. The kernel scheduler is free to migrate at will.
  /// Useful for lightly-loaded systems where the kernel's heuristics
  /// outperform any static policy.
  None,
  /// Each worker pinned to exactly one logical CPU from the pool's
  /// allowed-CPU set. Strict: the kernel cannot migrate the worker to
  /// any other CPU. Best for HPC / real-time use where determinism
  /// matters more than the kernel's ability to rebalance.
  PerCpu,
  /// `PerCpu` with one amendment: at `participants == 2` slot 1 lands
  /// on the producer's SMT sibling so the handshake stays L1-resident.
  /// Latency win for dispatch-bound bodies; compute-bound bodies pay
  /// peak-FP throughput when slot 0 and slot 1 share execution units.
  /// Identical to `PerCpu` at `participants > 2`.
  PerCpuSmtPair,
  /// Each worker pinned to its CCD's full logical-CPU set; the kernel
  /// may migrate the worker within its CCD but not across clusters.
  /// Preserves CCD-locality of caches while letting the kernel route
  /// wakes via `select_idle_sibling` and rebalance under transient
  /// intra-CCD load. Default for memory-bound primitives on multi-CCD
  /// parts.
  PerCluster,
};

/// Fork-join steal-victim selection policy.
///
/// Independent of `Affinity` (which controls worker placement); a pool
/// with `Affinity::PerCpu` and `StealPolicy::ClusterLocal` pins each
/// worker to its CPU AND biases its steal probe to same-CCD victims.
enum class StealPolicy : std::uint8_t {
  /// Probe every worker uniformly.
  Global,
  /// Probe same-CCD victims first; fall back to cross-CCD only when the
  /// local cluster has no work to take. Default for
  /// data-locality-sensitive recursive workloads.
  ClusterLocal,
};

/// Per-call priority class consulted when concurrent producers contend
/// on the same pool.
///
/// The pool serializes concurrent `dispatchOne` calls through a
/// two-bucket gate. Within a single primitive (one producer at a time)
/// the priority is hint-only and the workers see the same job either
/// way; the gate only matters when two or more threads call into the
/// pool concurrently. The bucket is minimal (two preferred, one
/// yielding) to keep the gate's overhead off the dispatch hot path.
enum class Priority : std::uint8_t {
  /// Jumps ahead of `Throughput` callers waiting in the dispatch gate.
  Latency,
  /// Default; yields to `Latency` and runs ahead of `Background`.
  Throughput,
  /// Best-effort; yields to throughput on dispatch contention. May be
  /// reordered behind any number of higher-priority dispatches and
  /// offers no progress guarantee under sustained higher-priority
  /// traffic.
  Background,
};

/// Synchronization barrier inserted between two adjacent stages of
/// `parallelChain`.
enum class BarrierKind : std::uint8_t {
  /// Worker proceeds to the next stage without synchronizing.
  None,
  /// Rendezvous across all `participants`.
  Global,
  /// `Global` followed by a chunk-id pairwise-tree reduction.
  DeterministicReduce,
  /// Workers other than rank 0 spin on a producer-done flag while
  /// rank 0 runs the serial body.
  ProducerSerial,
};

/// Runtime-configurable hint POD passed through `parallelForRuntime` and CPO
/// surfaces.
///
/// Compile-time call sites template on a named hint type (typically inheriting
/// from `HintsDefaults`); this struct is the runtime sibling for benchmark
/// drivers and CLI consumers that build a hint at runtime. The fields mirror
/// the static-constexpr members of `HintsDefaults` one-for-one, so the two
/// dispatch paths run the same engine.
struct Hints {
  Balance balance = Balance::StaticUniform;
  Determinism determinism = Determinism::FixedBlockOrder;
  Affinity affinity = Affinity::PerCluster;
  StealPolicy stealPolicy = StealPolicy::ClusterLocal;
  Priority priority = Priority::Throughput;
  /// Estimated per-item cost in nanoseconds; `n * estimatedItemNs` gates the
  /// inline fallback.
  double estimatedItemNs = 0.0;
  /// Minimum wall time per task that justifies fan-out; below this the producer
  /// runs inline. Defaults to `0.0` so the inline-fallback gate is disabled;
  /// presets override upward.
  double minTaskUs = 0.0;
  /// Static block grain when `balance == StaticUniform`; 0 means "derive from
  /// `n / participants`".
  std::size_t chunk = 0;
  /// Whether worker bodies must check the cancellation token at chunk
  /// boundaries.
  bool cancellationChecks = true;
};

/// Compile-time defaults for every field a citor primitive may read off
/// `HintsT`.
///
/// User hint presets inherit from this and override only the fields that
/// differ:
///
///
/// struct MyKahanReduceHints : citor::HintsDefaults {
///   static constexpr Determinism determinism = Determinism::KahanCompensated;
///   static constexpr double minTaskUs = 25.0;
/// };
///
///
/// Fields mirror `Hints` one-for-one. The defaults are conservative:
/// `StaticUniform` balance, `FixedBlockOrder` reductions, no affinity,
/// `Throughput` priority, no estimated cost (the inline-fallback gate is
/// disabled by default), 5us minimum task, derived chunk, and cancellation
/// polls on. Override down for hot loops that have already verified the path.
struct HintsDefaults {
  // DynamicChunked is the default: workers race for blocks via a shared atomic
  // counter, so a slow or descheduled worker does not gate the join on its
  // pre-assigned share. StaticUniform's deterministic block-id-to-rank
  // mapping is required by the chunk-id pairwise-tree reduction in
  // `parallelReduce`; reduce-side hint presets (`KahanReduceHints`,
  // `FixedBlockReduceHints`) override `balance` to StaticUniform explicitly.
  // Cold-dispatch latency is preserved: the dispatcher engages the same
  // `workerStateBase`-driven cold-collapse short-circuit under DynamicChunked
  // as under StaticUniform.
  static constexpr Balance balance = Balance::DynamicChunked;
  static constexpr Determinism determinism = Determinism::FixedBlockOrder;
  static constexpr Affinity affinity = Affinity::PerCluster;
  static constexpr StealPolicy stealPolicy = StealPolicy::ClusterLocal;
  static constexpr Priority priority = Priority::Throughput;
  static constexpr double estimatedItemNs = 0.0;
  static constexpr double minTaskUs = 0.0;
  static constexpr std::size_t chunk = 0;
  static constexpr bool cancellationChecks = true;
};

/// Explicit `Balance::StaticUniform` preset on top of `HintsDefaults`.
///
/// Use when the caller wants the deterministic rank-strided block assignment
/// without inheriting the `DynamicChunked` default. Useful for callers whose
/// body has zero cost variance (every block does identical work) and that
/// benefit from cold-collapse's typed monomorphized fast path. Reduce-side
/// presets that need deterministic chunk-id-to-rank mapping
/// (`KahanReduceHints`, `FixedBlockReduceHints`) inherit through this preset
/// rather than overriding the field individually.
struct StaticHints : HintsDefaults {
  static constexpr Balance balance = Balance::StaticUniform;
};

/// Explicit `Balance::DynamicChunked` preset on top of `HintsDefaults`.
///
/// Sibling of `StaticHints`. Equivalent to `HintsDefaults` today (the default
/// balance is already DynamicChunked) but provides a stable name for callers
/// that want the straggler-tolerant atomic-counter scheduling regardless of how
/// `HintsDefaults` may be retuned in the future.
struct DynamicHints : HintsDefaults {
  static constexpr Balance balance = Balance::DynamicChunked;
};

/// Latency-biased preset: dynamic-chunked balance with `Priority::Latency`.
/// Good first
///        cut for short jobs that want fast first response over peak
///        throughput.
struct LatencyHints : HintsDefaults {
  static constexpr Balance balance = Balance::DynamicChunked;
  static constexpr Priority priority = Priority::Latency;
};

/// Bulk preset: cancellation polls disabled and a 25us minimum task size, tuned
/// for hot
///        cost-uniform parallel-for loops where the body is known not to be
///        cancelled mid-flight.
struct BulkHints : HintsDefaults {
  static constexpr double minTaskUs = 25.0;
  static constexpr bool cancellationChecks = false;
};

/// Reduction preset that selects Kahan-compensated determinism on top of the
/// fixed-block
///        tree. Inherits the rest of `HintsDefaults`.
struct KahanReduceHints : HintsDefaults {
  static constexpr Determinism determinism = Determinism::KahanCompensated;
  static constexpr double minTaskUs = 25.0;
};

/// Reduction preset for plain fixed-block-order reductions without Kahan, for
/// integer or
///        order-insensitive partials. Inherits `HintsDefaults`.
struct FixedBlockReduceHints : HintsDefaults {
  static constexpr double minTaskUs = 25.0;
};

/// Fork-join preset with same-cluster victim biasing for cross-cluster
/// locality. Inherits from `HintsDefaults` and only sets the
/// steal-direction hint explicitly. forkJoin uses its own Chase-Lev
/// deques; the `Balance` field is not consulted on the fork-join hot
/// path.
struct CcdLocalForkJoinHints : HintsDefaults {
  static constexpr StealPolicy stealPolicy = StealPolicy::ClusterLocal;
};

namespace detail {

/// Internal adapter that preserves a hint preset while disabling cancellation
/// polls.
///
/// Used only after a primitive observes that the supplied `CancellationToken`
/// is the never-stopped sentinel. The public hint's scheduling, determinism,
/// affinity, priority, cost model, and chunking semantics are preserved
/// exactly; only the worker-side token poll is compiled out.
///
/// HintsT Source hint preset.
template <class HintsT>
struct NoCancellationHints {
  static constexpr Balance balance = HintsT::balance;
  static constexpr Determinism determinism = HintsT::determinism;
  static constexpr Affinity affinity = HintsT::affinity;
  static constexpr StealPolicy stealPolicy = HintsT::stealPolicy;
  static constexpr Priority priority = HintsT::priority;
  static constexpr double estimatedItemNs = HintsT::estimatedItemNs;
  static constexpr double minTaskUs = HintsT::minTaskUs;
  static constexpr std::size_t chunk = HintsT::chunk;
  static constexpr bool cancellationChecks = false;
};

} // namespace detail

/// Single stage of a `parallelChain`: a callable plus the barrier following it.
///
/// `Stage` owns no resources; it is a lightweight value carrying a reference
/// (or moved-in callable) and the compile-time `BarrierKind` driving the
/// post-stage synchronization. The variadic `parallelChain<ChainHintsT,
/// Stages...>` accepts a parameter pack of these.
///
/// F     Callable type invoked with the chunk descriptor for that stage.
/// After Compile-time barrier inserted after this stage.
template <class F, BarrierKind After>
struct Stage {
  /// Callable invoked once per chunk during this stage of the chain.
  F fn;

  /// Compile-time accessor for the post-stage barrier kind.
  static constexpr BarrierKind barrier = After;
};

/// Helper that constructs a `Stage` while deducing the callable type.
///
/// `makeStage<BarrierKind::Global>(lambda)` returns a `Stage<Lambda,
/// BarrierKind::Global>` without the caller spelling out the callable type. The
/// post-stage barrier is the only template argument the user must supply.
///
/// After Barrier inserted after this stage.
/// F     Deduced callable type.
/// fn     The callable to wrap.
template <BarrierKind After, class F>
constexpr auto makeStage(F &&fn) noexcept(
    noexcept(Stage<std::decay_t<F>, After>{std::forward<F>(fn)})) {
  return Stage<std::decay_t<F>, After>{std::forward<F>(fn)};
}

/// Runtime-configurable chain-shape hints supplied to `parallelChainRuntime`.
///
/// Compile-time call sites template on a named chain hint type (typically
/// inheriting from `ChainHintsDefaults`). This struct mirrors the
/// static-constexpr members of those types for the runtime overload.
struct ChainHints {
  Balance balance = Balance::StaticUniform;
  Priority priority = Priority::Throughput;
  /// Whether downstream stages reuse the upstream worker's chunk for cache
  /// locality.
  bool pipelineSameChunk = true;
  /// Whether worker bodies must check the cancellation token at chunk
  /// boundaries.
  bool cancellationChecks = true;
  /// Dynamic block grain shared by every stage when same-chunk pipelining is
  /// disabled.
  std::size_t chunk = 0;
};

/// Compile-time defaults for every field `parallelChain` reads off
/// `ChainHintsT`.
///
/// User chain hint presets inherit from this and override only the fields that
/// differ.
struct ChainHintsDefaults {
  static constexpr Balance balance = Balance::StaticUniform;
  static constexpr Priority priority = Priority::Throughput;
  static constexpr bool pipelineSameChunk = true;
  static constexpr bool cancellationChecks = true;
  static constexpr std::size_t chunk = 0;
};

/// Dynamic per-stage chain preset for globally synchronized, skewed stage
/// bodies.
///
/// Same-chunk pipelining keeps a worker on its contiguous slice across every
/// stage. That is ideal when stages reuse per-slot cache-local state.
/// Dynamic-chain mode opts out of that guarantee for stage packs where every
/// stage has a global-style barrier: each stage is split into chunks, and
/// participants claim chunks from a per-stage counter. Mixed packs keep the
/// same-chunk engine so `BarrierKind::None` and `BarrierKind::ProducerSerial`
/// semantics are preserved. Use this preset when per-item cost varies enough
/// that fixed slot ownership would create stragglers.
struct DynamicChainHints : ChainHintsDefaults {
  static constexpr Balance balance = Balance::DynamicChunked;
  static constexpr bool pipelineSameChunk = false;
};

} // namespace citor

// ===== citor/chain.h =====



namespace citor {

/// Construct a `Stage` with the `BarrierKind::None` post-stage barrier.
///
/// The "static" naming reflects that a worker proceeds to the next stage
/// without synchronizing on any shared state -- each worker increments its
/// local stage epoch and runs the next body. Use for stages whose downstream
/// consumer reads strictly per-worker state (no cross-worker reads).
///
/// F Deduced callable type.
/// name Diagnostic identifier surfaced through trace tooling; kept on the stage
/// value for
///              future plumbing (the pool does not currently consume it).
/// fn   Stage body invoked as `fn(stageIdx, slot, lo, hi)`.
/// A `Stage<decay_t<F>, BarrierKind::None>` carrying the callable.
template <class F>
[[nodiscard]] constexpr auto
staticStage([[maybe_unused]] const char *name,
            F &&fn) noexcept(noexcept(Stage<std::decay_t<F>, BarrierKind::None>{
    std::forward<F>(fn)})) -> Stage<std::decay_t<F>, BarrierKind::None> {
  return Stage<std::decay_t<F>, BarrierKind::None>{std::forward<F>(fn)};
}

/// Construct a `Stage` with the `BarrierKind::Global` post-stage barrier.
///
/// Every worker rendezvouses on the producer-driven stage epoch before any
/// worker may begin the next stage. Use when the next stage reads state that
/// any upstream worker may have written.
///
/// F Deduced callable type.
/// name Diagnostic identifier surfaced through trace tooling.
/// fn   Stage body invoked as `fn(stageIdx, slot, lo, hi)`.
/// A `Stage<decay_t<F>, BarrierKind::Global>` carrying the callable.
template <class F>
[[nodiscard]] constexpr auto
globalStage([[maybe_unused]] const char *name, F &&fn) noexcept(
    noexcept(Stage<std::decay_t<F>, BarrierKind::Global>{std::forward<F>(fn)}))
    -> Stage<std::decay_t<F>, BarrierKind::Global> {
  return Stage<std::decay_t<F>, BarrierKind::Global>{std::forward<F>(fn)};
}

/// Construct a `Stage` with the `BarrierKind::DeterministicReduce` post-stage
///        barrier.
///
/// Behaves as a global rendezvous in v1; the deterministic chunk-id pairwise
/// tree reduction is the user's own concern inside the stage body (callers run
/// a `parallelReduce` with the same fixed-block shape from inside the stage).
/// The chain primitive guarantees the global sync; the reduction is the call
/// site's responsibility.
///
/// F Deduced callable type.
/// name Diagnostic identifier surfaced through trace tooling.
/// fn   Stage body invoked as `fn(stageIdx, slot, lo, hi)`.
/// A `Stage<decay_t<F>, BarrierKind::DeterministicReduce>` carrying the
/// callable.
template <class F>
[[nodiscard]] constexpr auto
reduceStage([[maybe_unused]] const char *name, F &&fn) noexcept(
    noexcept(Stage<std::decay_t<F>, BarrierKind::DeterministicReduce>{
        std::forward<F>(fn)}))
    -> Stage<std::decay_t<F>, BarrierKind::DeterministicReduce> {
  return Stage<std::decay_t<F>, BarrierKind::DeterministicReduce>{
      std::forward<F>(fn)};
}

/// Construct a `Stage` with the `BarrierKind::ProducerSerial` post-stage
/// barrier.
///
/// The producer (slot 0) runs the stage body alone; non-producer workers spin
/// on the producer-done flag. Use for stages whose body is inherently serial
/// (centroid update divide, summary stats publish) that should not be
/// replicated across slots.
///
/// F Deduced callable type.
/// name Diagnostic identifier surfaced through trace tooling.
/// fn   Stage body invoked as `fn(stageIdx, slot, lo, hi)`.
/// A `Stage<decay_t<F>, BarrierKind::ProducerSerial>` carrying the callable.
template <class F>
[[nodiscard]] constexpr auto
serialStage([[maybe_unused]] const char *name, F &&fn) noexcept(noexcept(
    Stage<std::decay_t<F>, BarrierKind::ProducerSerial>{std::forward<F>(fn)}))
    -> Stage<std::decay_t<F>, BarrierKind::ProducerSerial> {
  return Stage<std::decay_t<F>, BarrierKind::ProducerSerial>{
      std::forward<F>(fn)};
}

} // namespace citor

// ===== citor/cpos/bulk_for_queries.h =====



namespace citor {

/// Tag type identifying the `citor::bulkForQueries` customization point.
///
/// Passed as the first argument to every `tag_invoke` overload that implements
/// `bulkForQueries` for a given executor. Mirrors the `ParallelForTag` pattern
/// so the extension surface is symmetric with the rest of the parallel CPO
/// family.
struct BulkForQueriesTag {};

namespace detail {

/// Function-object backing the `citor::bulkForQueries` customization point.
///
/// Dispatches to an unqualified `tag_invoke` call so ADL can reach overloads in
/// the executor's namespace. The function object itself is passed as the first
/// argument so overloads can key on the CPO identity in addition to the tag.
///
/// The `HintsT` template parameter is a *type*, not a value: that lets the
/// friend overload on `ThreadPool` template on the same `HintsT` and
/// monomorphize identically to the member-template call. Both surfaces
/// ultimately route through the same engine; the CPO has zero runtime hint
/// dispatch cost.
struct BulkForQueriesFn {
  /// Forward to the executor's `tag_invoke` overload, supplying a default
  /// `HintsT{}` value.
  ///
  /// The hint type is supplied as an explicit template parameter at the call
  /// site (mirroring the member-template surface) so the executor's overload
  /// can specialize on it via `if constexpr` or a regular template parameter.
  ///
  /// HintsT  Hint type whose `static constexpr` members drive compile-time
  /// policy. Pool    Executor type. QueryFn Callable invoked once per chunk as
  ///                 `QueryFn(std::size_t qFirst, std::size_t qLast)`; the body
  ///                 must process every query index in `[qFirst, qLast)`.
  /// pool    Executor instance.
  /// q       Total query count; the engine fans `[0, q)` across workers.
  /// fn      Callable invoked over each chunk of the query range.
  /// tok     Cancellation token observed at chunk boundaries.
  template <class HintsT, class Pool, class QueryFn>
  void operator()(Pool &pool, std::size_t q, QueryFn &&fn,
                  CancellationToken tok = CancellationToken{}) const {
    tag_invoke(*this, pool, q, std::forward<QueryFn>(fn), HintsT{},
               std::move(tok));
  }
};

} // namespace detail

/// Customization-point object for fanning many independent queries across a
/// pool.
///
/// Calling `bulkForQueries<HintsT>(pool, q, fn)` dispatches through unqualified
/// `tag_invoke`; the executor's overload partitions `[0, q)` and invokes
/// `fn(qFirst, qLast)` on each chunk so the body can run every query index in
/// the range. A `Balance::DynamicChunked` policy override on the hint amortizes
/// per-query traversal-depth skew across workers; sites whose per-query cost is
/// uniform can keep the `citor::HintsDefaults` static-uniform default.
///
/// Output stability: per-query results MUST be written to a per-query slot
/// (`out[q]` keyed on the query index passed into `fn`). The chunk dispatch
/// order varies across worker counts; only indexing by query index gives a
/// bit-identical output regardless of policy.
inline constexpr detail::BulkForQueriesFn bulkForQueries{};

} // namespace citor

// ===== citor/cpos/fork_join.h =====



namespace citor {

/// Tag type identifying the `citor::forkJoin` customization point.
///
/// Passed as the first argument to every `tag_invoke` overload that implements
/// `forkJoin` for a given executor. Mirrors the `ParallelForTag` pattern so the
/// extension surface is symmetric with the rest of the parallel CPO family.
struct ForkJoinTag {};

namespace detail {

/// Function-object backing the `citor::forkJoin` customization point.
///
/// Dispatches to an unqualified `tag_invoke` call so ADL can reach overloads in
/// the executor's namespace. The function object itself is passed as the first
/// argument so overloads can key on the CPO identity in addition to the tag.
///
/// The `HintsT` template parameter is a *type*, not a value: that lets the
/// friend overload on `ThreadPool` template on the same `HintsT` and
/// monomorphize identically to the member-template call. Both surfaces
/// ultimately route through the same engine; the CPO has zero runtime hint
/// dispatch cost.
///
/// Recursive tasks call back into the customization point from worker context;
/// each task receives a `ForkJoinScope` reference (defined by the executor) it
/// uses to spawn children. The scope abstraction keeps the CPO surface
/// decoupled from the engine's task-descriptor encoding.
struct ForkJoinFn {
  /// Forward to the executor's `tag_invoke` overload, supplying a default
  /// `HintsT{}` value.
  ///
  /// The hint type is supplied as an explicit template parameter at the call
  /// site (mirroring the member-template surface) so the executor's overload
  /// can specialize on it via `if constexpr` or a regular template parameter.
  ///
  /// HintsT  Hint type whose `static constexpr` members drive compile-time
  /// policy. Pool    Executor type. TaskFns Variadic pack of task callables,
  /// each invocable as `void(void)` or
  ///                 `void(ForkJoinScope&)` per the executor's contract.
  /// pool    Executor instance.
  /// tok     Cancellation token observed at task-boundary chunks.
  /// fns     Variadic pack of root tasks.
  template <class HintsT, class Pool, class... TaskFns>
  void operator()(Pool &pool, CancellationToken tok, TaskFns &&...fns) const {
    tag_invoke(*this, pool, std::move(tok), HintsT{},
               std::forward<TaskFns>(fns)...);
  }
};

} // namespace detail

/// Customization-point object for recursive fork-join with work-stealing.
///
/// Calling `forkJoin<HintsT>(pool, tok, fns...)` dispatches through unqualified
/// `tag_invoke`; the executor's overload submits each task to a per-worker
/// work-stealing deque and the call returns once every task (and every
/// recursive child spawned during their bodies) has retired. The producer
/// participates as slot 0 and steals from other workers' deques when its own
/// drains.
///
/// Cancellation: the supplied `CancellationToken` is observed at task-boundary
/// chunks. A stopped token causes participants to stop spawning fresh recursive
/// children and to drain the outstanding task count without admitting more
/// work; the join still rendezvous with every worker before returning.
///
/// Exception handling: if any task body throws, the first exception is captured
/// and rethrown from the producer after every outstanding task has retired.
/// Subsequent throws drop. The remaining tasks are cancelled (the cancellation
/// flag is set as part of the throw response) so the join does not block on
/// quiescence.
///
/// The `HintsT` policy carries the `StealPolicy` choice that the engine
/// uses to bias victim selection: `StealPolicy::ClusterLocal` prefers
/// same-CCD victims, mirroring the topology's shared-L3 grouping.
/// `StealPolicy::Global` falls back to a uniform xorshift random victim
/// probe.
inline constexpr detail::ForkJoinFn forkJoin{};

} // namespace citor

// ===== citor/cpos/inclusive_scan.h =====



namespace citor {

/// Tag type identifying the `citor::inclusiveScan` customization point.
struct InclusiveScanTag {};

namespace detail {

/// Function-object backing the `citor::inclusiveScan` customization
/// point.
///
/// `inclusiveScan` is an opinionated buffer-to-buffer prefix-sum entry
/// distinct from `parallelScan`. The two differ in what the engine
/// observes:
///
///   * `parallelScan` takes a user body. The engine knows the chunk
///     ranges but not the addresses of the input or output buffers, so
///     it cannot prefetch, NT-store, or otherwise reorder the user's
///     memory accesses. It is fully general (any monoid, any
///     side-effecting body) at the cost of leaving micro-architectural
///     headroom on the table.
///   * `inclusiveScan` takes the input and output buffers directly. The
///     engine owns the inner loop and is free to use whatever memory
///     traffic shape minimises wall time on the host: decoupled-lookback
///     single-pass, `PREFETCHW` write-prefetch ahead of Pass 2, NT stores
///     on workloads where the output is larger than L3, AVX-512 in-register
///     scan, per-cluster lookback chains, etc.
///
/// The tradeoff: `inclusiveScan` is restricted to plain memory-to-memory
/// scans of trivially-relocatable types under a user-supplied associative
/// combiner. Bodies that need to inspect or mutate side state, allocate,
/// or otherwise reach beyond `[in, out)` should keep using
/// `parallelScan`.
struct InclusiveScanFn {
  /// Compute an inclusive prefix scan from `in` to `out` under `prefix`.
  ///
  /// `in` and `out` must have equal length and may alias only when the
  /// caller has made the alias safe (the engine reads `in[i]` before
  /// writing `out[i]` for every `i` so `in == out` is well-formed). The
  /// returned value is the inclusive total at the right edge --
  /// `prefix(prefix(... prefix(identity, in[0]) ...), in[n-1])` -- and
  /// matches the value Blelloch's two-pass scan produces.
  ///
  /// The hint type carries compile-time policy (per-tile size cap,
  /// affinity, priority); the engine consults `HintsT::stealPolicy` only
  /// for any nested fork/join the implementation may use internally
  /// (currently none).
  template <class HintsT, class Pool, class T, class PrefixFn>
  [[nodiscard]] T
  operator()(Pool &pool, std::span<const T> in, std::span<T> out, T identity,
             PrefixFn &&prefix,
             const CancellationToken &tok = CancellationToken{}) const {
    return tag_invoke(*this, pool, in, out, std::move(identity),
                      std::forward<PrefixFn>(prefix), HintsT{}, tok);
  }
};

} // namespace detail

/// Customization-point object for the buffer-to-buffer inclusive
/// prefix scan. See `detail::InclusiveScanFn` for the contract.
inline constexpr detail::InclusiveScanFn inclusiveScan{};

} // namespace citor

// ===== citor/cpos/parallel_chain.h =====



namespace citor {

/// Tag type identifying the `citor::parallelChain` customization point.
///
/// Passed as the first argument to every `tag_invoke` overload that implements
/// `parallelChain` for a given executor. Mirrors the `ParallelForTag` pattern
/// so the extension surface is symmetric with the rest of the parallel CPO
/// family.
struct ParallelChainTag {};

namespace detail {

/// Function-object backing the `citor::parallelChain` customization point.
///
/// Dispatches to an unqualified `tag_invoke` call so ADL can reach overloads in
/// the executor's namespace. The function object itself is passed as the first
/// argument so overloads can key on the CPO identity in addition to the tag.
///
/// The `ChainHintsT` template parameter is a *type*, not a value: that lets the
/// friend overload on `ThreadPool` template on the same `ChainHintsT` and
/// monomorphize identically to the member-template call. Both surfaces
/// ultimately route through the same engine; the CPO has zero runtime hint
/// dispatch cost.
struct ParallelChainFn {
  /// Forward to the executor's `tag_invoke` overload, supplying a default
  /// `ChainHintsT{}`.
  ///
  /// The chain hint type is supplied as an explicit template parameter at the
  /// call site (mirroring the member-template surface) so the executor's
  /// overload can specialize on it via `if constexpr` or a regular template
  /// parameter. The variadic stage pack flows through perfect forwarding so
  /// each stage's compile-time `BarrierKind` is preserved.
  ///
  /// ChainHintsT Chain hint type whose `static constexpr` members drive
  /// compile-time
  ///                     policy.
  /// Pool        Executor type.
  /// Stages      Variadic pack of `Stage<F, BarrierKind>` value types.
  /// pool   Executor instance.
  /// n      Row-range upper bound; partitioned across slots as
  ///                `[n*slot/P, n*(slot+1)/P)`.
  /// stages Stage pack invoked in submission order with the declared barrier
  /// between
  ///                consecutive stages.
  template <class ChainHintsT, class Pool, class... Stages>
  void operator()(Pool &pool, std::size_t n, Stages &&...stages) const {
    tag_invoke(*this, pool, n, ChainHintsT{}, CancellationToken{},
               std::forward<Stages>(stages)...);
  }

  /// Forward with an explicit cancellation token.
  ///
  /// Available for callers that need to wire a token in alongside a chain hint
  /// type.
  template <class ChainHintsT, class Pool, class... Stages>
  void operator()(Pool &pool, std::size_t n, CancellationToken tok,
                  Stages &&...stages) const {
    tag_invoke(*this, pool, n, ChainHintsT{}, std::move(tok),
               std::forward<Stages>(stages)...);
  }
};

} // namespace detail

/// Customization-point object for the multi-stage chain primitive.
///
/// Calling `parallelChain<ChainHintsT>(pool, n, stages...)` dispatches through
/// unqualified `tag_invoke`; the executor's overload runs each stage in
/// submission order with the declared post-stage barrier. The producer
/// participates as slot 0 across every stage.
///
/// The chain primitive amortises the cost of fanning out a multi-stage
/// multi-stage compute-fan-out pipeline: one descriptor publish drives the
/// entire chain, with per-stage rendezvous handled in user-space spin-wait. Use
/// when the inter-stage transition latency is on the same order as a single
/// `parallelFor` dispatch and the chain has at least two stages.
inline constexpr detail::ParallelChainFn parallelChain{};

} // namespace citor

// ===== citor/cpos/parallel_for.h =====



namespace citor {

/// Tag type identifying the `citor::parallelFor` customization point.
///
/// Passed as the first argument to every `tag_invoke` overload that implements
/// `parallelFor` for a given executor. Mirrors the tag-based dispatch pattern
/// so the extension surface is symmetric with the rest of the codebase.
struct ParallelForTag {};

namespace detail {

/// Function-object backing the `citor::parallelFor` customization point.
///
/// Dispatches to an unqualified `tag_invoke` call so ADL can reach overloads in
/// the executor's namespace. The function object itself is passed as the first
/// argument so overloads can key on the CPO identity in addition to the tag.
///
/// The `HintsT` template parameter is a *type*, not a value: that lets the
/// friend overload on `ThreadPool` template on the same `HintsT` and
/// monomorphize identically to the member-template call. Both surfaces
/// ultimately route through the same engine; the CPO has zero runtime hint
/// dispatch cost.
struct ParallelForFn {
  /// Forward to the executor's `tag_invoke` overload, supplying a default
  /// `HintsT{}` value.
  ///
  /// The hint type is supplied as an explicit template parameter at the call
  /// site (mirroring the member-template surface) so the executor's overload
  /// can specialize on it via `if constexpr` or a regular template parameter.
  ///
  /// HintsT Hint type whose `static constexpr` members drive compile-time
  /// policy. Pool   Executor type. F      Callable type invoked once per block
  /// as `F(std::size_t lo, std::size_t hi)`. pool   Executor instance. first
  /// Inclusive lower bound of the iteration range. last   Exclusive upper bound
  /// of the iteration range. fn     Callable invoked over each block. tok
  /// Cancellation token observed at chunk boundaries.
  template <class HintsT, class Pool, class F>
  void operator()(Pool &pool, std::size_t first, std::size_t last, F &&fn,
                  CancellationToken tok = CancellationToken{}) const {
    tag_invoke(*this, pool, first, last, std::forward<F>(fn), HintsT{},
               std::move(tok));
  }
};

} // namespace detail

/// Customization-point object for parallel iteration over a `[first, last)`
/// range.
///
/// Calling `parallelFor<HintsT>(pool, first, last, fn)` dispatches through
/// unqualified `tag_invoke`; the executor's overload runs the iteration
/// synchronously and returns once every block has completed. The hint type
/// carries the compile-time policy (balance, partition, chunk grain,
/// inline-fallback parameters) so every overload can specialize without runtime
/// branching.
inline constexpr detail::ParallelForFn parallelFor{};

} // namespace citor

// ===== citor/cpos/parallel_reduce.h =====



namespace citor {

/// Tag type identifying the `citor::parallelReduce` customization point.
///
/// Passed as the first argument to every `tag_invoke` overload that implements
/// `parallelReduce` for a given executor. Mirrors the `ParallelForTag` pattern
/// so the extension surface is symmetric with the rest of the parallel CPO
/// family.
struct ParallelReduceTag {};

namespace detail {

/// Function-object backing the `citor::parallelReduce` customization point.
///
/// Dispatches to an unqualified `tag_invoke` call so ADL can reach overloads in
/// the executor's namespace. The function object itself is passed as the first
/// argument so overloads can key on the CPO identity in addition to the tag.
///
/// The `HintsT` template parameter is a *type*, not a value: that lets the
/// friend overload on `ThreadPool` template on the same `HintsT` and
/// monomorphize identically to the member-template call. Both surfaces
/// ultimately route through the same engine; the CPO has zero runtime hint
/// dispatch cost.
struct ParallelReduceFn {
  /// Forward to the executor's `tag_invoke` overload, supplying a default
  /// `HintsT{}` value.
  ///
  /// Returns the reduction result computed by the executor. The executor's
  /// overload is responsible for selecting `Determinism::FixedBlockOrder` vs
  /// `Determinism::KahanCompensated` reduction shapes from the static-constexpr
  /// members of |HintsT|.
  ///
  /// HintsT  Hint type whose `static constexpr` members drive compile-time
  /// policy. Pool    Executor type. T       Reduction value type. Map Per-block
  /// map callable: `T(std::size_t lo, std::size_t hi)`. Combine Binary combine
  /// callable: `T(T, T)`. pool    Executor instance. first   Inclusive lower
  /// bound of the iteration range. last    Exclusive upper bound of the
  /// iteration range. init    Identity value used when the range is empty AND
  /// seed for combiner. map     Callable that produces a partial value over a
  /// chunk range. combine Binary combiner over partial values. tok Cancellation
  /// token observed at chunk boundaries. The reduction result, identical across
  /// worker counts when the hint requests a
  ///         deterministic combine tree.
  template <class HintsT, class Pool, class T, class Map, class Combine>
  [[nodiscard]] T
  operator()(Pool &pool, std::size_t first, std::size_t last, T init, Map &&map,
             Combine &&combine,
             CancellationToken tok = CancellationToken{}) const {
    return tag_invoke(*this, pool, first, last, std::move(init),
                      std::forward<Map>(map), std::forward<Combine>(combine),
                      HintsT{}, std::move(tok));
  }
};

} // namespace detail

/// Customization-point object for parallel reductions over a `[first, last)`
/// range.
///
/// Calling `parallelReduce<HintsT>(pool, first, last, init, map, combine)`
/// dispatches through unqualified `tag_invoke`; the executor's overload runs
/// the reduction synchronously and returns the combined result. The hint type
/// carries the compile-time policy (balance, determinism, chunk grain,
/// inline-fallback parameters) so every overload can specialize without runtime
/// branching.
inline constexpr detail::ParallelReduceFn parallelReduce{};

} // namespace citor

// ===== citor/cpos/parallel_scan.h =====



namespace citor {

/// Tag type identifying the `citor::parallelScan` customization point.
///
/// Passed as the first argument to every `tag_invoke` overload that implements
/// `parallelScan` for a given executor. Mirrors the `ParallelForTag` pattern so
/// the extension surface is symmetric with the rest of the parallel CPO family.
struct ParallelScanTag {};

namespace detail {

/// Function-object backing the `citor::parallelScan` customization point.
///
/// Dispatches to an unqualified `tag_invoke` call so ADL can reach overloads in
/// the executor's namespace. The function object itself is passed as the first
/// argument so overloads can key on the CPO identity in addition to the tag.
///
/// The `HintsT` template parameter is a *type*, not a value: that lets the
/// friend overload on `ThreadPool` template on the same `HintsT` and
/// monomorphize identically to the member-template call. Both surfaces
/// ultimately route through the same engine; the CPO has zero runtime hint
/// dispatch cost.
struct ParallelScanFn {
  /// Forward to the executor's `tag_invoke` overload, supplying a default
  /// `HintsT{}` value.
  ///
  /// The hint type is supplied as an explicit template parameter at the call
  /// site (mirroring the member-template surface) so the executor's overload
  /// can specialize on it via `if constexpr` or a regular template parameter.
  ///
  /// Returns the final inclusive-prefix accumulator computed across every
  /// chunk: this matches the `op(prefix[last], partial[last])` value Blelloch's
  /// two-pass scan produces at the right edge, and gives callers a single value
  /// to consume without a follow-up reduction.
  ///
  /// HintsT   Hint type whose `static constexpr` members drive compile-time
  /// policy. Pool     Executor type. T        Reduction value type. BodyFn
  /// Per-chunk body callable: `T(std::size_t chunkId, std::size_t lo,
  ///                  std::size_t hi, T initial, T* out)`.
  /// PrefixFn Binary cross-chunk reduction operator: `T(T a, T b)`.
  /// pool     Executor instance.
  /// n        Range length; partitioned across slots as `[n*slot/P,
  /// n*(slot+1)/P)`. identity Identity value seeded into the first chunk's
  /// `initial` and returned for `n==0`. body     Per-chunk body callable
  /// invoked twice per slot (Pass 1 with `initial=identity`, Pass 2 with
  /// `initial=exclusivePrefix[slot]`). prefix   Binary combiner producing the
  /// cross-chunk exclusive-prefix sequence. tok      Cancellation token
  /// observed at pass boundaries. The inclusive prefix accumulator at the right
  /// edge of the scan.
  template <class HintsT, class Pool, class T, class BodyFn, class PrefixFn>
  [[nodiscard]] T
  operator()(Pool &pool, std::size_t n, T identity, BodyFn &&body,
             PrefixFn &&prefix,
             CancellationToken tok = CancellationToken{}) const {
    return tag_invoke(*this, pool, n, std::move(identity),
                      std::forward<BodyFn>(body),
                      std::forward<PrefixFn>(prefix), HintsT{}, std::move(tok));
  }
};

} // namespace detail

/// Customization-point object for the Blelloch two-pass parallel prefix scan.
///
/// Calling `parallelScan<HintsT>(pool, n, identity, body, prefix)` dispatches
/// through unqualified `tag_invoke`; the executor's overload runs the scan
/// synchronously and returns the inclusive accumulator at the right edge. The
/// hint type carries the compile-time policy (balance, chunk grain,
/// inline-fallback parameters) so every overload can specialize without runtime
/// branching.
///
/// The two-pass shape avoids the `n^2/p` sequential bottleneck of a naive
/// split-recombine: Pass 1 computes per-chunk partial sums in parallel, the
/// producer computes the chunk-level exclusive prefixes serially in
/// `O(participants)`, and Pass 2 re-runs the body with each chunk's exclusive
/// prefix as `initial` to write the final scan output.
inline constexpr detail::ParallelScanFn parallelScan{};

} // namespace citor

// ===== citor/cpos/run_plex.h =====



namespace citor {

/// Tag type identifying the `citor::runPlex` customization point.
///
/// Passed as the first argument to every `tag_invoke` overload that implements
/// `runPlex` for a given executor. Mirrors the `ParallelForTag` pattern so the
/// extension surface is symmetric with the rest of the parallel CPO family.
struct RunPlexTag {};

namespace detail {

/// Function-object backing the `citor::runPlex` customization point.
///
/// Dispatches to an unqualified `tag_invoke` call so ADL can reach overloads in
/// the executor's namespace. The function object itself is passed as the first
/// argument so overloads can key on the CPO identity in addition to the tag.
///
/// The `HintsT` template parameter is a *type*, not a value: that lets the
/// friend overload on `ThreadPool` template on the same `HintsT` and
/// monomorphize identically to the member-template call. Both surfaces
/// ultimately route through the same engine; the CPO has zero runtime hint
/// dispatch cost.
struct RunPlexFn {
  /// Forward to the executor's `tag_invoke` overload, supplying a default
  /// `HintsT{}` value.
  ///
  /// The hint type is supplied as an explicit template parameter at the call
  /// site (mirroring the member-template surface) so the executor's overload
  /// can specialize on it via `if constexpr` or a regular template parameter.
  ///
  /// HintsT  Hint type whose `static constexpr` members drive compile-time
  /// policy. Pool    Executor type. Phase   Phase callable: `void(std::size_t
  /// phaseIdx, std::uint32_t slot,
  ///                                       std::size_t lo, std::size_t hi)`.
  /// pool     Executor instance.
  /// nPhases  Number of phases to run; `0` is a no-op.
  /// n        Row-range upper bound; partitioned across slots as
  ///                  `[n*slot/P, n*(slot+1)/P)`.
  /// phaseFn  Callable invoked once per `(phase, slot)` pair.
  /// tok      Cancellation token observed at phase boundaries.
  template <class HintsT, class Pool, class Phase>
  void operator()(Pool &pool, std::size_t nPhases, std::size_t n,
                  Phase &&phaseFn,
                  CancellationToken tok = CancellationToken{}) const {
    tag_invoke(*this, pool, nPhases, n, std::forward<Phase>(phaseFn), HintsT{},
               std::move(tok));
  }
};

} // namespace detail

/// Customization-point object for the persistent-worker plex pattern.
///
/// Calling `runPlex<HintsT>(pool, nPhases, n, phaseFn)` dispatches through
/// unqualified `tag_invoke`; the executor's overload runs `nPhases` phases in
/// order. Each phase invokes `phaseFn(phaseIdx, slot, lo, hi)` once per
/// participant slot, where `(lo, hi)` is the slot's contiguous range over `[0,
/// n)`. The producer participates as slot 0.
///
/// The plex stays in user-space spin-wait between phases (no futex round-trip
/// per phase), so inter-phase transition latency is dominated by
/// cache-coherency on the per-worker `done` flags. Use only when the user has a
/// known number of phases and inter-phase latency matters more than overall
/// work; for one-shot fan-outs `parallelFor` is cheaper.
inline constexpr detail::RunPlexFn runPlex{};

} // namespace citor

// ===== citor/cpos/submit_detached.h =====



namespace citor {

/// Tag type identifying the `citor::submitDetached` customization point.
///
/// Passed as the first argument to every `tag_invoke` overload that implements
/// `submitDetached` for a given executor. Mirrors the `ForkJoinTag` pattern so
/// the extension surface is symmetric with the rest of the parallel CPO family.
struct SubmitDetachedTag {};

namespace detail {

/// Function-object backing the `citor::submitDetached` customization point.
///
/// Dispatches to an unqualified `tag_invoke` call so ADL can reach overloads in
/// the executor's namespace. The function object itself is passed as the first
/// argument so overloads can key on the CPO identity in addition to the tag.
///
/// The `HintsT` template parameter is a *type*, not a value: that lets the
/// friend overload on `ThreadPool` template on the same `HintsT` and
/// monomorphize identically to the member-template call. The hint type is
/// reserved for future routing decisions (priority class, affinity), but is
/// unused on the current shape since detached submission has no partition /
/// chunk schedule.
struct SubmitDetachedFn {
  /// Forward to the executor's `tag_invoke` overload, supplying a default
  /// `HintsT{}` value.
  ///
  /// The hint type is supplied as an explicit template parameter at the call
  /// site (mirroring the member-template surface) so the executor's overload
  /// can specialize on it via `if constexpr` or a regular template parameter.
  ///
  /// HintsT Hint type whose `static constexpr` members drive compile-time
  /// policy. Pool   Executor type. TaskFn Task callable, invocable as
  /// `void(void)`. pool   Executor instance. fn     Task body the executor runs
  /// without joining. tok    Cancellation token observed cooperatively by the
  /// body.
  template <class HintsT, class Pool, class TaskFn>
  void operator()(Pool &pool, TaskFn &&fn,
                  CancellationToken tok = CancellationToken{}) const {
    tag_invoke(*this, pool, HintsT{}, std::forward<TaskFn>(fn), std::move(tok));
  }
};

} // namespace detail

/// Customization-point object for fire-and-forget detached submission.
///
/// Calling `submitDetached<HintsT>(pool, fn, tok)` dispatches through
/// unqualified `tag_invoke`; the executor's overload accepts ownership of the
/// task, runs it asynchronously, and returns immediately. The pool's destructor
/// blocks until every detached task has completed; until then, the pool's
/// lifetime extends every in-flight body.
///
/// Cancellation: the supplied `CancellationToken` is observed cooperatively by
/// the body (the executor does not preempt). A pre-cancelled token
/// short-circuits the body before any work runs.
///
/// Exception handling: an exception escaping a detached body is captured into a
/// per-pool slot (latched on first throw) and surfaced via
/// `ThreadPool::lastDetachedException()`. The pool does not call
/// `std::terminate` on a detached throw, and subsequent throws drop on the
/// floor.
inline constexpr detail::SubmitDetachedFn submitDetached{};

} // namespace citor

// ===== citor/detail/cpu_relax.h =====

// Spin-loop CPU hint used by every busy-wait in the engine.
//
// Factored out of `worker_loop.h` so headers that only need the hint
// (`lookback_scan.h`, `coherence_probe.h`, ...) avoid pulling in the
// full worker dispatch state.


#if defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#endif

#ifdef _MSC_VER
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
#ifdef __SIZEOF_INT128__
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

// ===== citor/detail/chain_state.h =====



namespace citor::detail {

/// Per-worker per-stage completion slot used by every barrier kind of
///        `citor::ThreadPool::parallelChain`.
///
/// Each slot lives on its own `citor::kCacheLine` -sized line so a worker's
/// release-store on `done` for one stage cannot invalidate a neighbouring slot
/// the producer or downstream worker is acquiring. The owning worker writes
/// `done` exactly once per stage (or zero times when its slice is empty) via
/// release; downstream consumers read it via acquire to confirm the upstream
/// worker's slice for that stage has retired.
///
/// Padding-suppression note: the layout keeps the atomic on its own
/// `kCacheLine`-sized line, so the analyser's "excessive padding" warning is
/// the design trade-off we want -- false-sharing avoidance over byte-tight
/// packing.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct alignas(kCacheLine) ChainDoneSlot {
  /// Stage epoch the worker last finished. Downstream consumer waits until
  /// `done >= stageIdx + 1` before admitting the next stage's body for this
  /// worker's slot.
  std::atomic<std::uint64_t> done{0};
};

/// Per-stage dynamic-chain claim counter.
///
/// Dynamic-chain mode allocates one counter per stage before dispatch.
/// Participants claim blocks from the current stage's counter, so advancing to
/// the next stage does not require slot 0 to reset a shared counter and publish
/// a separate ready epoch. Each counter lives on its own line because it is the
/// only contended atomic while that stage is running.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct alignas(kCacheLine) ChainDynamicStageCounter {
  /// Next dynamic block id to claim for this stage.
  std::atomic<std::size_t> next{0};
};

/// Stack-resident state shared by the producer and background workers across
/// all stages of a
///        single `parallelChain` call.
///
/// Layout invariants:
/// - `chainCancelled` lives on its own line so cancellation broadcast does not
/// interfere with the
///   producer's hot path.
/// - `done[w]` lives on a dedicated line via `ChainDoneSlot` so adjacent
/// workers' release-stores
///   do not contend for the same cache line.
///
/// The state itself lives on the producer's stack across a `parallelChain`
/// call. The trailing per-worker `done` slots are NOT owned by `ChainState`:
/// the pool pre-allocates a contiguous `ChainDoneSlot` block once at
/// construction time (sized `participants()`), and every chain call borrows
/// that block via `doneSlots`. The producer zero-resets each slot at entry to
/// honour the chain's "fresh epoch per call" contract; this avoids `operator
/// new` / `operator delete` on the dispatch hot path, which the spec's <= 2 us
/// target rules out.
///
/// The Global rendezvous is fully decentralized: every slot (including slot 0)
/// stamps `done[slot] = s + 1` (release) after running stage `s`, and every
/// slot's pre-stage barrier is an independent scan over `done[w] >= s + 1` for
/// every other slot `w`. No participant acts as a serial gate; the wait loops
/// execute in parallel across all workers, mirroring the `runPlex` per-slot
/// done-epoch pattern documented in `plex_state.h`. The cancellation handshake
/// is encoded by stamping `done = nStages` on the cancelled slot's line, which
/// satisfies any active stage's wait condition unconditionally and lets the
/// spin loop drop a per-iteration `chainCancelled` poll.
///
/// For `BarrierKind::ProducerSerial` the producer runs the serial body alone
/// while non-producer workers spin on slot 0's `done`.
///
/// Padding-suppression note: the layout keeps every contended atomic on its own
/// `kCacheLine`-sized line, so the analyser's "excessive padding" warning is
/// the design trade-off we want -- false-sharing avoidance over byte-tight
/// packing.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct ChainState {
  /// Number of participants (producer + background workers) collaborating in
  /// the chain.
  std::uint32_t participants = 0;

  /// Total number of stages the chain was constructed with; used by workers as
  /// a phase ceiling.
  std::size_t nStages = 0;

  /// Row-range upper bound passed in by the caller; used for slot partitioning.
  std::size_t n = 0;

  /// Cancellation flag flipped by the producer's cancellation observer.
  ///
  /// Worker bodies check this between stages and exit cleanly when set;
  /// cancelled slots stamp `done = nStages` so peers waiting on `done >=
  /// target` for any active stage proceed without needing to poll this flag
  /// inside the spin loop. The flag is release-written by the slot that
  /// observed the cancellation, and acquire-read by other slots at stage
  /// boundaries (not in the spin loop).
  alignas(kCacheLine) std::atomic<std::uint32_t> chainCancelled{0};

  /// First-exception capture slot shared across all participants.
  ///
  /// Workers `compare_exchange` this from null to a heap-allocated
  /// `std::exception_ptr` to record the first failure deterministically;
  /// subsequent throws drop. The producer reads the slot after joining and
  /// rethrows if non-null. Allocation only happens on the cold throw path.
  alignas(kCacheLine) std::atomic<std::exception_ptr *> firstException{nullptr};

  /// Borrowed pointer to the pool's pre-allocated per-worker completion slots.
  ///
  /// The pool owns the storage; the chain call reserves a fresh interval
  /// `[epochBase, epochBase + nStages]` in the pool's monotonically-advancing
  /// epoch counter so successive calls observe disjoint targets without
  /// zero-resetting the slots. Sized `participants` valid elements; reading
  /// past that index is undefined.
  ChainDoneSlot *doneSlots = nullptr;

  /// Borrowed pointer to per-stage dynamic block counters.
  ///
  /// Present only when dynamic-chain mode is active. The producer owns the
  /// stack-resident counter array and zeroes every stage before dispatch, so no
  /// in-flight participant resets counters between stages.
  ChainDynamicStageCounter *dynamicStageCounters = nullptr;

  /// Dynamic-chain chunk size over `[0, n)`.
  std::size_t dynamicChunk = 0;

  /// Dynamic-chain number of chunks over `[0, n)`.
  std::size_t dynamicBlockCount = 0;

  /// Per-call base of the pool's monotonic done-epoch counter.
  ///
  /// Stamps are absolute: `done = epochBase + I + 1` where `I` is the
  /// just-finished stage index. Waits compare against `epochBase + target`. The
  /// producer reserves the interval under the dispatch gate before publishing,
  /// so prior-dispatch values cannot satisfy a current wait. Cancellation
  /// stamps `epochBase + nStages` so peers waiting on any active stage advance.
  std::uint64_t epochBase = 0;

  /// Subscript a slot by index.
  ///
  /// The slot itself owns mutable atomics; the accessor is `const` because
  /// reading from the borrowed pointer does not modify the owning state, and
  /// the returned slot's atomics carry their own internal mutability.
  ///
  /// idx Slot index in `[0, participants)`.
  /// Reference to the slot.
  [[nodiscard]] ChainDoneSlot &doneSlot(std::size_t idx) const noexcept {
    return doneSlots[idx];
  }

  /// Subscript a dynamic-stage counter by stage index.
  ///
  /// Valid only when dynamic-chain mode is active and `dynamicStageCounters`
  /// points at the producer-owned counter array.
  ///
  /// stage Stage index in `[0, nStages)`.
  /// Reference to the stage's dynamic claim counter.
  [[nodiscard]] ChainDynamicStageCounter &
  dynamicStageCounter(std::size_t stage) const noexcept {
    return dynamicStageCounters[stage];
  }

  /// Compute a slot's contiguous row range over `[0, n)` using static
  /// partitioning.
  ///
  /// The partition is `lo = (n * slot) / participants`, `hi = (n * (slot + 1))
  /// / participants`. Per-stage chunk identity is preserved across stages: a
  /// stage's chunk `c` is the slice
  /// `[lo, hi)` produced for `slot = c`.
  ///
  /// slot Worker slot index in `[0, participants)`.
  /// `(lo, hi)` pair denoting the slot's contiguous range over `[0, n)`.
  [[nodiscard]] std::pair<std::size_t, std::size_t>
  slotRange(std::uint32_t slot) const noexcept {
    const auto lo = static_cast<std::size_t>(mulDiv64(n, slot, participants));
    const auto hi =
        static_cast<std::size_t>(mulDiv64(n, slot + 1U, participants));
    return {lo, hi};
  }

  /// Compute a dynamic-chain block range over `[0, n)`.
  ///
  /// Unlike `slotRange`, block identity is not tied to participant identity.
  /// Faster slots claim additional block ids from that stage's dynamic counter.
  ///
  /// block Dynamic block id in `[0, dynamicBlockCount)`.
  /// `(lo, hi)` pair denoting the block's range over `[0, n)`.
  [[nodiscard]] std::pair<std::size_t, std::size_t>
  dynamicBlockRange(std::size_t block) const noexcept {
    const std::size_t lo = block * dynamicChunk;
    const std::size_t hi = (dynamicChunk < n - lo) ? lo + dynamicChunk : n;
    return {lo, hi};
  }
};

} // namespace citor::detail

// ===== citor/detail/chase_lev_deque.h =====



namespace citor::detail {

// Lock-free single-owner / multi-stealer work-stealing deque.
//
// Implements the Le / Pop / Cohen / Nardelli formulation (PPoPP 2013) of the
// Chase / Lev deque with explicit C++20 memory orders. The deque has a single
// owner thread that performs `push` and `pop` on the bottom; any number of
// stealer threads concurrently call `steal` at the top. The owner never
// resizes during a steal: growth happens only on the owner's `push` hot path
// when the underlying ring buffer is full.
//
// Memory-order summary (verbatim from Le 2013 figure 1):
//   - push: relaxed-load `bottom`, relaxed-load `top`, optional grow,
//     relaxed array store, release-store new `bottom`.
//   - pop: relaxed-store `bottom = bottom - 1`, seq_cst fence, relaxed-load
//     `top`, relaxed array load. On size 0 (empty) the owner restores
//     `bottom`. On size 1 the owner CAS-tightens `top` (seq_cst success,
//     relaxed failure) to settle the steal race.
//   - steal: acquire-load `top`, seq_cst fence, acquire-load `bottom`. If
//     empty, abandon. If non-empty, acquire-load array slot, then CAS `top`
//     (seq_cst success, relaxed failure). The `top` CAS is what serializes
//     against a concurrent `pop` on the last item.
//
// The internal buffer is a power-of-two `Array` indexed modulo capacity.
// Growth allocates a new `Array`, copies the live elements (those between
// `top` and `bottom`), and links the old array via a freelist that is only
// reaped at deque destruction time. A stealer holding a pointer into the old
// array remains valid for the rest of its current steal attempt because the
// freelist never frees mid-flight (Le 2013 section 3 footnote 2).
//
// Termination: at deque destruction, every owned `Array` (including any
// superseded ones pinned by an outstanding stealer) is freed via
// `reapAllArrays`. The owner is responsible for draining all in-flight steals
// before destroying the deque; the synchronous primitive that owns the deque
// joins on every worker before the deque goes out of scope.
template <class T>
class ChaseLevDeque {
public:
  static_assert(std::is_trivially_copyable_v<T>,
                "ChaseLevDeque payload must be trivially-copyable");
  static_assert(std::is_trivially_destructible_v<T>,
                "ChaseLevDeque payload must be trivially-destructible");

  /// Initial capacity for a freshly-constructed deque; a power of two.
  static constexpr std::size_t kInitialCapacity = 64;

  /// Construct an empty deque sized to |initialCapacity|, rounded up to a
  /// power of two and clamped to at least `kInitialCapacity`.
  explicit ChaseLevDeque(std::size_t initialCapacity = kInitialCapacity) {
    const std::size_t cap = std::max(
        roundUpPow2(initialCapacity > 0 ? initialCapacity : kInitialCapacity),
        kInitialCapacity);
    auto *arr = allocateArray(cap);
    m_array.store(arr, std::memory_order_relaxed);
  }

  // Deques are pinned in the worker-state block; copying would require
  // duplicating the atomics.
  ChaseLevDeque(const ChaseLevDeque &) = delete;
  ChaseLevDeque &operator=(const ChaseLevDeque &) = delete;
  ChaseLevDeque(ChaseLevDeque &&) = delete;
  ChaseLevDeque &operator=(ChaseLevDeque &&) = delete;

  // Free every `Array` ever allocated by this deque: the active array plus
  // every retired array still pinned in the freelist. The owner has joined
  // with every stealer by construction, so no array can be in use here.
  ~ChaseLevDeque() {
    Array *active = m_array.load(std::memory_order_relaxed);
    deleteArray(active);
    Array *retired = m_freelist.load(std::memory_order_relaxed);
    while (retired != nullptr) {
      Array *next = retired->next;
      deleteArray(retired);
      retired = next;
    }
  }

  /// Owner-only: pre-grow the underlying ring buffer so the next |needed|
  /// pushes do not trigger an allocation. No-op when capacity is already
  /// sufficient. Bulk-push call sites (e.g. `forkJoinAll` with many roots)
  /// call this once to fold N growth allocations into at most one.
  void reserveOwner(std::size_t needed) {
    Array *arr = m_array.load(std::memory_order_relaxed);
    if (needed + 1U <= arr->capacity) {
      return;
    }
    const std::int64_t b = m_bottom.load(std::memory_order_relaxed);
    const std::int64_t t = m_top.load(std::memory_order_acquire);
    std::size_t newCap = arr->capacity;
    while (newCap < needed + 1U) {
      newCap *= 2;
    }
    Array *newArr = allocateArray(newCap);
    for (std::int64_t i = t; i < b; ++i) {
      newArr->store(i, arr->load(i));
    }
    m_array.store(newArr, std::memory_order_release);
    Array *head = m_freelist.load(std::memory_order_relaxed);
    do {
      arr->next = head;
    } while (!m_freelist.compare_exchange_weak(
        head, arr, std::memory_order_release, std::memory_order_relaxed));
  }

  /// Owner-only: push a value onto the bottom of the deque.
  ///
  /// Resizes the underlying ring buffer to twice its capacity when the buffer
  /// is full. The grow path allocates a fresh `Array`, copies live elements,
  /// and chains the old array onto a freelist so concurrent stealers'
  /// acquire-loaded array pointers remain valid for the rest of their attempt.
  void push(T value) {
    const std::int64_t b = m_bottom.load(std::memory_order_relaxed);
    const std::int64_t t = m_top.load(std::memory_order_acquire);
    Array *arr = m_array.load(std::memory_order_relaxed);
    const std::int64_t size = b - t;
    if (size >= static_cast<std::int64_t>(arr->capacity) - 1) {
      arr = grow(arr, b, t);
    }
    arr->store(b, value);
    std::atomic_thread_fence(std::memory_order_release);
    m_bottom.store(b + 1, std::memory_order_relaxed);
  }

  /// Owner-only: pop a value from the bottom of the deque.
  ///
  /// Returns `std::nullopt` when the deque is empty. The last-item race with a
  /// concurrent `steal` is settled via a seq_cst CAS on `top`: only one of
  /// the two participants succeeds.
  std::optional<T> pop() noexcept {
    const Array *arr = m_array.load(std::memory_order_relaxed);
    const std::int64_t b = m_bottom.load(std::memory_order_relaxed) - 1;
    m_bottom.store(b, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    std::int64_t t = m_top.load(std::memory_order_relaxed);
    if (t > b) {
      // Empty.
      m_bottom.store(b + 1, std::memory_order_relaxed);
      return std::nullopt;
    }
    const T value = arr->load(b);
    if (t != b) {
      // More than one element; the load is uncontended.
      return value;
    }
    // Last item: race against any in-flight stealer.
    const bool won = m_top.compare_exchange_strong(
        t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed);
    m_bottom.store(b + 1, std::memory_order_relaxed);
    if (!won) {
      return std::nullopt;
    }
    return value;
  }

  /// Stealer: try to steal a value from the top of the deque.
  ///
  /// Returns `std::nullopt` on contention or empty. The caller retries (or
  /// moves to another victim) when steal returns empty without distinguishing
  /// the two cases; the canonical Chase-Lev formulation collapses them. The
  /// `top` CAS uses seq_cst on success, relaxed on failure, matching Le 2013
  /// figure 1.
  std::optional<T> steal() noexcept {
    std::int64_t t = m_top.load(std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const std::int64_t b = m_bottom.load(std::memory_order_acquire);
    if (t >= b) {
      return std::nullopt;
    }
    const Array *arr = m_array.load(std::memory_order_acquire);
    const T value = arr->load(t);
    if (!m_top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                       std::memory_order_relaxed)) {
      return std::nullopt;
    }
    return value;
  }

  /// Stealer-friendly empty-probe used on the worker poll fast path.
  ///
  /// Reads `top` (acquire) and `bottom` (acquire) and returns true when `top
  /// >= bottom`. The relaxed orders the canonical Chase-Lev empty-check would
  /// use are insufficient here because the result is consumed before the
  /// steal CAS; the acquire-loads are paired with the release-store on the
  /// owner's `push` so the worker observes any payload published before the
  /// steal probe.
  [[nodiscard]] bool empty() const noexcept {
    const std::int64_t t = m_top.load(std::memory_order_acquire);
    const std::int64_t b = m_bottom.load(std::memory_order_acquire);
    return t >= b;
  }

  /// Owner-side observation of the deque's logical size. Suitable for debug
  /// assertions; not for hot-path scheduling, since the value can be
  /// invalidated by an in-flight steal between the load and the consumer.
  [[nodiscard]] std::size_t size() const noexcept {
    const std::int64_t t = m_top.load(std::memory_order_acquire);
    const std::int64_t b = m_bottom.load(std::memory_order_acquire);
    return (b > t) ? static_cast<std::size_t>(b - t) : std::size_t{0};
  }

  /// Owner-side query: current capacity of the underlying ring buffer; always
  /// a power of two.
  [[nodiscard]] std::size_t capacity() const noexcept {
    return m_array.load(std::memory_order_relaxed)->capacity;
  }

private:
  /// Ring-buffer backing storage. Indexed modulo `capacity`. `next` chains
  /// retired buffers.
  struct Array {
    /// Ring-buffer capacity in elements; a power of two.
    std::size_t capacity = 0;
    /// Power-of-two mask, equal to `capacity - 1`.
    std::size_t mask = 0;
    /// Next retired buffer in the freelist; `nullptr` if not retired.
    Array *next = nullptr;
    /// Trailing storage of `capacity` slots; flexible array layout via
    /// `operator new`. The declared length of 1 is a placeholder; the actual
    /// element count is `capacity` and is allocated by `allocateArray` via a
    /// single oversized `operator new` call.
    /// NOLINTNEXTLINE(modernize-avoid-c-arrays)
    std::atomic<T> slots[1];

    /// Store |v| at logical index |i| (mod capacity).
    ///
    /// Release ordering pairs with the matching acquire load on the steal path
    /// so a stealer that acquire-loads `bottom` and then loads this slot
    /// observes everything the owner wrote before the corresponding push,
    /// including the descriptor fields the stolen payload points at. The Le
    /// 2013 push protocol's release fence + relaxed `bottom` store would be
    /// sufficient on its own, but the explicit per-slot release lets
    /// ThreadSanitizer model the cross-thread happens-before edge directly
    /// without relying on the fence-relaxed-store equivalence.
    void store(std::int64_t i, T v) noexcept {
      slots[static_cast<std::size_t>(i) & mask].store(
          v, std::memory_order_release);
    }

    /// Load the value at logical index |i| (mod capacity).
    ///
    /// Acquire ordering pairs with the per-slot release store on push so a
    /// stealer (or the owner after the seq_cst fence in pop) observes the
    /// descriptor the slot points to. On x86-64 the acquire load is free; on
    /// weaker memory models it is the canonical pairing for the slot.
    [[nodiscard]] T load(std::int64_t i) const noexcept {
      return slots[static_cast<std::size_t>(i) & mask].load(
          std::memory_order_acquire);
    }
  };

  /// Allocate a fresh `Array` with the requested power-of-two |cap|. Payload
  /// slots are default-initialized to `T{}`.
  static Array *allocateArray(std::size_t cap) {
    const std::size_t headerBytes = offsetof(Array, slots);
    const std::size_t totalBytes = headerBytes + (sizeof(std::atomic<T>) * cap);
    void *raw = ::operator new(totalBytes, std::align_val_t{kCacheLine});
    auto *arr = static_cast<Array *>(raw);
    arr->capacity = cap;
    arr->mask = cap - 1;
    arr->next = nullptr;
    for (std::size_t i = 0; i < cap; ++i) {
      ::new (static_cast<void *>(arr->slots + i)) std::atomic<T>(T{});
    }
    return arr;
  }

  /// Free an array previously returned by `allocateArray`. Tolerates nullptr.
  static void deleteArray(Array *arr) noexcept {
    if (arr == nullptr) {
      return;
    }
    for (std::size_t i = 0; i < arr->capacity; ++i) {
      arr->slots[i].~atomic();
    }
    ::operator delete(static_cast<void *>(arr), std::align_val_t{kCacheLine});
  }

  /// Owner-side: double the ring buffer's capacity in place.
  ///
  /// Allocates a new `Array` with `2 * old.capacity` slots, copies every live
  /// element, retires the old array onto the freelist (so any in-flight
  /// stealer's acquire-loaded pointer remains valid for the rest of its
  /// attempt), and publishes the new array via release-store on `m_array`.
  Array *grow(Array *oldArr, std::int64_t b, std::int64_t t) {
    const std::size_t newCap = oldArr->capacity * 2;
    Array *newArr = allocateArray(newCap);
    for (std::int64_t i = t; i < b; ++i) {
      newArr->store(i, oldArr->load(i));
    }
    m_array.store(newArr, std::memory_order_release);
    // Retire `oldArr` onto the freelist; reaped only at deque destruction.
    // A stealer that already loaded `oldArr` via acquire load on `m_array`
    // keeps a valid pointer for the remainder of its current steal attempt.
    Array *head = m_freelist.load(std::memory_order_relaxed);
    do {
      oldArr->next = head;
    } while (!m_freelist.compare_exchange_weak(
        head, oldArr, std::memory_order_release, std::memory_order_relaxed));
    return newArr;
  }

  /// Round |v| up to the next power of two. Input must be at least 1.
  static constexpr std::size_t roundUpPow2(std::size_t v) noexcept {
    if (v <= 1) {
      return 1;
    }
    --v;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    if constexpr (sizeof(std::size_t) > 4) {
      v |= v >> 32;
    }
    return v + 1;
  }

  /// Owner-incremented push index. Release-stored after a successful push.
  alignas(kCacheLine) std::atomic<std::int64_t> m_bottom{0};

  /// Stealer-incremented pop index. CAS-updated by both the owner's `pop`
  /// last-item branch and every successful `steal`.
  alignas(kCacheLine) std::atomic<std::int64_t> m_top{0};

  /// Active backing array. Replaced via release-store by `grow`.
  alignas(kCacheLine) std::atomic<Array *> m_array{nullptr};

  /// Singly-linked freelist of retired arrays; reaped at destruction time.
  alignas(kCacheLine) std::atomic<Array *> m_freelist{nullptr};
};

} // namespace citor::detail

// ===== citor/detail/coherence_probe.h =====


#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif


namespace citor::detail {

/// One-time pool-init coherence probe.
///
/// Builds an NxN cache-line ping-pong latency matrix between every pair of
/// CPUs in the pool's affinity mask, then clusters CPUs into coherence
/// groups so primitives that benefit from topology-aware partitioning
/// (parallelScan, parallelReduce, fork/join victim selection) can size
/// their cross-cluster work share from observed cost ratios instead of
/// hardware-specific constants.
///
/// Methodology summary (from research):
/// - Ping-pong probe: two threads, one shared `std::atomic<uint64_t>`,
///   each side flips even/odd parity; median of N round-trips is the
///   pair's latency in ns. (nviennot/core-to-core-latency, ChipsandCheese
///   Microbenchmarks.)
/// - Disjoint-pair scheduling: for N CPUs, run N-1 rounds where each
///   round's pairs are a perfect matching of K_N. Compresses wall time
///   from O(N^2 * probe-ms) to O(N * probe-ms).
/// - Clustering: log-latency single-linkage agglomerative + Otsu's
///   threshold on the off-diagonal histogram. Parameter-free,
///   scale-invariant, falls back to the sysfs/L3 grouping prior when
///   the histogram is unimodal (single-CCX consumer chip).
/// - Cache nothing across pool ctors in this header; the pool may
///   memoise the result if it wants.

struct LatencyMatrix {
  /// CPU ids (in matrix index order). `cpus.size() == matrix.size()`.
  std::vector<std::uint32_t> cpus;
  /// Symmetric pairwise latency in nanoseconds. `matrix[i][j]` is the
  /// median round-trip latency between `cpus[i]` and `cpus[j]`. The
  /// diagonal is zero (defined; not measured).
  std::vector<std::vector<double>> matrix;
  /// Valid if every off-diagonal cell was successfully measured.
  bool valid = false;
};

/// Coherence-cluster assignment for the CPUs in a `LatencyMatrix`.
struct ClusterResult {
  /// Cluster identifier for each entry in the parent `LatencyMatrix::cpus`.
  /// Values are `0..numClusters-1`. Empty when clustering was not
  /// performed (single-CPU pool, probe failed, etc.).
  std::vector<std::uint32_t> clusterIdOfCpuIndex;
  /// Number of distinct clusters discovered.
  std::uint32_t numClusters = 0;
  /// Median pairwise latency between cluster pairs. `clusterDistanceNs[i][j]`
  /// is the median over all `(cpu_a, cpu_b)` with `cluster(a)==i,
  /// cluster(b)==j`. Diagonal entries hold the median intra-cluster
  /// pairwise latency.
  std::vector<std::vector<double>> clusterDistanceNs;
};

/// Combined output of a one-time coherence probe: the raw pairwise latency
/// matrix, the derived cluster assignment, and a single ratio scalar that
/// callers can use as a topology bias without inspecting the full matrix.
struct CoherenceProbe {
  /// True when the probe completed and `matrix` plus `clusters` are
  /// populated. False on probe failure or single-CPU pools.
  bool valid = false;
  /// Pairwise round-trip latency matrix between every pair of CPUs in the
  /// pool's affinity mask.
  LatencyMatrix matrix;
  /// Cluster assignment derived from `matrix` via Otsu's threshold on the
  /// off-diagonal log-latency histogram.
  ClusterResult clusters;
  /// Worst-case (maximum) cross-cluster / intra-cluster median latency
  /// ratio. `1.0` when there is only one cluster. This is the convenience
  /// scalar used by primitives that want a single bias factor without
  /// inspecting the full matrix.
  double maxCrossOverIntraRatio = 1.0;
};

#ifdef __linux__
/// Pins the calling thread to `|cpu|` for the duration of one probe round.
/// Failures from `pthread_setaffinity_np` are ignored; the probe degrades
/// to whatever scheduling the kernel chooses.
inline void coherenceProbePin(int cpu) noexcept {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<std::size_t>(cpu), &set);
  (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}
#elif defined(_WIN32)
/// Windows peer: pin `|cpu|` via `SetThreadAffinityMask`. Failures
/// degrade the probe to whatever scheduling Windows chooses. Single
/// processor group only (64 logical CPUs).
inline void coherenceProbePin(int cpu) noexcept {
  const DWORD_PTR mask = static_cast<DWORD_PTR>(1)
                         << (static_cast<std::uint32_t>(cpu) & 63U);
  (void)::SetThreadAffinityMask(::GetCurrentThread(), mask);
}
#else
/// No-op fallback on platforms with no first-class affinity API; the
/// probe runs unpinned.
inline void coherenceProbePin(int /*cpu*/) noexcept {}
#endif

/// Single-pair atomic-CAS ping-pong. Pins one helper thread to `cpuB`,
/// pins the calling thread to `cpuA`, then runs `roundTrips` round-trips
/// of an even/odd parity flip on a single shared cache line. Returns the
/// MEAN round-trip time in nanoseconds; mean is fine here because the
/// loop body is an atomic-RMW that dominates; outliers from interrupts
/// average out across hundreds of round-trips.
///
/// Restores caller's pre-probe affinity mask on exit.
inline double pingPongLatencyNs(int cpuA, int cpuB,
                                std::uint32_t roundTrips = 1024U) noexcept {
  alignas(kCacheLine) std::atomic<std::uint64_t> counter{0};
  alignas(kCacheLine) std::atomic<int> ready{0};
  alignas(kCacheLine) std::atomic<int> stop{0};

#ifdef __linux__
  cpu_set_t savedSet;
  CPU_ZERO(&savedSet);
  const bool savedOk =
      pthread_getaffinity_np(pthread_self(), sizeof(savedSet), &savedSet) == 0;
#elif defined(_WIN32)
  // Windows has no read-only affinity accessor; round-trip
  // `SetThreadAffinityMask(thread, ~0)` to capture the caller's mask.
  // The brief all-CPUs window is closed by the next pin call.
  const DWORD_PTR savedSet = ::SetThreadAffinityMask(
      ::GetCurrentThread(), static_cast<DWORD_PTR>(~DWORD_PTR{0}));
  const bool savedOk = (savedSet != 0U);
#endif

  std::thread helper([&, cpuB] {
    coherenceProbePin(cpuB);
    ready.store(1, std::memory_order_release);
    while (stop.load(std::memory_order_acquire) == 0) {
      const std::uint64_t observed = counter.load(std::memory_order_acquire);
      if ((observed & 1ULL) == 1ULL) {
        counter.store(observed + 1ULL, std::memory_order_release);
      } else {
        cpuRelax();
      }
    }
  });

  coherenceProbePin(cpuA);
  while (ready.load(std::memory_order_acquire) == 0) {
    cpuRelax();
  }

  // Warmup: 64 round-trips to settle the shared cache line and let the
  // helper's first scheduler dispatch retire.
  for (std::uint32_t i = 0; i < 64U; ++i) {
    const std::uint64_t observed = counter.load(std::memory_order_acquire);
    counter.store(observed + 1ULL, std::memory_order_release);
    while ((counter.load(std::memory_order_acquire) & 1ULL) == 1ULL) {
      cpuRelax();
    }
  }

  const auto t0 = std::chrono::steady_clock::now();
  for (std::uint32_t i = 0; i < roundTrips; ++i) {
    const std::uint64_t observed = counter.load(std::memory_order_acquire);
    counter.store(observed + 1ULL, std::memory_order_release);
    while ((counter.load(std::memory_order_acquire) & 1ULL) == 1ULL) {
      cpuRelax();
    }
  }
  const auto t1 = std::chrono::steady_clock::now();

  stop.store(1, std::memory_order_release);
  // Helper observes `stop` on its next iter (it is currently spinning on
  // an even counter waiting for us to flip it). To unblock cleanly we
  // flip the counter to odd one more time, helper advances it to even,
  // observes stop, and exits.
  const std::uint64_t observed = counter.load(std::memory_order_acquire);
  counter.store(observed + 1ULL, std::memory_order_release);
  helper.join();

#ifdef __linux__
  if (savedOk) {
    (void)pthread_setaffinity_np(pthread_self(), sizeof(savedSet), &savedSet);
  }
#elif defined(_WIN32)
  if (savedOk) {
    (void)::SetThreadAffinityMask(::GetCurrentThread(), savedSet);
  }
#endif

  const double totalNs =
      std::chrono::duration<double, std::nano>(t1 - t0).count();
  return totalNs / static_cast<double>(roundTrips);
}

/// Build the round-robin disjoint-pair schedule for `N` participants.
/// Returns `N-1` rounds when `N` is even, each round containing `N/2`
/// pairs. For odd `N` adds a "bye" slot internally and returns `N`
/// rounds with one bye per round; bye pairs are filtered out.
///
/// Each pair (i, j) appears in exactly one round, so the union of all
/// rounds' pairs is the complete graph K_N's edge set.
inline std::vector<std::vector<std::pair<std::uint32_t, std::uint32_t>>>
roundRobinPairs(std::uint32_t n) {
  std::vector<std::vector<std::pair<std::uint32_t, std::uint32_t>>> rounds;
  if (n < 2U) {
    return rounds;
  }
  const bool addBye = (n % 2U) != 0U;
  const std::uint32_t m = addBye ? (n + 1U) : n;
  std::vector<std::uint32_t> arr(m);
  std::iota(arr.begin(), arr.end(), 0U);
  rounds.reserve(m - 1U);
  for (std::uint32_t r = 0; r < m - 1U; ++r) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> pairs;
    pairs.reserve(m / 2U);
    for (std::uint32_t i = 0; i < m / 2U; ++i) {
      const std::uint32_t a = arr[i];
      const std::uint32_t b = arr[m - 1U - i];
      if (addBye && (a == n || b == n)) {
        continue; // bye for this round's affected participant
      }
      pairs.emplace_back(a, b);
    }
    rounds.push_back(std::move(pairs));
    // Rotate: arr[0] is the pivot, arr[1..m-1] rotate by one (last to
    // index 1, others shift right).
    const std::uint32_t last = arr[m - 1U];
    for (std::uint32_t i = m - 1U; i > 1U; --i) {
      arr[i] = arr[i - 1U];
    }
    arr[1U] = last;
  }
  return rounds;
}

/// Probe the full pairwise latency matrix for `cpus`. Uses disjoint-pair
/// scheduling: per round, every active CPU is in at most one pair, so we
/// can spawn one std::thread per pair (each pair has 2 threads) and
/// collect all measurements in parallel. Total wall time is approximately
/// `(N - 1) * single-pair-probe-time + thread-spawn-overhead`.
///
/// Caller's affinity is saved on entry and restored on exit.
inline LatencyMatrix
probeLatencyMatrix(const std::vector<std::uint32_t> &cpus,
                   std::uint32_t roundTrips = 1024U) noexcept {
  LatencyMatrix out;
  if (cpus.size() < 2U) {
    return out;
  }
  const auto n = static_cast<std::uint32_t>(cpus.size());
  out.cpus = cpus;
  out.matrix.assign(n, std::vector<double>(n, 0.0));

#ifdef __linux__
  cpu_set_t savedSet;
  CPU_ZERO(&savedSet);
  const bool savedOk =
      pthread_getaffinity_np(pthread_self(), sizeof(savedSet), &savedSet) == 0;
#elif defined(_WIN32)
  // See `pingPongLatencyNs` for the all-CPUs round-trip used to capture
  // the caller's affinity on Windows.
  const DWORD_PTR savedSet = ::SetThreadAffinityMask(
      ::GetCurrentThread(), static_cast<DWORD_PTR>(~DWORD_PTR{0}));
  const bool savedOk = (savedSet != 0U);
#endif

  const auto schedule = roundRobinPairs(n);
  for (const auto &round : schedule) {
    // Spawn one thread per pair. Each pair is (cpus[a], cpus[b]). A and
    // B in different pairs are non-overlapping CPUs (matching property),
    // so per-pair threads do not contend with each other for the
    // measurement window.
    std::vector<std::thread> pairThreads;
    pairThreads.reserve(round.size());
    std::vector<double> roundLatencies(round.size(), 0.0);
    for (std::size_t pi = 0; pi < round.size(); ++pi) {
      const auto [a, b] = round[pi];
      const int cpuA = static_cast<int>(cpus[a]);
      const int cpuB = static_cast<int>(cpus[b]);
      pairThreads.emplace_back([cpuA, cpuB, &roundLatencies, pi, roundTrips] {
        roundLatencies[pi] = pingPongLatencyNs(cpuA, cpuB, roundTrips);
      });
    }
    for (auto &t : pairThreads) {
      t.join();
    }
    for (std::size_t pi = 0; pi < round.size(); ++pi) {
      const auto [a, b] = round[pi];
      out.matrix[a][b] = roundLatencies[pi];
      out.matrix[b][a] = roundLatencies[pi];
    }
  }

#ifdef __linux__
  if (savedOk) {
    (void)pthread_setaffinity_np(pthread_self(), sizeof(savedSet), &savedSet);
  }
#elif defined(_WIN32)
  if (savedOk) {
    (void)::SetThreadAffinityMask(::GetCurrentThread(), savedSet);
  }
#endif

  out.valid = true;
  return out;
}

/// Otsu's method for choosing a bimodal threshold on the off-diagonal
/// log-latency histogram. Returns the threshold in log-ns, plus a
/// `bimodality` score (between-class variance / total variance) in
/// `[0, 1]`. Scores below ~0.4 indicate the histogram is essentially
/// unimodal, in which case the caller should fall back to the sysfs
/// prior rather than trust the threshold.
/// Output of `otsuThresholdLog`.
struct OtsuResult {
  /// Threshold in log-ns that maximises between-class variance on the
  /// off-diagonal log-latency histogram.
  double threshold = 0.0;
  /// Normalised between-class variance in `[0, 1]`. Scores below ~0.4
  /// indicate a unimodal histogram and the threshold should be ignored.
  double bimodality = 0.0;
};

/// Computes Otsu's threshold on the log-space histogram of `|values|` and
/// returns the threshold plus a bimodality score the caller uses to decide
/// whether the bipartition is trustworthy.
inline OtsuResult otsuThresholdLog(const std::vector<double> &values) noexcept {
  OtsuResult r;
  if (values.size() < 2U) {
    return r;
  }
  // Histogram in log-space, 64 bins.
  constexpr std::size_t kBins = 64U;
  double minV = std::numeric_limits<double>::infinity();
  double maxV = -std::numeric_limits<double>::infinity();
  for (const double v : values) {
    if (v <= 0.0) {
      continue;
    }
    const double lv = std::log(v);
    minV = std::min(minV, lv);
    maxV = std::max(maxV, lv);
  }
  if (!(maxV > minV)) {
    return r;
  }
  std::vector<std::uint32_t> hist(kBins, 0U);
  for (const double v : values) {
    if (v <= 0.0) {
      continue;
    }
    const double lv = std::log(v);
    auto bin = static_cast<std::size_t>((lv - minV) / (maxV - minV) *
                                        static_cast<double>(kBins - 1U));
    if (bin >= kBins) {
      bin = kBins - 1U;
    }
    hist[bin] += 1U;
  }
  std::uint64_t total = 0U;
  double sumAll = 0.0;
  for (std::size_t b = 0; b < kBins; ++b) {
    total += hist[b];
    sumAll += static_cast<double>(hist[b]) * static_cast<double>(b);
  }
  if (total == 0U) {
    return r;
  }
  std::uint64_t cumCount = 0U;
  double cumSum = 0.0;
  double bestVar = -1.0;
  std::size_t bestBin = 0U;
  for (std::size_t b = 0; b < kBins; ++b) {
    cumCount += hist[b];
    cumSum += static_cast<double>(hist[b]) * static_cast<double>(b);
    if (cumCount == 0U || cumCount == total) {
      continue;
    }
    const double w0 =
        static_cast<double>(cumCount) / static_cast<double>(total);
    const double w1 = 1.0 - w0;
    const double m0 = cumSum / static_cast<double>(cumCount);
    const double m1 = (sumAll - cumSum) / static_cast<double>(total - cumCount);
    const double bcv = w0 * w1 * (m0 - m1) * (m0 - m1);
    if (bcv > bestVar) {
      bestVar = bcv;
      bestBin = b;
    }
  }
  // Total variance.
  const double mean = sumAll / static_cast<double>(total);
  double tv = 0.0;
  for (std::size_t b = 0; b < kBins; ++b) {
    const double diff = static_cast<double>(b) - mean;
    tv += static_cast<double>(hist[b]) * diff * diff;
  }
  tv /= static_cast<double>(total);
  r.threshold = minV + (((static_cast<double>(bestBin) + 0.5) /
                         static_cast<double>(kBins - 1U)) *
                        (maxV - minV));
  r.bimodality = (tv > 0.0) ? (bestVar / tv) : 0.0;
  return r;
}

/// Cluster the matrix CPUs by latency. Builds a graph where edge `(i, j)`
/// exists if `log(matrix[i][j]) <= threshold` (Otsu cut on the
/// off-diagonal log-latency histogram), then finds connected components.
/// Each component is one cluster.
///
/// Falls back to the `sysfsPrior` (per-CCD/L3 grouping from
/// `topology.h`) when the histogram is essentially unimodal -- a
/// single-CCX consumer chip will produce a unimodal histogram with no
/// useful threshold; the sysfs grouping is the right answer there.
///
/// `sysfsPrior[k]` is a list of CPU ids that share an L3 cache. The
/// function maps prior CPU ids to matrix indices and uses them as the
/// fallback partition.
inline ClusterResult clusterByLatency(
    const LatencyMatrix &mat,
    const std::vector<std::vector<std::uint32_t>> &sysfsPrior) noexcept {
  ClusterResult out;
  const auto n = static_cast<std::uint32_t>(mat.cpus.size());
  if (!mat.valid || n < 2U) {
    if (n > 0U) {
      out.clusterIdOfCpuIndex.assign(n, 0U);
      out.numClusters = 1U;
      out.clusterDistanceNs.assign(1U, std::vector<double>(1U, 0.0));
    }
    return out;
  }

  // Off-diagonal latencies for Otsu.
  std::vector<double> offDiag;
  offDiag.reserve(static_cast<std::size_t>(n) * (n - 1U) / 2U);
  for (std::uint32_t i = 0; i < n; ++i) {
    for (std::uint32_t j = i + 1U; j < n; ++j) {
      offDiag.push_back(mat.matrix[i][j]);
    }
  }
  const OtsuResult ot = otsuThresholdLog(offDiag);

  // Use Otsu cut only if the histogram is sufficiently bimodal. The
  // threshold is the BCV/TV ratio (between-class variance over total
  // variance, computed in log-space). For a unimodal Gaussian sample,
  // Otsu picks the median and yields BCV/TV ~0.32; for a clearly bimodal
  // sample with two well-separated peaks, BCV/TV approaches 1.0. The
  // 0.55 cutoff rejects the Gaussian-noise case (single CCD on a
  // multi-core probe) while accepting genuine multi-cluster splits;
  // 0.555 (5/9) is the textbook bimodality-coefficient cutoff.
  //
  // Otsu's bimodality is unstable on tiny samples: a 4-CPU probe yields
  // only 6 off-diagonal pairs, and ordinary timing jitter can push the
  // BCV/TV ratio above 0.55 even on a homogeneous CCD. Require at least
  // 10 pairs (= 5 CPUs) before trusting the histogram split; on smaller
  // pools fall through to the sysfs prior, which on a single-CCD
  // worker subset returns one cluster.
  constexpr double kBimodalCutoff = 0.55;
  constexpr std::size_t kMinOtsuPairs = 10U;
  std::vector<std::uint32_t> clusterId(n, 0U);
  std::uint32_t numClusters = 1U;

  if (offDiag.size() >= kMinOtsuPairs && ot.bimodality >= kBimodalCutoff) {
    // Connected-components on the "fast" graph (union-find).
    std::vector<std::uint32_t> parent(n);
    std::iota(parent.begin(), parent.end(), 0U);
    auto find = [&](std::uint32_t x) {
      while (parent[x] != x) {
        parent[x] = parent[parent[x]];
        x = parent[x];
      }
      return x;
    };
    auto unite = [&](std::uint32_t a, std::uint32_t b) {
      a = find(a);
      b = find(b);
      if (a != b) {
        parent[a] = b;
      }
    };
    for (std::uint32_t i = 0; i < n; ++i) {
      for (std::uint32_t j = i + 1U; j < n; ++j) {
        if (mat.matrix[i][j] > 0.0 &&
            std::log(mat.matrix[i][j]) <= ot.threshold) {
          unite(i, j);
        }
      }
    }
    // Compact roots to dense cluster ids.
    constexpr std::uint32_t kUnassigned = UINT32_MAX;
    std::vector<std::uint32_t> rootToCluster(n, kUnassigned);
    std::uint32_t next = 0U;
    for (std::uint32_t i = 0; i < n; ++i) {
      const std::uint32_t r = find(i);
      if (rootToCluster[r] == kUnassigned) {
        rootToCluster[r] = next;
        ++next;
      }
      clusterId[i] = rootToCluster[r];
    }
    numClusters = next;
  } else if (!sysfsPrior.empty()) {
    // Sysfs prior fallback. Each probed CPU keyed into the prior keeps
    // the prior's cluster id; any CPU absent from every prior group gets
    // a fresh cluster id so it cannot be silently merged with cluster 0.
    std::uint32_t maxCpu = 0U;
    for (const auto &group : sysfsPrior) {
      for (auto c : group) {
        maxCpu = std::max(maxCpu, c);
      }
    }
    std::vector<std::int32_t> sysfsCluster(maxCpu + 1U, -1);
    for (std::size_t k = 0; k < sysfsPrior.size(); ++k) {
      for (auto c : sysfsPrior[k]) {
        sysfsCluster[c] = static_cast<std::int32_t>(k);
      }
    }
    auto nextOrphanId = static_cast<std::uint32_t>(sysfsPrior.size());
    std::uint32_t maxSeen = 0U;
    for (std::uint32_t i = 0; i < n; ++i) {
      const auto cpu = mat.cpus[i];
      if (cpu < sysfsCluster.size() && sysfsCluster[cpu] >= 0) {
        clusterId[i] = static_cast<std::uint32_t>(sysfsCluster[cpu]);
      } else {
        clusterId[i] = nextOrphanId;
        ++nextOrphanId;
      }
      maxSeen = std::max(maxSeen, clusterId[i]);
    }
    numClusters = maxSeen + 1U;
  }

  out.clusterIdOfCpuIndex = std::move(clusterId);
  out.numClusters = numClusters;
  out.clusterDistanceNs.assign(numClusters,
                               std::vector<double>(numClusters, 0.0));
  // Median pairwise per-cluster-pair distance.
  std::vector<std::vector<std::vector<double>>> bucket(
      numClusters, std::vector<std::vector<double>>(numClusters));
  for (std::uint32_t i = 0; i < n; ++i) {
    for (std::uint32_t j = i; j < n; ++j) {
      const std::uint32_t ci = out.clusterIdOfCpuIndex[i];
      const std::uint32_t cj = out.clusterIdOfCpuIndex[j];
      if (i == j) {
        continue;
      }
      bucket[ci][cj].push_back(mat.matrix[i][j]);
      if (ci != cj) {
        bucket[cj][ci].push_back(mat.matrix[i][j]);
      }
    }
  }
  for (std::uint32_t a = 0; a < numClusters; ++a) {
    for (std::uint32_t b = 0; b < numClusters; ++b) {
      if (bucket[a][b].empty()) {
        continue;
      }
      std::sort(bucket[a][b].begin(), bucket[a][b].end());
      out.clusterDistanceNs[a][b] = bucket[a][b][bucket[a][b].size() / 2U];
    }
  }
  return out;
}

/// Top-level: build the latency matrix, cluster, populate
/// `CoherenceProbe`. `cpus` is the flat list of CPU ids the pool is
/// allowed to schedule on (typically the union of the sysfs CCD groups
/// intersected with the process affinity mask). `sysfsPrior` is the
/// per-CCD CPU grouping from `detail::detectTopology()`; used only as
/// a fallback when the latency histogram is unimodal.
inline CoherenceProbe
runCoherenceProbe(const std::vector<std::uint32_t> &cpus,
                  const std::vector<std::vector<std::uint32_t>> &sysfsPrior,
                  std::uint32_t roundTrips = 1024U) noexcept {
  CoherenceProbe out;
  if (cpus.size() < 2U) {
    return out;
  }
  out.matrix = probeLatencyMatrix(cpus, roundTrips);
  if (!out.matrix.valid) {
    return out;
  }
  out.clusters = clusterByLatency(out.matrix, sysfsPrior);
  out.valid = !out.clusters.clusterIdOfCpuIndex.empty();

  // Convenience scalar: max(cluster i to cluster j median) / max(cluster
  // i intra median). Intra is the diagonal of `clusterDistanceNs`. If
  // there is only one cluster the ratio is 1.0.
  double maxCross = 0.0;
  double maxIntra = 0.0;
  for (std::uint32_t a = 0; a < out.clusters.numClusters; ++a) {
    maxIntra = std::max(maxIntra, out.clusters.clusterDistanceNs[a][a]);
    for (std::uint32_t b = 0; b < out.clusters.numClusters; ++b) {
      if (a == b) {
        continue;
      }
      maxCross = std::max(maxCross, out.clusters.clusterDistanceNs[a][b]);
    }
  }
  if (maxIntra > 0.0 && maxCross > 0.0) {
    out.maxCrossOverIntraRatio = maxCross / maxIntra;
  }
  return out;
}

/// Process-wide cache for `runCoherenceProbe`, keyed on the
/// sorted-unique `cpus` vector. The latency matrix depends only on the
/// host hardware, so repeated probes for the same set are duplicate
/// work; test suites, bench harnesses, and `PoolGroup` arenas reuse a
/// single matrix per process.
inline CoherenceProbe
cachedCoherenceProbe(const std::vector<std::uint32_t> &cpus,
                     const std::vector<std::vector<std::uint32_t>> &sysfsPrior,
                     std::uint32_t roundTrips = 1024U) {
  std::vector<std::uint32_t> key = cpus;
  std::sort(key.begin(), key.end());
  key.erase(std::unique(key.begin(), key.end()), key.end());

  static std::mutex cacheMutex;
  static std::map<std::vector<std::uint32_t>, CoherenceProbe> cache;

  {
    const std::scoped_lock guard(cacheMutex);
    const auto hit = cache.find(key);
    if (hit != cache.end()) {
      return hit->second;
    }
  }

  // Run the probe outside the lock so a concurrent caller probing a
  // different key is not blocked. A duplicate-probe race for the same
  // key is harmless: the second insertion is dropped, and we return the
  // first inserter's copy so identical pools see identical numbers.
  CoherenceProbe fresh = runCoherenceProbe(cpus, sysfsPrior, roundTrips);

  const std::scoped_lock guard(cacheMutex);
  return cache.emplace(std::move(key), std::move(fresh)).first->second;
}

} // namespace citor::detail

// ===== citor/function_ref.h =====


namespace citor {

/// Primary template; the specialization below handles function-type signatures.
/// Instantiating `FunctionRef` with a non-function type produces a compile-time
/// diagnostic.
template <class Sig>
class FunctionRef;

// Non-owning callable reference; analogous to `std::string_view` for callables.
// Stores two pointers: an erased object pointer and a thunk that invokes the
// original callable through it.
//
// Parameter-only: the bound callable's lifetime must exceed every invocation
// through this `FunctionRef`. Storing one as a class data member, or returning
// one from a function that constructed the bound callable, creates a dangling
// pointer.
//
// The shape (16 bytes, two pointers, no allocation) matches
// `llvm::function_ref`, `absl::FunctionRef`, and `folly::FunctionRef`.
// Trivially copyable, trivially destructible, and passable in registers. Each
// instance is single-owner; concurrent invocation from multiple threads is
// undefined.
template <class R, class... Args>
class FunctionRef<R(Args...)> {
public:
  // Construct an empty `FunctionRef`. Invoking an empty instance is undefined.
  constexpr FunctionRef() noexcept = default;

  /// Bind to a callable |fn| living in the caller's storage. Stores a
  /// non-owning pointer to |fn| and a thunk that invokes it through the erased
  /// pointer. The SFINAE constraint excludes `FunctionRef`-of-`FunctionRef`
  /// self-binding so copy semantics stay intact.
  template <class F>
    requires(!std::is_same_v<std::remove_cv_t<std::remove_reference_t<F>>,
                             FunctionRef> &&
             std::is_invocable_r_v<R, F &, Args...>)
  // NOLINTNEXTLINE(bugprone-forwarding-reference-overload)
  constexpr FunctionRef(F &&fn) noexcept
      : m_obj(
            const_cast<void *>(static_cast<const void *>(std::addressof(fn)))),
        m_invoke(
            &FunctionRef::template invokeImpl<std::remove_reference_t<F>>) {}

  // Forward |args| through the thunk to the bound callable. Behavior is
  // undefined if the underlying callable has been destroyed or this
  // `FunctionRef` is empty.
  R operator()(Args... args) const {
    return m_invoke(m_obj, std::forward<Args>(args)...);
  }

  // Equality on (object pointer, thunk pointer). Used by descriptor write
  // elision: when the same callable is re-bound across back-to-back calls
  // (steady-state bench loop), the producer skips the redundant store and
  // keeps the descriptor cache line MESI-Shared.
  constexpr bool operator==(const FunctionRef &other) const noexcept {
    return m_obj == other.m_obj && m_invoke == other.m_invoke;
  }
  constexpr bool operator!=(const FunctionRef &other) const noexcept {
    return !(*this == other);
  }

  // Returns `true` if a callable is bound; `false` for a default-constructed
  // instance.
  [[nodiscard]] constexpr explicit operator bool() const noexcept {
    return m_invoke != nullptr;
  }

private:
  /// Static thunk that downcasts the erased pointer back to the source type
  /// and invokes it. Lives as a function pointer in the `m_invoke` slot so the
  /// `FunctionRef` pays no virtual call cost.
  template <class F>
  static R invokeImpl(void *obj, Args... args) {
    return (*static_cast<F *>(obj))(std::forward<Args>(args)...);
  }

  /// Erased pointer to the bound callable; null when empty.
  void *m_obj = nullptr;
  /// Thunk that recovers the source type and invokes the callable; null when
  /// empty.
  R (*m_invoke)(void *, Args...) = nullptr;
};

// Two-pointer (16-byte) layout invariant. The dispatch hot path depends on
// this shape; if a future compiler regresses it, the hot-dispatch budget
// breaks.
static_assert(sizeof(FunctionRef<void(std::size_t, std::size_t)>) == 16,
              "FunctionRef must be exactly two pointers (16 bytes on x86-64)");

} // namespace citor

// ===== citor/detail/forkjoin_state.h =====



namespace citor::detail {

/// Stack-resident shared state for one in-flight `citor::ThreadPool::forkJoin`
/// call.
///
/// The producer constructs a `ForkJoinState` on its stack, populates the
/// immutable shape, and publishes a non-owning pointer to every participating
/// worker via the worker-loop dispatch descriptor. Workers consume tasks from
/// their own Chase-Lev deque, falling back to victim stealing when their deque
/// drains. The producer participates as slot 0 and joins on the `pendingTasks`
/// countdown reaching zero.
///
/// Layout invariants:
/// - `pendingTasks` lives on its own line; it is the single contended atomic on
/// the dispatch
///   completion path. Workers `fetch_sub(1)` on it after retiring a task so the
///   producer's spin-then-park loop can detect zero without reading any
///   per-worker slot.
/// - `forkJoinCancelled` lives on its own line so cancellation broadcast does
/// not interfere with
///   the per-task hot path. Workers acquire-read this flag at task-boundary
///   chunks; a stopped token causes any victim probe to stop emitting fresh
///   task descriptors.
/// - `firstException` is on its own line; the CAS-from-null path is cold.
///
/// Padding-suppression note: the layout keeps every contended atomic on its own
/// `kCacheLine`-sized line, so the analyser's "excessive padding" warning is
/// the design trade-off we want -- false-sharing avoidance over byte-tight
/// packing.
// Lower store-queue stalls under recursive spawn:
// libfork's per-call frame is 36-40 B on a single cache line, no internal
// alignas padding. citor's prior layout (4 alignas(kCacheLine=128)-separated
// blocks) issued 3-4 separate Read-For-Ownership transactions per recursive
// forkJoin -- one per cold cache line touched. Collapsing the contended atomics
// onto a single line cuts per-call RFOs from 3-4 to 1. False-sharing on the
// no-cancel/no-throw common path is moot: peer writes to
// `cancelled`/`firstException` only fire on the cold cancel or throw paths, so
// the producer's hot `pendingTasks` poll keeps the line stable.
struct alignas(kCacheLine) ForkJoinState {
  /// Number of participants (producer + background workers) collaborating in
  /// the call.
  std::uint32_t participants = 0;

  /// Cancellation flag broadcast by the producer's cancellation observer or by
  /// any participant that observed an exception. Co-located with `pendingTasks`
  /// because the steady-state common path never writes it; the no-cancel /
  /// no-throw call shape leaves the field at 0 throughout, so peer `fetch_sub`
  /// traffic on `pendingTasks` keeps the line uncontended.
  std::atomic<std::uint32_t> forkJoinCancelled{0};

  /// CCD index for each participant slot; used by the victim-selection RNG
  /// to bias stealing toward same-CCD victims when
  /// `StealPolicy::ClusterLocal` is requested. Sized `participants`.
  const std::uint32_t *ccdOfSlot = nullptr;

  /// CCD-local steal-policy flag derived from the call's
  /// `HintsT::stealPolicy`. When `true`, the victim-selection probe order
  /// biases toward same-CCD workers; when `false`, the probe order is a
  /// uniform xorshift random.
  bool preferSameCcd = false;

  /// Cancellation token observed by workers at task-boundary chunks; copied
  /// from the producer when `forkJoin` is invoked. Workers stop emitting fresh
  /// tasks once the token is stopped.
  CancellationToken token;

  /// Outstanding task count. Producer initializes with the root task pack size;
  /// each task `fetch_sub(1)` after retiring; the producer's join spins on `<=
  /// 0`.
  std::atomic<std::int64_t> pendingTasks{0};

  /// First-exception capture slot. Workers CAS from null on throw; producer
  /// reads after join. Allocation only happens on the cold throw path;
  /// default-init nullptr keeps the line clean for the no-throw common path so
  /// peer fetch_sub on `pendingTasks` does not bounce against a contended
  /// exception slot.
  std::atomic<std::exception_ptr *> firstException{nullptr};
};

/// Type-erased recursive task descriptor stored in a worker's Chase-Lev deque.
///
/// The deque holds raw `void *` payloads and the worker dereferences them as
/// `Task *` pointers back to descriptors that live either on the producer's
/// stack (root tasks) or in a per-worker arena (recursive children spawned
/// during a forkJoin body). The descriptor's `body` is a `FunctionRef` over
/// `void()`, kept by the call site for the duration of the synchronous call.
///
/// The owning lifetime of `Task` is tied to the synchronous
/// `ThreadPool::forkJoin` call: once the producer's join observes `pendingTasks
/// == 0`, every task body has retired, no worker holds a pointer to any
/// descriptor, and the producer's stack frame can unwind safely.
struct Task {
  /// Non-owning reference to the user's callable. The callable lives in the
  /// producer or in a recursive task arena for the duration of the synchronous
  /// call.
  FunctionRef<void()> body;

  /// Pointer to the call's shared state, used by the worker to record
  /// exceptions and decrement the outstanding-task counter after the body
  /// retires.
  ForkJoinState *state = nullptr;
};

} // namespace citor::detail

// ===== citor/detail/futex_park.h =====


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
  // Bounded 1 ms re-park instead of `INFINITE`: gives the worker a
  // periodic chance to observe shutdown or a mailbox update that
  // raced the wake. Caller re-parks when nothing is pending. Linux's
  // `FUTEX_WAIT` is driven by the explicit wake and stays on
  // `INFINITE`.
  constexpr DWORD kWindowsParkTimeoutMs = 1U;
  const BOOL ok = ::WaitOnAddress(static_cast<volatile VOID *>(addr), &compare,
                                  sizeof(std::uint32_t), kWindowsParkTimeoutMs);
  return ok ? 0 : -1;
}

/// Windows peer of `FUTEX_WAKE_PRIVATE`. `WakeByAddressAll` wakes every
/// waiter parked on `addr`. Caller must publish the source-of-truth
/// state before invoking, so a freshly-parked waiter either observes
/// the update or is woken.
inline long futexWakePrivate(std::atomic<std::uint32_t> *addr, int n) noexcept {
  // Collapse any `n >= 2` request to a single `WakeByAddressAll`:
  // `WakeByAddressSingle` has no batched variant, so an N-fold loop
  // would issue N separate wakes. Broadcast wakes the queue in one
  // call; spurious wakees re-park on their next mailbox check.
  // `n == 1` still uses `WakeByAddressSingle` to avoid disturbing a
  // second parked waiter.
  if (n <= 0) {
    return 0;
  }
  if (n == 1) {
    ::WakeByAddressSingle(static_cast<PVOID>(addr));
    return 0;
  }
  ::WakeByAddressAll(static_cast<PVOID>(addr));
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

// ===== citor/detail/hints_traits.h =====

// Compile-time predicates over `HintsT` parameters. Centralises the
// `requires { HintsT::cancellationChecks; }` probe + default that every
// primitive's entry would otherwise open-code.

namespace citor::detail {

/// Cancellation-active probe. `true` when |HintsT| has no
/// `cancellationChecks` member (default to honour the token), or when
/// `HintsT::cancellationChecks` is `true`.
template <class HintsT>
inline constexpr bool kCancellationActive = []() {
  if constexpr (requires { HintsT::cancellationChecks; }) {
    return HintsT::cancellationChecks;
  } else {
    return true;
  }
}();

} // namespace citor::detail

// ===== citor/detail/inline_fallback.h =====


namespace citor::detail {

/// Predicate gating the inline fallback for small fan-outs.
///
/// The pool's dispatch overhead is amortized over |n| items at
/// |estimatedItemNs| each; when the fan-out's per-participant wall time falls
/// below |minTaskUs|, the dispatch round-trip dominates and the producer should
/// run inline. The exact gate is `n * estimatedItemNs * 1e-3 < minTaskUs *
/// participants`, in `double` arithmetic to avoid integer-overflow surprises.
///
/// When |estimatedItemNs| is zero, the gate defaults to "do not inline": the
/// inline fallback is opt-in via a non-zero estimate.
[[nodiscard]] inline bool shouldRunInline(std::size_t n,
                                          std::size_t participants,
                                          double estimatedItemNs,
                                          double minTaskUs) noexcept {
  if (participants <= 1) {
    return true;
  }
  if (n == 0) {
    return true;
  }
  if (estimatedItemNs <= 0.0) {
    return false;
  }
  const double estimatedTotalUs =
      static_cast<double>(n) * estimatedItemNs * 1e-3;
  const double threshold = minTaskUs * static_cast<double>(participants);
  return estimatedTotalUs < threshold;
}

/// Compile-time-hinted variant. With a non-zero `HintsT::estimatedItemNs` the
/// caller pays the runtime gate; otherwise the gate folds to `participants <=
/// 1` at compile time. Centralises the `if constexpr` cascade open-coded at
/// every typed primitive's entry.
template <class HintsT>
[[nodiscard]] inline bool shouldRunInlineHinted(std::size_t n,
                                                std::size_t participants) {
  if constexpr (HintsT::estimatedItemNs > 0.0) {
    return shouldRunInline(n, participants, HintsT::estimatedItemNs,
                           HintsT::minTaskUs);
  } else {
    (void)n;
    return participants <= 1;
  }
}

} // namespace citor::detail

// ===== citor/detail/job_descriptor.h =====



namespace citor::detail {

/// Stack-resident job-publish protocol used by every fan-out primitive.
///
/// A `JobDescriptor` lives on the producer's stack for the duration of one
/// synchronous primitive call. The producer fills it before publishing the new
/// generation; workers read it via the acquire-load on `PoolControl::activeJob`
/// after observing the matching generation. The descriptor is single-writer
/// (producer fills, then publishes), many-reader (workers consume).
///
/// Layout:
/// - The first cache line holds the immutable descriptor body (range bounds,
/// chunk shape,
///   participants, balance / priority, body / token). Workers acquire-load
///   these once after observing the matching generation.
/// - The contended atomics (`nextBlock`, `firstException`, `exceptionWorkerId`)
/// sit on dedicated
///   `kCacheLine`-sized lines so concurrent dynamic-counter increments and
///   exception CAS attempts do not invalidate the immutable body.
///
/// The descriptor's `body` is a `FunctionRef` pointing into a closure that
/// lives on the producer's stack. Because every primitive in v1 is synchronous
/// (the producer joins before returning), the closure outlives the descriptor
/// by construction.
///
/// The padding overhead trades several hundred bytes of stack against
/// MESI cache-coherency traffic on the contended atomics, which is the dominant
/// hot-path cost.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct alignas(kCacheLine) JobDescriptor {
  /// Generation value the producer published this descriptor under; workers
  /// stamp `doneEpoch` with the same value once their share completes so the
  /// producer's join can rendezvous.
  std::uint64_t generation = 0;

  /// Inclusive start of the iteration range.
  std::size_t first = 0;

  /// Exclusive end of the iteration range.
  std::size_t last = 0;

  /// Static block grain. Must be at least 1; a primitive computes the value
  /// once before publishing.
  std::size_t chunk = 0;

  /// Total number of blocks the producer carved the range into.
  std::size_t blockCount = 0;

  /// Number of participants (producer + background workers) the dispatch was
  /// sized against.
  std::uint32_t participants = 0;

  /// Compile-time balance choice serialized into the descriptor for the runtime
  /// path; the member-template path bakes the choice into the call shape via
  /// `if constexpr`.
  Balance balance = Balance::StaticUniform;

  /// Compile-time priority choice serialized into the descriptor for the
  /// runtime path.
  Priority priority = Priority::Throughput;

  /// Opt-in flag for the producer-side hot-completion probe in
  /// `dispatchOneStaticLockedBody`. When `true`, the producer briefly probes
  /// every background worker's `doneEpoch` after publishing the new generation;
  /// if every background worker has already stamped the new epoch (i.e.
  /// spinning workers picked up the dispatch and finished an empty / trivial
  /// body before the probe ran), the producer skips the futex-word bump and the
  /// `FUTEX_WAKE_PRIVATE(INT_MAX)` syscall entirely. Independent one-shot
  /// primitives
  /// (`parallelFor`, `parallelReduce`, `bulkForQueries`) opt in;
  /// protocol-driving primitives
  /// (`parallelChain`, `parallelScan`, `runPlex`, `forkJoin`) leave the flag
  /// default-`false` because their wrapper bodies must run to completion
  /// regardless of when the producer observes done. Sits inside the existing
  /// 16-bit padding before `body`, so it adds no descriptor size.
  bool preWakeCompletionProbe = false;

  /// Non-owning reference to the user's closure. Lives on the producer's stack
  /// for the duration of the synchronous primitive call.
  FunctionRef<void(std::size_t, std::size_t)> body;

  /// Cancellation token observed by workers at chunk boundaries when the call
  /// site requested it.
  CancellationToken token;

  /// Direct pointer to the user's callable. Set by `parallelFor<HintsT,F>` so
  /// the typed `workerEntry` runner can recover `F*` and call it without going
  /// through `desc.body`'s FunctionRef indirection. Null for legacy primitives.
  void *fnPtr = nullptr;

  /// Optional monomorphized worker entry. When non-null, workers call this
  /// function pointer instead of `runActiveJob -> runStaticPartition`. Set by
  /// `parallelFor<HintsT,F>` to a runner specialized on (HintsT, F) so token
  /// check / try-catch elide via if-constexpr.
  void (*workerEntry)(JobDescriptor *, std::uint32_t,
                      std::uint64_t) noexcept = nullptr;

  /// Pointer to the pool's `WorkerState` array, set by primitives that opt into
  /// producer cold-collapse (`parallelFor<HintsT, F>` only at present). When
  /// non-null, the worker entry CAS-races the producer's join path on
  /// `WorkerState[rank].claimedAt`; whoever wins runs rank R's blocks, the
  /// loser stamps mailbox=doneSentinel without re-running the work. When null,
  /// the legacy "every worker runs its own blocks" protocol holds (used by
  /// parallelReduce / parallelScan / runPlex / forkJoin which need rank-keyed
  /// partial outputs). The pointer is a `void*` so this header does not have to
  /// pull in `worker_state.h`.
  void *workerStateBase = nullptr;

  /// First-exception capture slot. Workers `compare_exchange` this from null to
  /// a heap-allocated `std::exception_ptr` to record the first failure
  /// deterministically; subsequent throws drop. Worker rank that filled the
  /// slot is captured alongside so the slot's line carries both values that
  /// change together. Co-located with `token` on the secondary publication line
  /// so the worker's per-block exception probe and cancellation poll share one
  /// cache-line fetch. Common-case writes here are rare (only on a throwing
  /// body), so the producer's body-line stays uninvalidated.
  std::atomic<std::exception_ptr *> firstException{nullptr};

  /// Worker rank that filled `firstException`; used to break ties between
  /// simultaneous throws.
  std::atomic<std::uint32_t> exceptionWorkerId{0};

  /// Centralized counter used by `Balance::DynamicChunked`; workers race on a
  /// relaxed `fetch_add(1)` to claim the next block id. Sits on its own line so
  /// contention here does not invalidate the immutable descriptor body or the
  /// exception-capture line.
  alignas(kCacheLine) std::atomic<std::uint64_t> nextBlock{0};
};

} // namespace citor::detail

// ===== citor/detail/kahan.h =====

namespace citor::detail {

/// Compensated floating-point pair carrying a running sum and its rounding
/// residual.
///
/// `KahanPair` represents a partial reduction state used by
/// `Determinism::KahanCompensated` `parallelReduce` calls. Every per-chunk
/// accumulator is a `KahanPair`; per-chunk pairs are combined deterministically
/// through `kahanCombine` so the whole reduction tree carries compensation
/// through every interior node.
///
/// `sum` is the running cancelled sum and `c` is the running compensation term
/// (negated lost-low-bits). The pair is initialized to zero by default; user
/// code never sees a partially constructed pair on the producer's stack.
struct KahanPair {
  /// Cancelled running sum.
  double sum = 0.0;

  /// Compensation term (negated lost-low-bits).
  double c = 0.0;
};

/// Add a scalar |x| to a running `KahanPair` accumulator using Kahan
/// compensation.
///
/// Implements one step of the textbook compensated summation. The compensation
/// term |a|.c is subtracted from |x| to recover the previously lost low bits
/// before the running sum is bumped; the new compensation captures the rounding
/// error introduced by this step.
///
/// a Current accumulator.
/// x Scalar to add.
/// New accumulator with |x| folded in.
[[nodiscard]] inline KahanPair kahanAdd(KahanPair a, double x) noexcept {
  const double y = x - a.c;
  const double t = a.sum + y;
  KahanPair r;
  r.c = (t - a.sum) - y;
  r.sum = t;
  return r;
}

/// Combine two `KahanPair` accumulators into a single compensated sum.
///
/// Used at every interior node of the chunk-id pairwise reduction tree: each
/// subtree's partial sum is itself a `KahanPair`, and combining two siblings
/// preserves the compensation contract. The implementation folds |b|.sum into
/// |a| via `kahanAdd`, then folds |b|.c (the right child's compensation) so the
/// residual carried into the parent is the sum of both children's residuals.
///
/// a Left subtree accumulator.
/// b Right subtree accumulator.
/// Combined accumulator covering both subtrees.
[[nodiscard]] inline KahanPair kahanCombine(KahanPair a, KahanPair b) noexcept {
  const KahanPair afterSum = kahanAdd(a, b.sum);
  return kahanAdd(afterSum, -b.c);
}

} // namespace citor::detail

// ===== citor/detail/lookback_scan.h =====



namespace citor::detail {

/// Per-tile state for Merrill-Garland 2016 decoupled-lookback scan.
///
/// Each tile lives on its own cache line so a worker's release-store on
/// `flag` for one tile does not invalidate a neighbour's flag line. The
/// state machine is:
///
///   `Initialized` -> `AggregateAvailable` -> `PrefixAvailable`
///
/// A tile's worker:
///   1. Reads `d.in[tile_lo..tile_hi]`, computes local total and writes
///      the chunk-local inclusive scan into `d.out[tile_lo..tile_hi]`.
///   2. Release-stores `aggregate` and lifts `flag` to
///      `AggregateAvailable`.
///   3. Walks predecessors, summing `aggregate`s, until it finds a
///      predecessor in `PrefixAvailable` (acquire). That predecessor's
///      `prefix` plus the accumulated aggregates is this tile's prefix.
///   4. Release-stores `prefix` and lifts `flag` to `PrefixAvailable`.
///   5. Adds `prefix` in place over `d.out[tile_lo..tile_hi]`.
///
/// Tile 0 is special: its prefix is `identity`, so it can skip the
/// lookback walk entirely and proceed straight from step 2 to step 4.
template <class T>
struct alignas(kCacheLine) LookbackTile {
  /// State-machine value stored in `flag`. The transitions are monotonic:
  /// `Initialized` -> `AggregateAvailable` -> `PrefixAvailable`.
  enum class Flag : std::uint8_t {
    /// The tile owns a slot but has not yet computed its local aggregate.
    Initialized = 0,
    /// `aggregate` is published and synchronises through an acquire-load
    /// of `flag`. The prefix is not yet known.
    AggregateAvailable = 1,
    /// `prefix` is published and synchronises through an acquire-load of
    /// `flag`. Successors may stop their lookback walk at this tile.
    PrefixAvailable = 2,
  };

  /// State-machine bit; release-stored after the corresponding payload
  /// (aggregate or prefix) has been written, so an acquire-load of
  /// `flag` synchronises with the payload.
  std::atomic<std::uint64_t> flag{0};

  /// Tile's local total. Valid (well-defined) when `flag >=
  /// AggregateAvailable`.
  T aggregate{};

  /// Tile's exclusive prefix. Valid when `flag >= PrefixAvailable`.
  T prefix{};
};

/// Walk back from `myTile - 1`, summing predecessor aggregates, until a
/// predecessor is observed in `PrefixAvailable` state. Returns the
/// computed prefix for `myTile`.
///
/// `prefix` is the user-supplied associative combiner; the walk
/// composes left-to-right (oldest predecessor first) to preserve
/// associativity even when the combiner is not commutative.
///
/// The walk avoids stalling on a slow predecessor by spinning with
/// `cpuRelax()`; on workloads where every tile's Pass-1 work is
/// roughly uniform, the chain typically terminates after one step (the
/// immediate predecessor's prefix is already published by the time the
/// successor finishes its own aggregate).
///
/// `noexcept` follows the user combiner: a throwing |prefix| propagates
/// to the caller, which the engine catches in `runInclusiveScanLookback`
/// and surfaces through `firstException`.
template <class T, class PrefixFn>
[[gnu::always_inline]] inline T
lookbackWalk(LookbackTile<T> *tiles, std::uint32_t myTile, T identity,
             PrefixFn &&prefix) noexcept(noexcept(prefix(std::declval<T>(),
                                                         std::declval<T>()))) {
  // `accum` carries the running total of every tile whose `aggregate`
  // we have folded in but whose `prefix` was not yet published. When
  // we hit a tile in `PrefixAvailable` state, that tile's prefix
  // covers everything to its left, so the result is
  // `prefix.left = prefix(prefix.left, peer.prefix, peer.aggregate, accum)`.
  // Compose left-to-right (peer is to the left of accum) so the user
  // monoid only needs to be associative, not commutative.
  T accum = identity;
  std::uint32_t cursor = myTile;
  while (cursor > 0U) {
    --cursor;
    auto &peer = tiles[cursor];
    while (true) {
      const auto state = peer.flag.load(std::memory_order_acquire);
      if (state >=
          static_cast<std::uint64_t>(LookbackTile<T>::Flag::PrefixAvailable)) {
        // peer.prefix is the exclusive total of [0, cursor_lo);
        // peer.aggregate is the total of [cursor_lo, cursor_hi);
        // accum is the total of [cursor_hi, myTile_lo). Stitch them
        // left-to-right.
        T leftCombined = prefix(peer.prefix, peer.aggregate);
        return prefix(std::move(leftCombined), std::move(accum));
      }
      if (state >= static_cast<std::uint64_t>(
                       LookbackTile<T>::Flag::AggregateAvailable)) {
        // peer's prefix not yet ready; fold its aggregate into `accum`
        // and keep walking back.
        accum = prefix(peer.aggregate, std::move(accum));
        break;
      }
      cpuRelax();
    }
  }
  return accum;
}

} // namespace citor::detail

// ===== citor/detail/plex_state.h =====



namespace citor::detail {

/// Per-worker phase-completion slot used by the persistent-worker plex
/// protocol.
///
/// Each slot lives on its own `citor::kCacheLine` -sized line so the producer's
/// acquire-loads on `done` during phase rendezvous never collide with a
/// neighbouring worker's release-store. The owning worker writes `done` exactly
/// once per phase via release; the producer reads it via acquire to confirm the
/// worker's slice for that phase has retired.
struct alignas(kCacheLine) PlexDoneSlot {
  /// Phase epoch the worker last finished. Producer waits until `done >=
  /// currentPhase` before admitting the next phase.
  std::atomic<std::uint64_t> done{0};
};

/// Stack-resident state shared by the producer and background workers across
/// all phases of a
///        single `runPlex` call.
///
/// Layout invariants:
/// - `currentPhase` lives on its own cache line so the producer's release-store
/// does not invalidate
///   per-worker state every phase.
/// - `phaseCancelled` lives on its own line so cancellation broadcast does not
/// interfere with the
///   producer's phase-publish hot path.
/// - `done[w]` lives on a dedicated line via `PlexDoneSlot` so adjacent
/// workers' release-stores
///   do not contend for the same cache line.
///
/// The state is owned by the producer's stack via a `std::unique_ptr` so the
/// trailing `done[]` vector is heap-resident with cache-line alignment but the
/// producer still controls the lifetime. Workers receive a non-owning pointer
/// through the dispatch closure.
///
/// Padding-suppression note: the layout keeps every contended atomic on its own
/// `kCacheLine`-sized line, so the analyser's "excessive padding" warning is
/// the design trade-off we want -- false-sharing avoidance over byte-tight
/// packing.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct PlexState {
  /// Total number of phases the user requested.
  std::size_t nPhases = 0;

  /// Per-phase row range upper bound passed in by the caller; used for slot
  /// partitioning.
  std::size_t n = 0;

  /// Number of participants (producer + background workers) collaborating in
  /// the plex.
  std::uint32_t participants = 0;

  /// Phase epoch published by the producer. Workers acquire-spin until
  /// `currentPhase >= localPhase` before admitting their slice for
  /// `localPhase`.
  ///
  /// Initial value is `0`; the producer publishes `1, 2, ..., nPhases` in
  /// order. Workers complete phase `p` when they observe `currentPhase >= p`,
  /// then signal `done[slot] = p`.
  alignas(kCacheLine) std::atomic<std::uint64_t> currentPhase{0};

  /// Cancellation flag flipped by the producer's cancellation observer.
  ///
  /// Worker bodies check this between phases and exit cleanly when set. The
  /// flag is release-written by the producer at most once per call (when the
  /// user's token transitions to stopped), and acquire-read by workers between
  /// phases.
  alignas(kCacheLine) std::atomic<std::uint32_t> phaseCancelled{0};

  /// First-exception capture slot shared across all participants.
  ///
  /// Workers `compare_exchange` this from null to a heap-allocated
  /// `std::exception_ptr` to record the first failure deterministically;
  /// subsequent throws drop. The producer reads the slot after joining and
  /// rethrows if non-null.
  alignas(kCacheLine) std::atomic<std::exception_ptr *> firstException{nullptr};

  /// Borrowed pointer to the pool's pre-allocated per-worker completion slots.
  ///
  /// The pool owns the storage (`ThreadPool::m_plexDoneSlots`); the plex call
  /// reserves a fresh interval `[epochBase, epochBase + nPhases]` in the pool's
  /// monotonically-advancing `m_plexEpochBase` counter so successive calls
  /// observe disjoint targets without zero-resetting the slots. Sized
  /// `participants` valid elements; reading past that index is undefined.
  PlexDoneSlot *doneSlots = nullptr;

  /// Per-call base of the pool's monotonic done-epoch counter.
  ///
  /// Stamps are absolute: `done = epochBase + p` after running phase `p`. Waits
  /// compare against `epochBase + p`. The producer reserves the interval under
  /// the dispatch gate before publishing, so prior-dispatch stamps cannot
  /// satisfy a current wait. Cancellation stamps `epochBase + nPhases` (the
  /// saturation value) so peers waiting on any active phase advance.
  std::uint64_t epochBase = 0;

  /// Subscript a slot by index.
  ///
  /// The slot itself owns mutable atomics; the accessor is `const` because
  /// reading from the borrowed pointer does not modify the owning state, and
  /// the returned slot's atomics carry their own internal mutability.
  ///
  /// idx Slot index in `[0, participants)`.
  /// Reference to the slot.
  [[nodiscard]] PlexDoneSlot &doneSlot(std::size_t idx) const noexcept {
    return doneSlots[idx];
  }

  /// Compute a slot's contiguous row range over `[0, n)` using static
  /// partitioning.
  ///
  /// The partition is `lo = (n * slot) / participants`, `hi = (n * (slot + 1))
  /// / participants`, matching the prim_mst_backend.h convention so the
  /// migration produces bit-identical block boundaries.
  ///
  /// slot Worker slot index in `[0, participants)`.
  /// `(lo, hi)` pair denoting the slot's contiguous range over `[0, n)`.
  [[nodiscard]] std::pair<std::size_t, std::size_t>
  slotRange(std::uint32_t slot) const noexcept {
    const auto lo = static_cast<std::size_t>(mulDiv64(n, slot, participants));
    const auto hi =
        static_cast<std::size_t>(mulDiv64(n, slot + 1U, participants));
    return {lo, hi};
  }
};

} // namespace citor::detail

// ===== citor/detail/pool_control.h =====



namespace citor::detail {

/// Process-internal control word shared between producer and workers.
///
/// Four contended atomics (`generation`, `futexWord`, `activeJob`,
/// `hotSpinDepth`) plus a const `participants` count form the source of truth
/// for pool state. Each contended atomic is on its own `kCacheLine`-sized line
/// so MESI traffic on one never invalidates another. The layout places
/// `generation` (release publish), `futexWord` (parking token), `activeJob`
/// (descriptor pointer), a low-latency spin-depth gate, and `participants` on
/// dedicated 128-byte lines.
///
/// The 64-bit `generation` carries both flags and a monotonic phase counter.
/// Bits 0 (shutdown) and 1 (cancel) are reserved; the producer increments by 4
/// per published job so the high 62 bits act as the ABA-free phase counter. A
/// 32-bit phase would be at risk of wrapping under sustained dispatch; 64 bits
/// is overkill but free given the cache-line padding.
///
/// The `futexWord` is parking-only: workers re-check `generation` after every
/// wait return, so spurious or duplicated wakes are correctness-neutral.
/// Updates use `relaxed` atomics; the happens-before chain runs through
/// `generation` (release) instead.
///
/// `activeJob` is published with `release`; observed with `acquire`. The slot
/// is `nullptr` until a primitive publishes a `JobDescriptor`; the engine
/// itself never writes here.
struct PoolControl {
  /// Bit flag in `generation` indicating the pool has been told to shut down.
  ///
  /// Set once by the destructor's `fetch_or`; never cleared. Workers observing
  /// this exit the loop.
  static constexpr std::uint64_t kShutdownBit = 1ULL << 0;

  /// Bit flag in `generation` reserved for global cancellation broadcasts.
  ///
  /// Reserved for pool-wide cancellation; the bit lives here so the
  /// `generation` layout is stable once the cancellation path lands without
  /// needing to shuffle the flag-bit assignments.
  static constexpr std::uint64_t kCancelBit = 1ULL << 1;

  /// Bit set by a worker on its `mailbox` line to acknowledge dispatch
  /// completion.
  ///
  /// Same-line ack protocol: the producer publishes the new phase with this bit
  /// clear; the worker stamps `mailbox = phase | kDoneBit` after running its
  /// share. The producer's join reads the worker's mailbox (the same line it
  /// published to) and waits for the DONE bit to appear. Removes the separate
  /// `doneEpoch` cache-line transit on the hot path.
  ///
  /// Lives in the bit-1 slot that was reserved for cancel broadcasts. The
  /// cancel path is carried by `CancellationToken`, not by a generation/mailbox
  /// flag, so the bit was free.
  static constexpr std::uint64_t kDoneBit = 1ULL << 1;

  /// Bit set by the producer on the worker's `mailbox` when this dispatch
  /// reuses the previous dispatch's typed-runner cached parameters
  /// (same-command reuse fast path). When the worker observes this bit, it
  /// skips reading desc fields entirely and uses its TLS-cached (HintsT, F) job
  /// parameters. Producer sets it only when:
  ///   1. Hints opt-in (StaticUniform balance, no cancellation, nothrow body,
  ///   lvalue F)
  ///   2. The current dispatch's key matches the producer's TLS-cached key
  ///   3. Low-latency scope is active (so workers spin and observe the bit
  ///   promptly)
  static constexpr std::uint64_t kReuseBit = 1ULL << 2;

  /// Bit set by the producer on low-latency dispatches where every worker has
  /// already acknowledged hot-spin mode and the fanout is large enough that
  /// per-rank cold-collapse ownership CAS traffic costs more than it can save.
  /// Workers that observe this bit skip the `claimedAt` CAS and stamp DONE with
  /// a plain release store.
  static constexpr std::uint64_t kSkipClaimBit = 1ULL << 3;

  /// Bit set by the worker on its mailbox after every cold-collapse-eligible
  /// dispatch's release-store, regardless of whether its own CAS won the race
  /// against the producer's cold-collapse self-stamp or lost it. The next
  /// dispatch's publish path waits for this bit on every cold-stamped slot
  /// before overwriting `mailboxDesc`, ensuring the worker's reads of the prior
  /// dispatch's `JobDescriptor` happen-before this dispatch's writes to the
  /// same stack-frame address.
  static constexpr std::uint64_t kAckedBit = 1ULL << 4;

  /// Number of low bits reserved for flags; producer increments `generation` by
  /// `1 << kPhaseShift`. Bits: 0=shutdown, 1=done (worker->producer ack),
  /// 2=reuse (producer->worker hot-path hint), 3=skip cold-collapse claim on
  /// hot large fanouts, 4=worker-acked-prior-desc (next-publish gate).
  static constexpr std::uint64_t kPhaseShift = 5;

  /// Increment applied per published phase so flags survive the bump.
  static constexpr std::uint64_t kPhaseStep = 1ULL << kPhaseShift;

  /// Mask of all flag bits below the phase counter.
  static constexpr std::uint64_t kFlagMask = kPhaseStep - 1;

  /// Source-of-truth phase counter.
  ///
  /// Bit 0 = shutdown, bit 1 = cancel-broadcast, bits 2..63 = monotonic phase.
  /// Producer publishes a new phase via `release`; workers read with `acquire`.
  /// Together with `activeJob` this is the acquire/release pair that orders
  /// descriptor visibility. `activeJob` is co-located on the same cache line so
  /// the worker's first acquire-load of `generation` also pulls in the new
  /// descriptor pointer with one cache-line transit.
  alignas(kCacheLine) std::atomic<std::uint64_t> generation{0};

  /// Descriptor pointer published alongside each new generation.
  ///
  /// Co-located with `generation` on the same cache line: the producer writes
  /// both in program order (`activeJob` first, then `generation`), and on x86
  /// TSO the worker's acquire-load of `generation` synchronizes-with both
  /// stores in a single cache-line fetch. The slot is cleared to `nullptr` once
  /// in `shutdownAndJoin` (BEFORE the shutdown bit is set on `generation`) so
  /// worker `shouldExit` semantics are preserved without per-dispatch clears.
  std::atomic<void *> activeJob{nullptr};

  /// 32-bit parking token used as the futex address.
  ///
  /// Updates are relaxed: the futex word identifies the wait queue and gates
  /// re-entry into the kernel; happens-before runs through `generation`'s
  /// release/acquire pair instead. ABA-safe by construction because callers
  /// re-read `generation` after a wake before assuming a new job landed.
  alignas(kCacheLine) std::atomic<std::uint32_t> futexWord{0};

  /// Active low-latency scopes that keep workers spinning instead of parking.
  ///
  /// When non-zero, idle workers re-enter their spin loop after the normal spin
  /// budget instead of calling into futex wait. Producers may skip the
  /// per-dispatch futex wake while this gate is set because a scope transition
  /// wakes parked workers once before hot dispatch begins.
  alignas(kCacheLine) std::atomic<std::uint32_t> hotSpinDepth{0};

  /// Monotonic epoch bumped when entering low-latency mode so workers can
  /// acknowledge readiness.
  alignas(kCacheLine) std::atomic<std::uint64_t> hotSpinEpoch{0};

  /// Number of participants the pool was constructed with (producer +
  /// background workers).
  ///
  /// Read by every worker but never modified after construction; placed on its
  /// own line so reads never share a cache line with the contended atomics
  /// above.
  alignas(kCacheLine) std::uint32_t participants = 0;

  /// Pre-computed bitmask of background-worker slots `[1, participants)` for
  /// the join's
  ///        pending set; producer slot 0 already cleared.
  ///
  /// Constant for the pool's lifetime (set once at construction). Co-located on
  /// the `participants` cache line so the producer's dispatch picks both fields
  /// from a single cache-line fetch instead of reaching into `ThreadPool`'s own
  /// member layout.
  std::uint64_t pendingMaskBits = 0;
};

/// Pool-level relaxed-atomic counters for diagnostics.
///
/// Three monotonic pool-scoped counters incremented at the dispatch publish,
/// inline-fallback, and cancellation-observed sites. Worker-scoped counters
/// (futex parks/wakes, steal attempts) live on `WorkerState` and are aggregated
/// into `PoolCountersSnapshot` by `snapshotCounters()`.
///
/// Compile-time gated by `CITOR_ENABLE_POOL_COUNTERS`. When the macro is
/// undefined (the default), the struct has no atomic members and the increment
/// sites compile to no-ops; the dispatch hot path pays zero extra atomics. When
/// defined, each increment is `relaxed` and the struct is on its own cache line
/// so the counter line never bounces with `PoolControl`'s contended atomics.
/// Reset is not provided; counters are cumulative for the pool's lifetime.
#ifdef CITOR_ENABLE_POOL_COUNTERS
/// Pool-scoped diagnostic counters; populated when
/// `CITOR_ENABLE_POOL_COUNTERS` is defined.
struct alignas(kCacheLine) PoolCounters {
  /// Producer-side dispatches that reached the worker fan-out path (one
  /// increment per published generation).
  std::atomic<std::uint64_t> dispatches{0};

  /// Calls that hit the `runInline` short-circuit (single participant, range
  /// too small per `minTaskUs`, cross-arena guard, or empty range).
  std::atomic<std::uint64_t> inlineFallbacks{0};

  /// Producer observed a cancellation request before fan-out and skipped the
  /// body.
  std::atomic<std::uint64_t> cancellationStops{0};
};
#define CITOR_COUNTERS_INC(member)                                             \
  do {                                                                         \
    m_counters.member.fetch_add(1, std::memory_order_relaxed);                 \
  } while (0)
#else
/// Empty stub used when `CITOR_ENABLE_POOL_COUNTERS` is undefined; the
/// member is zero-sized and every increment site compiles to a no-op.
struct PoolCounters {};
#define CITOR_COUNTERS_INC(member)                                             \
  do {                                                                         \
  } while (0)
#endif

/// Snapshot POD returned by `ThreadPool::snapshotCounters()`. Pool-scoped
/// fields come from `PoolCounters`; worker-scoped fields are aggregated by
/// summing the matching field across every `WorkerState`. Each load is
/// `relaxed` so values may not reflect a single point in time.
struct PoolCountersSnapshot {
  /// Producer dispatches that reached fan-out (matches
  /// `PoolCounters::dispatches`).
  std::uint64_t dispatches = 0;
  /// `runInline` short-circuits (matches `PoolCounters::inlineFallbacks`).
  std::uint64_t inlineFallbacks = 0;
  /// Producer-observed cancellation stops (matches
  /// `PoolCounters::cancellationStops`).
  std::uint64_t cancellationStops = 0;
  /// Sum of `WorkerState::parks` across workers (worker-side
  /// `FUTEX_WAIT_PRIVATE` invocations).
  std::uint64_t futexParks = 0;
  /// Sum of `WorkerState::wakes` across workers (worker-side futex returns).
  std::uint64_t futexWakes = 0;
  /// Sum of `WorkerState::stealAttempts` across workers (forkJoin steal
  /// probes).
  std::uint64_t stealAttempts = 0;
  /// Sum of `WorkerState::stealSuccesses` across workers (forkJoin steals that
  /// found work).
  std::uint64_t stealSuccesses = 0;
};

} // namespace citor::detail

// ===== citor/detail/reduce_tree.h =====


namespace citor::detail {

/// Combine an array of partial values via a chunk-id pairwise reduction tree.
///
/// The combine order is the deterministic pairwise tree on chunk ids (NOT
/// worker ids):
///
/// - Step 1 combines `(0,1), (2,3), (4,5), ...`
/// - Step 2 combines `(0,2), (4,6), ...`
/// - The doubling step continues until a single value remains in `partials[0]`.
///
/// If the number of partials is not a power of two, the surplus right-hand
/// element is carried forward to the next step unchanged (i.e., interior nodes
/// always combine two siblings, leaves with no sibling get reused as their own
/// subtree partial). This preserves the Demmel-Nguyen (TOMS 2014)
/// bit-reproducibility property: the tree shape is `n`-determined and each
/// interior node has fixed left/right operands, so the output is bit-identical
/// regardless of which worker computed which leaf.
///
/// T       Partial value type (e.g. `double`, `KahanPair`).
/// Combine Binary combiner; called as `combine(left, right)` and must return a
/// `T`. partials In-place workspace; mutated as the tree collapses upward.
/// combine  Combiner function.
/// The fully combined partial covering every chunk; matches `partials.front()`
/// after the
///         call. Returns a default-constructed `T` when |partials| is empty.
template <class T, class Combine>
[[nodiscard]] T pairwiseTreeCombine(std::vector<T> &partials, Combine combine) {
  if (partials.empty()) {
    return T{};
  }
  std::size_t step = 1;
  while (step < partials.size()) {
    const std::size_t stride = step * 2;
    for (std::size_t i = 0; i + step < partials.size(); i += stride) {
      partials[i] = combine(partials[i], partials[i + step]);
    }
    step = stride;
  }
  return std::move(partials[0]);
}

} // namespace citor::detail

// ===== citor/detail/scan_state.h =====



namespace citor::detail {

/// Per-chunk completion slot used by the Blelloch two-pass `parallelScan`
/// rendezvous.
///
/// Each slot lives on its own `citor::kCacheLine` -sized line so a worker's
/// release-store on `done` for one chunk cannot invalidate a neighbouring slot
/// the producer or downstream worker is acquiring. The owning worker writes
/// `done = 1` once after its pass-1 partial-sum write and `done = 2` once after
/// its pass-2 final-output write; downstream readers use acquire-loads to
/// confirm the upstream worker's pass-1 partials are visible before the
/// sequential reduce, and the producer's pass-2 join uses the same per-slot
/// epoch ladder.
///
/// Padding-suppression note: the layout keeps the atomic on its own
/// `kCacheLine`-sized line, so the analyser's "excessive padding" warning is
/// the design trade-off we want -- false-sharing avoidance over byte-tight
/// packing.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct alignas(kCacheLine) ScanDoneSlot {
  /// Pass epoch the worker last completed for its chunk. `0` means "not yet
  /// started", `1` means "pass-1 partial written and visible", `2` means
  /// "pass-2 output written".
  std::atomic<std::uint64_t> done{0};
};

/// Stack-resident state shared by the producer and background workers across
/// both passes of a
///        single `parallelScan` call.
///
/// Layout invariants:
/// - `scanCancelled` lives on its own line so cancellation broadcast does not
/// interfere with the
///   producer's hot path.
/// - `firstException` lives on its own line so exception capture does not
/// invalidate the per-chunk
///   done slots on the worker write path.
/// - `done[c]` lives on a dedicated line via `ScanDoneSlot` so adjacent chunks'
/// release-stores
///   do not contend for the same cache line.
///
/// The state itself lives on the producer's stack across a `parallelScan` call.
/// The trailing per-chunk `done` slots are NOT owned by `ScanState`: the pool
/// pre-allocates a contiguous `ChainDoneSlot` block once at construction time
/// (sized `participants()`), and every scan call borrows that block via
/// `doneSlots`, reinterpreting each `ChainDoneSlot::done` as a
/// `ScanDoneSlot::done` epoch ladder. The producer zero-resets each slot at
/// entry to honour the scan's "fresh epoch per call" contract; this avoids
/// `operator new` / `operator delete` on the dispatch hot path.
///
/// The rendezvous between Pass 1 and the sequential reduce is fully
/// decentralized: every slot stamps `done[slot] = 1` (release) after writing
/// its partial, and the producer (slot 0) waits until every slot has reached
/// `done >= 1` before computing exclusive prefixes. The rendezvous between the
/// sequential reduce and Pass 2 is implicit: the producer's release-store on
/// `prefixesPublished` synchronizes with each worker's acquire-load before that
/// worker reads `partials[slot]` as the seed for its Pass-2 body invocation.
///
/// The cancellation handshake is encoded by stamping `done = 2` on the
/// cancelled slot's line, which satisfies any active pass's wait condition
/// unconditionally and lets the spin loop drop a per-iteration `scanCancelled`
/// poll, mirroring the chain's `nStages` stamping idiom in `chain_state.h`.
///
/// Padding-suppression note: the layout keeps every contended atomic on its own
/// `kCacheLine`-sized line, so the analyser's "excessive padding" warning is
/// the design trade-off we want -- false-sharing avoidance over byte-tight
/// packing.
///
/// T Reduction value type the scan operates on.
template <class T>
struct ScanState {
  /// Number of participants (= number of chunks) collaborating in the scan.
  std::uint32_t participants = 0;

  /// Row-range upper bound passed in by the caller; used for chunk
  /// partitioning.
  std::size_t n = 0;

  /// Cancellation flag flipped by the producer's cancellation observer.
  ///
  /// Worker bodies check this between passes and exit cleanly when set;
  /// cancelled slots stamp `done = 2` so peers waiting on `done >= target` for
  /// any active pass proceed without needing to poll this flag inside the spin
  /// loop. The flag is release-written by the slot that observed the
  /// cancellation, and acquire-read by other slots at pass boundaries.
  alignas(kCacheLine) std::atomic<std::uint32_t> scanCancelled{0};

  /// First-exception capture slot shared across all participants.
  ///
  /// Workers `compare_exchange` this from null to a heap-allocated
  /// `std::exception_ptr` to record the first failure deterministically;
  /// subsequent throws drop. The producer reads the slot after joining and
  /// rethrows if non-null. Allocation only happens on the cold throw path.
  alignas(kCacheLine) std::atomic<std::exception_ptr *> firstException{nullptr};

  /// Producer-side flag flipped after the sequential reduce computes every
  /// chunk's exclusive
  ///        prefix.
  ///
  /// Workers acquire-spin on this between passes; the release-store from the
  /// producer publishes the `partials` array (re-purposed to hold exclusive
  /// prefixes after the reduce) so every worker's pass-2 body sees the seed for
  /// its chunk.
  alignas(kCacheLine) std::atomic<std::uint32_t> prefixesPublished{0};

  /// Per-chunk partial / exclusive-prefix slot.
  ///
  /// After Pass 1, `partials[c]` holds chunk `c`'s partial sum. After the
  /// sequential reduce, `partials[c]` holds chunk `c`'s exclusive prefix (the
  /// seed each Pass-2 invocation feeds back into the body's `initial`
  /// argument). Sized `participants` valid elements.
  T *partials = nullptr;

  /// Borrowed pointer to the pool's pre-allocated per-worker completion slots.
  ///
  /// The pool owns the storage; the scan call reserves a fresh interval
  /// `[epochBase, epochBase + 2]` in the pool's monotonically-advancing epoch
  /// counter so successive calls observe disjoint targets without
  /// zero-resetting the slots. Sized `participants` valid elements; reading
  /// past that index is undefined.
  ChainDoneSlot *doneSlots = nullptr;

  /// Per-call base of the pool's monotonic done-epoch counter.
  ///
  /// Stamps are absolute: `done = epochBase + 1` after Pass 1, `done =
  /// epochBase + 2` after Pass 2. Waits compare against `epochBase + 1` or
  /// `epochBase + 2`. The producer reserves the interval under the dispatch
  /// gate before publishing, so prior-dispatch values cannot satisfy a current
  /// wait. Cancellation stamps `epochBase + 2` so peers waiting on either pass
  /// advance.
  std::uint64_t epochBase = 0;

  /// Borrowed pointer to the pool's per-slot CCD index array.
  ///
  /// Set when the call enables CCD-aware asymmetric chunk partitioning;
  /// nullptr otherwise. Used by `slotRange` to give larger chunks to slots on
  /// the producer's CCD and smaller chunks to slots on a different CCD,
  /// reducing the cross-CCD coherence volume on Pass-2 writes.
  const std::uint32_t *ccdOfSlot = nullptr;

  /// CCD identity of the producer (slot 0). When asymmetric partitioning is
  /// enabled, slots whose `ccdOfSlot[s] == producerCcd` are the
  /// "producer-CCD" slots and receive a larger share of `n`.
  std::uint32_t producerCcd = UINT32_MAX;

  /// Numerator (out of 16) of `n` allocated to the producer-CCD slot group as
  /// a whole. The cross-CCD slot group receives `(16 - asymmetricNum) / 16`.
  /// Used only when `ccdOfSlot != nullptr`. Shared evenly within each group.
  /// The declarator default matches the pool's initial value so the field
  /// stays in a consistent neutral state until the per-call setup overwrites
  /// it.
  std::uint32_t asymmetricNum = 8;

  /// Number of slots on the producer's CCD. Precomputed by the caller to
  /// avoid a per-`slotRange()` walk over `ccdOfSlot`.
  std::uint32_t slotsOnProducerCcd = 0;

  /// Probe-derived per-slot cluster id; `clusterIdOfSlot[s]` indexes a
  /// `[0, numClusters)` cluster. Set when the pool's coherence probe
  /// returns `numClusters >= 2` and the scan call opts into the
  /// hierarchical algorithm. Nullptr when the call uses the
  /// single-cluster reduce path.
  const std::uint32_t *clusterIdOfSlot = nullptr;

  /// Number of distinct clusters in this scan call. Zero when the
  /// hierarchical path is disabled.
  std::uint32_t numClusters = 0;

  /// Per-cluster slot ranges precomputed by the caller. `clusterFirstSlot[c]`
  /// is the lowest slot index in cluster `c`; `clusterSlotCount[c]` is the
  /// count of slots in cluster `c`. The caller arranges that each cluster's
  /// slots are contiguous in slot-index order, so cluster k's slots are
  /// `[clusterFirstSlot[k], clusterFirstSlot[k] + clusterSlotCount[k])`.
  /// Nullptr when the hierarchical path is disabled.
  const std::uint32_t *clusterFirstSlot = nullptr;
  /// Companion to `clusterFirstSlot`. `clusterSlotCount[c]` is the number
  /// of slots in cluster `c`. Nullptr when the hierarchical path is
  /// disabled.
  const std::uint32_t *clusterSlotCount = nullptr;

  /// Per-cluster total / exclusive prefix slots. Each lives on its own
  /// cache line. `clusterTotals[k]` is written by cluster k's leader
  /// after its local reduce; `clusterPrefixes[k]` is the cross-cluster
  /// exclusive prefix the producer writes back. Both are sized
  /// `numClusters`. Nullptr when the hierarchical path is disabled.
  T *clusterTotals = nullptr;
  /// Per-cluster cross-cluster exclusive prefixes the producer writes back
  /// after the cluster reduce. Sized `numClusters`. Nullptr when the
  /// hierarchical path is disabled.
  T *clusterPrefixes = nullptr;

  /// Subscript a slot by index.
  ///
  /// The slot itself owns mutable atomics; the accessor is `const` because
  /// reading from the borrowed pointer does not modify the owning state, and
  /// the returned slot's atomics carry their own internal mutability.
  ///
  /// idx Slot index in `[0, participants)`.
  /// Reference to the slot.
  [[nodiscard]] ChainDoneSlot &doneSlot(std::size_t idx) const noexcept {
    return doneSlots[idx];
  }

  /// Compute a slot's contiguous row range over `[0, n)`.
  ///
  /// Uniform mode (`ccdOfSlot == nullptr`): `lo = (n * slot) / participants`,
  /// `hi = (n * (slot + 1)) / participants`, matching the chain / plex
  /// convention so the scan's chunk identity is bit-stable across worker
  /// counts at fixed `n`.
  ///
  /// Asymmetric mode (`ccdOfSlot != nullptr`): producer-CCD slots receive a
  /// larger contiguous prefix of `n` (`asymmetricNum / 16`), cross-CCD slots
  /// receive the trailing remainder. Within each group the chunks are
  /// uniform. The producer-CCD slots take the prefix `[0, producerVolume)` so
  /// chunk-id-order over slots is still left-to-right, preserving the
  /// `slot=0` -> `chunk-id=0` invariant the seq-reduce relies on. Whether
  /// this is enabled is decided per-call by `runScanParallel` based on
  /// detected cross-CCD presence; it is opt-in to avoid regressing balanced
  /// compute-bound bodies on single-CCD or homogeneous-CCD topologies.
  ///
  /// slot Worker slot index in `[0, participants)`.
  /// `(lo, hi)` pair denoting the slot's contiguous range over `[0, n)`.
  [[nodiscard]] std::pair<std::size_t, std::size_t>
  slotRange(std::uint32_t slot) const noexcept {
    if (ccdOfSlot == nullptr) {
      const auto lo = static_cast<std::size_t>(mulDiv64(n, slot, participants));
      const auto hi =
          static_cast<std::size_t>(mulDiv64(n, slot + 1U, participants));
      return {lo, hi};
    }
    // Producer-CCD slot group covers the prefix `[0, producerVolume)`;
    // cross-CCD group covers `[producerVolume, n)`.
    const auto producerVolume =
        static_cast<std::size_t>(mulDiv64(n, asymmetricNum, 16U));
    const std::uint32_t numProducer = slotsOnProducerCcd;
    const std::uint32_t numCross = participants - slotsOnProducerCcd;
    // Index of `slot` within its CCD group (0-based, in slot-index order).
    std::uint32_t indexInGroup = 0;
    const bool isProducerCcd = (ccdOfSlot[slot] == producerCcd);
    for (std::uint32_t s = 0; s < slot; ++s) {
      if ((ccdOfSlot[s] == producerCcd) == isProducerCcd) {
        ++indexInGroup;
      }
    }
    if (isProducerCcd && numProducer > 0U) {
      const auto lo = static_cast<std::size_t>(
          mulDiv64(producerVolume, indexInGroup, numProducer));
      const auto hi = static_cast<std::size_t>(
          mulDiv64(producerVolume, indexInGroup + 1U, numProducer));
      return {lo, hi};
    }
    if (numCross > 0U) {
      const std::size_t crossVolume = n - producerVolume;
      const std::size_t lo =
          producerVolume + static_cast<std::size_t>(
                               mulDiv64(crossVolume, indexInGroup, numCross));
      const std::size_t hi =
          producerVolume + static_cast<std::size_t>(mulDiv64(
                               crossVolume, indexInGroup + 1U, numCross));
      return {lo, hi};
    }
    return {0U, 0U};
  }
};

} // namespace citor::detail

// ===== citor/detail/topology.h =====


#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace citor::detail {

#ifdef _WIN32

/// Walk every `SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX` record for
/// `rel` and pass each to `f`. Returns `true` only when the entire
/// buffer was walked. Centralises the probe-size / allocate / fetch /
/// variable-stride scan that `RelationProcessorCore` and `RelationCache` share.
template <class F>
inline bool walkLogicalProcessorInfoEx(LOGICAL_PROCESSOR_RELATIONSHIP rel,
                                       F &&f) {
  DWORD length = 0;
  // First call is expected to fail with ERROR_INSUFFICIENT_BUFFER and set the
  // required size in `length`. Any other failure shape (rel not supported,
  // truncated query) means we cannot probe topology and the caller should fall
  // back.
  if (::GetLogicalProcessorInformationEx(rel, nullptr, &length) != FALSE) {
    return false;
  }
  if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    return false;
  }
  std::vector<unsigned char> buffer(length);
  if (::GetLogicalProcessorInformationEx(
          rel,
          reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
              buffer.data()),
          &length) == FALSE) {
    return false;
  }
  unsigned char *p = buffer.data();
  unsigned char *const end = p + length;
  while (p < end) {
    auto *info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(p);
    if (info->Size == 0U || p + info->Size > end) {
      return false;
    }
    f(*info);
    p += info->Size;
  }
  return true;
}

/// Lift a `GROUP_AFFINITY` into the flat logical-CPU-id space. Single
/// processor group only; multi-group hosts (>64 logical CPUs) require
/// `SetThreadGroupAffinity`, which the pool does not yet emit.
inline std::vector<std::uint32_t>
expandGroupAffinity(const GROUP_AFFINITY &ga) {
  std::vector<std::uint32_t> result;
  KAFFINITY mask = ga.Mask;
  const std::uint32_t base = static_cast<std::uint32_t>(ga.Group) * 64U;
  for (std::uint32_t bit = 0; mask != 0U; ++bit, mask >>= 1) {
    if ((mask & 1U) != 0U) {
      result.push_back(base + bit);
    }
  }
  return result;
}

#endif // _WIN32

/// Logical view of the host's CPU topology used for affinity decisions.
///
/// Constructed once via `detectTopology()` at pool startup; never queried on
/// the hot path. The pool uses `physicalCores` for one-worker-per-physical-core
/// pinning, `ccdGroups` and `ccdOfCpu` for CCD-aware victim selection, and the
/// counts for sizing the workers vector when no explicit participant count is
/// supplied. On hosts where sysfs is unavailable the constructor falls back to
/// `std::thread::hardware_concurrency()` and treats every logical CPU as its
/// own physical core inside one synthetic CCD.
struct Topology {
  /// Process-affinity-filtered list of one logical CPU per physical core (the
  /// pinning targets).
  std::vector<std::uint32_t> physicalCores;

  /// Lists of physical-core CPU ids grouped by shared L3 cache (CCD on Zen,
  /// cluster on big.LITTLE).
  std::vector<std::vector<std::uint32_t>> ccdGroups;

  /// CCD index for each logical CPU id; `ccdOfCpu[id]` indexes `ccdGroups`.
  std::vector<std::uint32_t> ccdOfCpu;

  /// L3 cache size in KiB for each CCD; `l3KibOfCcd[i]` corresponds to
  /// `ccdGroups[i]`. Read from
  /// `/sys/devices/system/cpu/cpuN/cache/index3/size` once per probe; zero when
  /// sysfs is absent.
  std::vector<std::uint64_t> l3KibOfCcd;

  /// Per-core L2 cache size in KiB, sampled from
  /// `cache/index2/size` of the first physical-core CPU id once per probe.
  /// Per-core L2 is uniform on every supported microarchitecture today,
  /// so one sample suffices; heterogeneous chips will need a per-cluster
  /// probe later. Used by primitives that pick tile sizes from the
  /// runtime cache hierarchy instead of hardcoded constants. Zero when
  /// sysfs is absent; primitives fall back to a conservative default.
  std::uint64_t l2KibPerCore = 0;

  /// Index into `ccdGroups` of the preferred CCD for small pools that fit in a
  /// single L3. Picks the largest L3 (= 3D V-Cache CCD on V-Cache parts, where
  /// one CCD has a stacked SRAM die); breaks ties by lowest index so the choice
  /// is deterministic across runs.
  std::uint32_t preferredCcd = 0;

  /// Total logical CPU count reported by the OS.
  std::uint32_t logicalCount = 0;

  /// Number of physical cores in the process affinity mask.
  std::uint32_t physicalCount = 0;

  /// Number of distinct CCDs (or shared-L3 groups).
  std::uint32_t ccdCount = 0;

  /// SMT sibling of each logical CPU id, or `UINT32_MAX` when the core
  /// is single-threaded or the OS did not report siblings. SMT4 silicon
  /// records the first non-self entry deterministically. The placement
  /// rule reads this to route slot 1 onto the producer's SMT sibling.
  std::vector<std::uint32_t> smtSiblingOfCpu;
};

/// Read a sysfs cache size string like "32768K" or "96M" and convert to KiB.
/// Returns 0 on parse failure or missing file so the caller can fall back to
/// "unknown size".
inline std::uint64_t readCacheSizeKib(const std::string &path) noexcept {
  std::ifstream in(path);
  if (!in.is_open()) {
    return 0U;
  }
  std::string token;
  if (!(in >> token) || token.empty()) {
    return 0U;
  }
  std::uint64_t value = 0U;
  std::size_t i = 0;
  while (i < token.size() &&
         std::isdigit(static_cast<unsigned char>(token[i])) != 0) {
    value = (value * 10U) + static_cast<std::uint64_t>(token[i] - '0');
    ++i;
  }
  if (i == 0U) {
    return 0U;
  }
  if (i < token.size()) {
    const char unit = token[i];
    if (unit == 'M' || unit == 'm') {
      value *= 1024U;
    } else if (unit == 'G' || unit == 'g') {
      value *= std::uint64_t{1024} * 1024U;
    }
    // 'K'/'k' or unknown unit: leave value as-is (sysfs convention is KiB by
    // default).
  }
  return value;
}

/// Read a comma-separated CPU list (e.g. `0-7,16-23`) from a sysfs file.
///
/// Used for both `thread_siblings_list` (SMT detection) and
/// `cache/index3/shared_cpu_list` (CCD detection). Returns an empty vector when
/// the file is absent so the caller can fall back to a conservative default;
/// missing sysfs entries are common in containers and CI runners.
///
/// path Absolute path to a sysfs file containing the cpu-list-format string.
/// Sorted, deduplicated list of CPU ids; empty when the file cannot be read.
inline std::vector<std::uint32_t> readCpuList(const std::string &path) {
  std::vector<std::uint32_t> result;
  std::ifstream in(path);
  if (!in.is_open()) {
    return result;
  }
  std::string line;
  if (!std::getline(in, line)) {
    return result;
  }
  std::stringstream ss(line);
  std::string token;
  while (std::getline(ss, token, ',')) {
    if (token.empty()) {
      continue;
    }
    const auto dash = token.find('-');
    if (dash == std::string::npos) {
      try {
        result.push_back(static_cast<std::uint32_t>(std::stoul(token)));
      } catch (const std::exception &) {
        // Malformed token; skip rather than throwing from a topology probe.
        continue;
      }
    } else {
      try {
        const auto first =
            static_cast<std::uint32_t>(std::stoul(token.substr(0, dash)));
        const auto last =
            static_cast<std::uint32_t>(std::stoul(token.substr(dash + 1)));
        for (std::uint32_t cpu = first; cpu <= last; ++cpu) {
          result.push_back(cpu);
        }
      } catch (const std::exception &) {
        // Malformed range; skip the token.
        continue;
      }
    }
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

#ifdef _WIN32
/// Windows-side topology probe. Uses `GetLogicalProcessorInformationEx` for
/// SMT-sibling discovery (`RelationProcessorCore`), shared-L3 groups
/// (`RelationCache` filtered to `Level == 3`), and cache sizes; uses
/// `GetProcessAffinityMask` for the allowed-CPU set. Falls back to a single
/// synthetic CCD covering every allowed CPU when the OS rejects the query
/// (e.g. inside a container that masked off the API).
inline Topology detectTopologyWindows() {
  Topology topo;
  topo.logicalCount = std::thread::hardware_concurrency();
  if (topo.logicalCount == 0U) {
    topo.logicalCount = 1U;
  }
  topo.ccdOfCpu.assign(topo.logicalCount, 0U);
  topo.smtSiblingOfCpu.assign(topo.logicalCount, UINT32_MAX);

  std::vector<std::uint32_t> allowed;
  allowed.reserve(topo.logicalCount);
  {
    DWORD_PTR procMask = 0;
    DWORD_PTR sysMask = 0;
    if (::GetProcessAffinityMask(::GetCurrentProcess(), &procMask, &sysMask) !=
        FALSE) {
      const std::uint32_t bits =
          static_cast<std::uint32_t>(sizeof(DWORD_PTR) * 8U);
      const std::uint32_t scanLimit =
          topo.logicalCount < bits ? topo.logicalCount : bits;
      for (std::uint32_t cpu = 0; cpu < scanLimit; ++cpu) {
        if ((procMask & (static_cast<DWORD_PTR>(1) << cpu)) != 0U) {
          allowed.push_back(cpu);
        }
      }
    }
  }
  if (allowed.empty()) {
    allowed.reserve(topo.logicalCount);
    for (std::uint32_t cpu = 0; cpu < topo.logicalCount; ++cpu) {
      allowed.push_back(cpu);
    }
  }

  // Build a CPU -> SMT-sibling list and pick one CPU per physical core.
  // `RelationProcessorCore` records have `GroupMask[0].Mask` set to the
  // logical-CPU bitset of the physical core's siblings.
  std::vector<std::vector<std::uint32_t>> smtSiblings(topo.logicalCount);
  (void)walkLogicalProcessorInfoEx(
      RelationProcessorCore,
      [&](const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX &info) {
        if (info.Relationship != RelationProcessorCore) {
          return;
        }
        const auto &core = info.Processor;
        if (core.GroupCount == 0U) {
          return;
        }
        const std::vector<std::uint32_t> cpus =
            expandGroupAffinity(core.GroupMask[0]);
        for (const std::uint32_t cpu : cpus) {
          if (cpu < smtSiblings.size()) {
            smtSiblings[cpu] = cpus;
          }
        }
      });
  // Record the "other" sibling per CPU. On SMT4 silicon (>2 reported
  // siblings) we pick the first non-self entry deterministically.
  for (std::uint32_t cpu = 0; cpu < topo.logicalCount; ++cpu) {
    const auto &sibs = smtSiblings[cpu];
    for (const std::uint32_t s : sibs) {
      if (s != cpu) {
        topo.smtSiblingOfCpu[cpu] = s;
        break;
      }
    }
  }

  std::vector<bool> consumed(topo.logicalCount, false);
  for (const std::uint32_t cpu : allowed) {
    if (cpu >= consumed.size() || consumed[cpu]) {
      continue;
    }
    consumed[cpu] = true;
    topo.physicalCores.push_back(cpu);
    if (cpu < smtSiblings.size()) {
      for (const std::uint32_t sib : smtSiblings[cpu]) {
        if (sib < consumed.size()) {
          consumed[sib] = true;
        }
      }
    }
  }
  if (topo.physicalCores.empty()) {
    topo.physicalCores = allowed;
  }
  topo.physicalCount = static_cast<std::uint32_t>(topo.physicalCores.size());

  // Per-CPU L3 shared-CPU list + L3 size from `RelationCache` filtered to
  // `Level == 3`. Per-core L2 sampled the same way at `Level == 2`. Caches
  // whose `GroupCount > 1` (split across processor groups) are skipped;
  // pools spanning multiple groups would need `SetThreadGroupAffinity`.
  std::vector<std::vector<std::uint32_t>> l3SharedByCpu(topo.logicalCount);
  std::vector<std::uint64_t> l3SizeKibByCpu(topo.logicalCount, 0U);
  std::vector<std::uint64_t> l2SizeKibByCpu(topo.logicalCount, 0U);
  (void)walkLogicalProcessorInfoEx(
      RelationCache, [&](const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX &info) {
        if (info.Relationship != RelationCache) {
          return;
        }
        const auto &cache = info.Cache;
        if (cache.Level != 2 && cache.Level != 3) {
          return;
        }
        if (cache.GroupCount == 0U) {
          return;
        }
        const std::vector<std::uint32_t> cpus =
            expandGroupAffinity(cache.GroupMasks[0]);
        const std::uint64_t sizeKib =
            static_cast<std::uint64_t>(cache.CacheSize) / 1024U;
        if (cache.Level == 3) {
          for (const std::uint32_t cpu : cpus) {
            if (cpu < l3SharedByCpu.size()) {
              l3SharedByCpu[cpu] = cpus;
              l3SizeKibByCpu[cpu] = sizeKib;
            }
          }
        } else { // Level == 2
          for (const std::uint32_t cpu : cpus) {
            if (cpu < l2SizeKibByCpu.size()) {
              l2SizeKibByCpu[cpu] = sizeKib;
            }
          }
        }
      });

  // Group physical cores by shared L3.
  std::vector<bool> assigned(topo.logicalCount, false);
  for (const std::uint32_t cpu : topo.physicalCores) {
    if (cpu < assigned.size() && assigned[cpu]) {
      continue;
    }
    std::vector<std::uint32_t> shared = cpu < l3SharedByCpu.size()
                                            ? l3SharedByCpu[cpu]
                                            : std::vector<std::uint32_t>{};
    if (shared.empty()) {
      shared.push_back(cpu);
    }
    std::vector<std::uint32_t> physicalInGroup;
    physicalInGroup.reserve(shared.size());
    for (const std::uint32_t sharedCpu : shared) {
      const auto it = std::find(topo.physicalCores.begin(),
                                topo.physicalCores.end(), sharedCpu);
      if (it != topo.physicalCores.end()) {
        physicalInGroup.push_back(sharedCpu);
        if (sharedCpu < assigned.size()) {
          assigned[sharedCpu] = true;
        }
      }
    }
    if (physicalInGroup.empty()) {
      physicalInGroup.push_back(cpu);
      if (cpu < assigned.size()) {
        assigned[cpu] = true;
      }
    }
    const auto ccdIndex = static_cast<std::uint32_t>(topo.ccdGroups.size());
    for (const std::uint32_t physCpu : physicalInGroup) {
      if (physCpu < topo.ccdOfCpu.size()) {
        topo.ccdOfCpu[physCpu] = ccdIndex;
      }
    }
    topo.ccdGroups.push_back(std::move(physicalInGroup));
  }
  topo.ccdCount = static_cast<std::uint32_t>(topo.ccdGroups.size());

  // Per-CCD L3 size from the per-CPU map.
  topo.l3KibOfCcd.assign(topo.ccdGroups.size(), 0U);
  for (std::size_t ccd = 0; ccd < topo.ccdGroups.size(); ++ccd) {
    if (topo.ccdGroups[ccd].empty()) {
      continue;
    }
    const std::uint32_t rep = topo.ccdGroups[ccd].front();
    if (rep < l3SizeKibByCpu.size()) {
      topo.l3KibOfCcd[ccd] = l3SizeKibByCpu[rep];
    }
  }
  if (!topo.physicalCores.empty()) {
    const std::uint32_t first = topo.physicalCores.front();
    if (first < l2SizeKibByCpu.size()) {
      topo.l2KibPerCore = l2SizeKibByCpu[first];
    }
  }
  // Two-step `preferredCcd` pick: a largest-L3 CCD wider than 1.5x
  // the next-largest wins outright (catches V-Cache at 3x without
  // firing on uniform L3); otherwise pick the largest L3 CCD that
  // does not contain the BSP, since both kernels bias DPC / IRQ /
  // kthread placement onto the BSP's CCD. Fall back to the absolute
  // largest L3 when only one CCD exists.
  std::uint64_t largestKib = 0U;
  std::uint64_t secondKib = 0U;
  std::uint32_t largestIdx = 0;
  for (std::size_t ccd = 0; ccd < topo.l3KibOfCcd.size(); ++ccd) {
    const std::uint64_t k = topo.l3KibOfCcd[ccd];
    if (k > largestKib) {
      secondKib = largestKib;
      largestKib = k;
      largestIdx = static_cast<std::uint32_t>(ccd);
    } else if (k > secondKib) {
      secondKib = k;
    }
  }
  const bool asymmetricL3 =
      secondKib == 0U || (largestKib * 2U > secondKib * 3U);
  if (asymmetricL3) {
    topo.preferredCcd = largestIdx;
  } else {
    const std::uint32_t bspCcd =
        (!topo.ccdOfCpu.empty()) ? topo.ccdOfCpu[0] : UINT32_MAX;
    std::uint64_t bestKib = 0U;
    std::uint32_t bestIdx = UINT32_MAX;
    for (std::size_t ccd = 0; ccd < topo.l3KibOfCcd.size(); ++ccd) {
      if (static_cast<std::uint32_t>(ccd) == bspCcd) {
        continue;
      }
      if (topo.l3KibOfCcd[ccd] > bestKib ||
          (topo.l3KibOfCcd[ccd] == bestKib && bestIdx == UINT32_MAX)) {
        bestKib = topo.l3KibOfCcd[ccd];
        bestIdx = static_cast<std::uint32_t>(ccd);
      }
    }
    topo.preferredCcd = bestIdx == UINT32_MAX ? largestIdx : bestIdx;
  }

  // Reorder each CCD so SMT-capable cores appear first, descending CPU
  // id otherwise. Hybrid Intel parts expose E-cores at higher CPU ids;
  // pure-descending order would place the producer on an E-core, where
  // the slot-1 SMT-sibling routing rule has nothing to attach to. AMD
  // parts have SMT on every core, so the bias is a no-op.
  std::vector<std::uint32_t> reordered;
  reordered.reserve(topo.physicalCores.size());
  for (const auto &group : topo.ccdGroups) {
    std::vector<std::uint32_t> withSibling;
    std::vector<std::uint32_t> withoutSibling;
    withSibling.reserve(group.size());
    withoutSibling.reserve(group.size());
    for (std::size_t i = group.size(); i > 0U; --i) {
      const std::uint32_t cpu = group[i - 1U];
      const bool hasSibling = cpu < topo.smtSiblingOfCpu.size() &&
                              topo.smtSiblingOfCpu[cpu] != UINT32_MAX;
      if (hasSibling) {
        withSibling.push_back(cpu);
      } else {
        withoutSibling.push_back(cpu);
      }
    }
    reordered.insert(reordered.end(), withSibling.begin(), withSibling.end());
    reordered.insert(reordered.end(), withoutSibling.begin(),
                     withoutSibling.end());
  }
  if (reordered.size() == topo.physicalCores.size()) {
    topo.physicalCores = std::move(reordered);
  }
  return topo;
}
#endif // _WIN32

/// Probe the host's CPU topology via sysfs and the process affinity mask.
///
/// The detection sequence:
/// 1. `sched_getaffinity(0, ...)` reads which logical CPUs the process is
/// allowed to use.
/// 2. For each allowed CPU, `topology/thread_siblings_list` selects one logical
/// per physical core.
/// 3. For each chosen physical CPU, `cache/index3/shared_cpu_list` groups them
/// by shared L3.
/// 4. When sysfs is absent the function falls back to `hardware_concurrency()`
/// and a single CCD.
///
/// The returned `Topology` is the source of truth for affinity decisions for
/// the pool's lifetime.
///
/// Populated `Topology`; never throws even if sysfs is unavailable.
inline Topology detectTopology() {
#ifdef _WIN32
  return detectTopologyWindows();
#else
  Topology topo;
  topo.logicalCount = std::thread::hardware_concurrency();
  if (topo.logicalCount == 0) {
    topo.logicalCount = 1;
  }
  topo.ccdOfCpu.assign(topo.logicalCount, 0U);
  topo.smtSiblingOfCpu.assign(topo.logicalCount, UINT32_MAX);

  std::vector<std::uint32_t> allowed;
  allowed.reserve(topo.logicalCount);

#ifdef __linux__
  cpu_set_t mask;
  CPU_ZERO(&mask);
  if (sched_getaffinity(0, sizeof(mask), &mask) == 0) {
    /// Cap the scan at `CPU_SETSIZE` so the fixed-size `cpu_set_t` is never
    /// indexed past its bit range on hosts with more than `CPU_SETSIZE`
    /// logical CPUs. Pools never need more than `physicalCores` workers so
    /// the cap only affects affinity reporting, not scheduling.
    const auto cpuMax = static_cast<std::uint32_t>(CPU_SETSIZE);
    const std::uint32_t scanLimit =
        topo.logicalCount < cpuMax ? topo.logicalCount : cpuMax;
    for (std::uint32_t cpu = 0; cpu < scanLimit; ++cpu) {
      if (CPU_ISSET(static_cast<std::size_t>(cpu), &mask)) {
        allowed.push_back(cpu);
      }
    }
  }
#endif

  if (allowed.empty()) {
    allowed.reserve(topo.logicalCount);
    for (std::uint32_t cpu = 0; cpu < topo.logicalCount; ++cpu) {
      allowed.push_back(cpu);
    }
  }

  // Pick one logical CPU per physical core (skip SMT siblings). Record
  // per-CPU sibling mapping while walking sysfs so callers can route
  // slot 1 to the producer's SMT sibling without re-reading sysfs.
  std::vector<bool> consumed(topo.logicalCount, false);
  for (const std::uint32_t cpu : allowed) {
    if (cpu >= consumed.size() || consumed[cpu]) {
      continue;
    }
    consumed[cpu] = true;
    topo.physicalCores.push_back(cpu);

    const std::string siblingsPath = "/sys/devices/system/cpu/cpu" +
                                     std::to_string(cpu) +
                                     "/topology/thread_siblings_list";
    const std::vector<std::uint32_t> siblings = readCpuList(siblingsPath);
    for (const std::uint32_t sib : siblings) {
      if (sib < consumed.size()) {
        consumed[sib] = true;
      }
    }
    // Stamp both directions of each sibling pair on first sighting; the
    // SMT4 fallback in `smtSiblingOfCpu`'s doc applies if a core reports
    // more than two siblings.
    for (const std::uint32_t sib : siblings) {
      if (sib >= topo.smtSiblingOfCpu.size() || sib == cpu) {
        continue;
      }
      if (topo.smtSiblingOfCpu[cpu] == UINT32_MAX) {
        topo.smtSiblingOfCpu[cpu] = sib;
      }
      if (topo.smtSiblingOfCpu[sib] == UINT32_MAX) {
        topo.smtSiblingOfCpu[sib] = cpu;
      }
    }
  }

  if (topo.physicalCores.empty()) {
    // Fallback: every allowed logical CPU is its own physical core.
    topo.physicalCores = allowed;
  }

  topo.physicalCount = static_cast<std::uint32_t>(topo.physicalCores.size());

  // Group physical cores by shared L3 (CCD on Zen, cluster on heterogeneous
  // SoCs).
  std::vector<bool> assigned(topo.logicalCount, false);
  for (const std::uint32_t cpu : topo.physicalCores) {
    if (cpu < assigned.size() && assigned[cpu]) {
      continue;
    }
    const std::string l3Path = "/sys/devices/system/cpu/cpu" +
                               std::to_string(cpu) +
                               "/cache/index3/shared_cpu_list";
    std::vector<std::uint32_t> shared = readCpuList(l3Path);
    if (shared.empty()) {
      // Either sysfs is absent or this CPU has no L3; emit a singleton group.
      shared.push_back(cpu);
    }

    std::vector<std::uint32_t> physicalInGroup;
    physicalInGroup.reserve(shared.size());
    for (const std::uint32_t sharedCpu : shared) {
      const auto it = std::find(topo.physicalCores.begin(),
                                topo.physicalCores.end(), sharedCpu);
      if (it != topo.physicalCores.end()) {
        physicalInGroup.push_back(sharedCpu);
        if (sharedCpu < assigned.size()) {
          assigned[sharedCpu] = true;
        }
      }
    }
    if (physicalInGroup.empty()) {
      physicalInGroup.push_back(cpu);
      if (cpu < assigned.size()) {
        assigned[cpu] = true;
      }
    }
    const auto ccdIndex = static_cast<std::uint32_t>(topo.ccdGroups.size());
    for (const std::uint32_t physCpu : physicalInGroup) {
      if (physCpu < topo.ccdOfCpu.size()) {
        topo.ccdOfCpu[physCpu] = ccdIndex;
      }
    }
    topo.ccdGroups.push_back(std::move(physicalInGroup));
  }

  topo.ccdCount = static_cast<std::uint32_t>(topo.ccdGroups.size());

  // Per-CCD L3 size + preferred-CCD selection. V-Cache parts have one CCD with
  // a stacked SRAM die (96 MiB on 9950X3D's CCD0 vs 32 MiB on the regular CCD);
  // for workloads whose working set exceeds the smaller L3 but fits the larger,
  // landing on the V-Cache CCD is a 5-10x speedup. We pick the largest-L3 CCD
  // as the default placement target; tie-break by lowest index so symmetric
  // Zens (no X3D) still get a deterministic choice across runs.
  topo.l3KibOfCcd.assign(topo.ccdGroups.size(), 0U);
  for (std::size_t ccd = 0; ccd < topo.ccdGroups.size(); ++ccd) {
    if (topo.ccdGroups[ccd].empty()) {
      continue;
    }
    const std::uint32_t rep = topo.ccdGroups[ccd].front();
    topo.l3KibOfCcd[ccd] =
        readCacheSizeKib("/sys/devices/system/cpu/cpu" + std::to_string(rep) +
                         "/cache/index3/size");
  }
  // Per-core L2: probe one representative CPU. Per-core L2 is
  // architecture-uniform on every CPU we currently target (Zen, P-cores
  // on Alder Lake, Apple firestorm/icestorm), so a single sample is
  // sufficient. Future heterogeneous parts will need a per-cluster
  // probe.
  if (!topo.physicalCores.empty()) {
    topo.l2KibPerCore = readCacheSizeKib(
        "/sys/devices/system/cpu/cpu" +
        std::to_string(topo.physicalCores.front()) + "/cache/index2/size");
  }
  // Two-step CCD pick: a largest-L3 CCD wider than 1.5x the next wins
  // outright (V-Cache); otherwise pick the largest non-BSP CCD, since
  // IRQ / kthread placement biases toward CPU 0's CCD. Fall back to
  // absolute largest when only one CCD exists.
  std::uint64_t largestKib = 0U;
  std::uint64_t secondKib = 0U;
  std::uint32_t largestIdx = 0;
  for (std::size_t ccd = 0; ccd < topo.l3KibOfCcd.size(); ++ccd) {
    const std::uint64_t k = topo.l3KibOfCcd[ccd];
    if (k > largestKib) {
      secondKib = largestKib;
      largestKib = k;
      largestIdx = static_cast<std::uint32_t>(ccd);
    } else if (k > secondKib) {
      secondKib = k;
    }
  }
  const bool asymmetricL3 =
      secondKib == 0U || (largestKib * 2U > secondKib * 3U);
  if (asymmetricL3) {
    topo.preferredCcd = largestIdx;
  } else {
    const std::uint32_t bspCcd =
        (!topo.ccdOfCpu.empty()) ? topo.ccdOfCpu[0] : UINT32_MAX;
    std::uint64_t bestKib = 0U;
    std::uint32_t bestIdx = UINT32_MAX;
    for (std::size_t ccd = 0; ccd < topo.l3KibOfCcd.size(); ++ccd) {
      if (static_cast<std::uint32_t>(ccd) == bspCcd) {
        continue;
      }
      if (topo.l3KibOfCcd[ccd] > bestKib ||
          (topo.l3KibOfCcd[ccd] == bestKib && bestIdx == UINT32_MAX)) {
        bestKib = topo.l3KibOfCcd[ccd];
        bestIdx = static_cast<std::uint32_t>(ccd);
      }
    }
    topo.preferredCcd = bestIdx == UINT32_MAX ? largestIdx : bestIdx;
  }

  // Reorder each CCD so SMT-capable cores appear first, descending CPU
  // id otherwise. Hybrid Intel parts expose E-cores at higher CPU ids;
  // pure-descending order would place the producer on an E-core, where
  // the slot-1 SMT-sibling routing rule has nothing to attach to. The
  // descending tail within each bucket preserves the irqbalance bias
  // that puts kthreads on the low-numbered siblings.
  std::vector<std::uint32_t> reordered;
  reordered.reserve(topo.physicalCores.size());
  for (const auto &group : topo.ccdGroups) {
    std::vector<std::uint32_t> withSibling;
    std::vector<std::uint32_t> withoutSibling;
    withSibling.reserve(group.size());
    withoutSibling.reserve(group.size());
    for (std::size_t i = group.size(); i > 0U; --i) {
      const std::uint32_t cpu = group[i - 1U];
      const bool hasSibling = cpu < topo.smtSiblingOfCpu.size() &&
                              topo.smtSiblingOfCpu[cpu] != UINT32_MAX;
      if (hasSibling) {
        withSibling.push_back(cpu);
      } else {
        withoutSibling.push_back(cpu);
      }
    }
    reordered.insert(reordered.end(), withSibling.begin(), withSibling.end());
    reordered.insert(reordered.end(), withoutSibling.begin(),
                     withoutSibling.end());
  }
  if (reordered.size() == topo.physicalCores.size()) {
    topo.physicalCores = std::move(reordered);
  }
  return topo;
#endif // _WIN32
}

/// Enumerate the CPU ids belonging to each CCD (or shared-L3 cluster).
///
/// Returns one inner vector per CCD; the outer vector's index is the CCD id.
/// The CPU ids are physical-core representatives (one logical per sibling set)
/// inside the process affinity mask, matching `Topology::ccdGroups` exactly.
/// Used by the `PoolGroup` constructor to size and pin one arena per CCD.
///
/// Mock fallback: when sysfs is unavailable or the process is restricted to a
/// single CPU, the function returns a single CCD containing every allowed CPU.
/// Callers should treat the empty outer vector as a host with no usable CPUs
/// (never observed in practice).
///
/// Outer vector indexed by CCD id; inner vectors are CPU id lists per CCD.
inline std::vector<std::vector<unsigned>> enumerateCcds() {
  const Topology topo = detectTopology();
  std::vector<std::vector<unsigned>> result;
  result.reserve(topo.ccdGroups.size());
  for (const auto &group : topo.ccdGroups) {
    std::vector<unsigned> cpus;
    cpus.reserve(group.size());
    for (const std::uint32_t cpu : group) {
      cpus.push_back(static_cast<unsigned>(cpu));
    }
    result.push_back(std::move(cpus));
  }
  if (result.empty()) {
    std::vector<unsigned> all;
    all.reserve(topo.physicalCores.size());
    for (const std::uint32_t cpu : topo.physicalCores) {
      all.push_back(static_cast<unsigned>(cpu));
    }
    if (all.empty()) {
      const unsigned hwc = std::thread::hardware_concurrency();
      const unsigned bound = hwc > 0U ? hwc : 1U;
      all.reserve(bound);
      for (unsigned cpu = 0; cpu < bound; ++cpu) {
        all.push_back(cpu);
      }
    }
    result.push_back(std::move(all));
  }
  return result;
}

/// Reorder `|cpuPins|` so the producer's reserved CPU sits at index 0.
/// Two exceptions to the default physical-cores-first layout:
///   * `participants == 2`: slot 1 lands on the producer's SMT sibling
///     so the handshake stays L1-resident.
///   * `participants > pins.size()`: SMT siblings of existing pins are
///     appended as overflow, producer-CCD first.
/// Standalone pools also get a producer-CCD-first reorder. Arena pools
/// skip it because `PoolGroup` already filtered `cpuPins` to one CCD.
inline std::vector<std::uint32_t>
reserveProducerCpuFirst(const std::vector<std::uint32_t> &cpuPins,
                        std::size_t participants, bool standalone,
                        const Topology &topo) {
  std::vector<std::uint32_t> pins = cpuPins;
  if (pins.size() <= 1U) {
    return pins;
  }
  if (standalone) {
    const std::uint32_t targetCcd = topo.preferredCcd;
    if (targetCcd < topo.ccdGroups.size()) {
      std::vector<std::uint32_t> reordered;
      reordered.reserve(pins.size());
      for (const std::uint32_t cpu : pins) {
        if (cpu < topo.ccdOfCpu.size() && topo.ccdOfCpu[cpu] == targetCcd) {
          reordered.push_back(cpu);
        }
      }
      for (const std::uint32_t cpu : pins) {
        if (cpu >= topo.ccdOfCpu.size() || topo.ccdOfCpu[cpu] != targetCcd) {
          reordered.push_back(cpu);
        }
      }
      if (reordered.size() == pins.size()) {
        pins = std::move(reordered);
      }
    }
  }

  // Exception A: single-worker pool. Slot 1 = producer's SMT sibling.
  if (participants == 2U && !topo.smtSiblingOfCpu.empty()) {
    const std::uint32_t prodCpu = pins[0];
    if (prodCpu < topo.smtSiblingOfCpu.size()) {
      const std::uint32_t sib = topo.smtSiblingOfCpu[prodCpu];
      if (sib != UINT32_MAX && sib != prodCpu) {
        const auto sibIt = std::find(pins.begin(), pins.end(), sib);
        if (sibIt == pins.end()) {
          pins.insert(pins.begin() + 1, sib);
        } else if (sibIt != pins.begin() + 1) {
          std::iter_swap(pins.begin() + 1, sibIt);
        }
      }
    }
    return pins;
  }

  // Exception B: oversubscribed pool. Append SMT siblings as overflow,
  // producer-CCD siblings first.
  if (participants > pins.size() && !topo.smtSiblingOfCpu.empty()) {
    const std::uint32_t targetCcd = topo.preferredCcd;
    const std::size_t baseCount = pins.size();
    for (std::size_t pass = 0; pass < 2U; ++pass) {
      for (std::size_t i = 0; i < baseCount; ++i) {
        const std::uint32_t cpu = pins[i];
        if (cpu >= topo.smtSiblingOfCpu.size()) {
          continue;
        }
        const std::uint32_t sib = topo.smtSiblingOfCpu[cpu];
        if (sib == UINT32_MAX || sib == cpu) {
          continue;
        }
        const bool preferredCcdMatch = sib < topo.ccdOfCpu.size() &&
                                       targetCcd < topo.ccdGroups.size() &&
                                       topo.ccdOfCpu[sib] == targetCcd;
        if (pass == 0U && !preferredCcdMatch) {
          continue;
        }
        if (pass == 1U && preferredCcdMatch) {
          continue;
        }
        const bool already =
            std::find(pins.begin(), pins.end(), sib) != pins.end();
        if (!already) {
          pins.push_back(sib);
        }
      }
    }
  }
  return pins;
}

/// Pin the calling thread to a single CPU id.
///
/// Called exactly once per worker, immediately after creation. The hot path
/// never invokes `pthread_setaffinity_np`. Failures (e.g. CPU not in the
/// process mask) are silently ignored; the pool tolerates the fallback to OS
/// scheduling rather than aborting startup.
///
/// cpuId Logical CPU id to pin to.
inline void bindAffinityOnce(std::uint32_t cpuId) noexcept {
#ifdef __linux__
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<std::size_t>(cpuId), &set);
  (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#elif defined(_WIN32)
  // Single-CPU mask within the current processor group. Hosts with >64
  // logical CPUs would need `SetThreadGroupAffinity`; pools that large are
  // out of the supported envelope. Failure (e.g. CPU outside the process
  // affinity mask) is intentionally non-fatal.
  const DWORD_PTR mask = static_cast<DWORD_PTR>(1) << (cpuId & 63U);
  (void)::SetThreadAffinityMask(::GetCurrentThread(), mask);
#else
  (void)cpuId;
#endif
}

#ifdef _WIN32
/// Pin the calling thread to a single CPU and return the previous
/// affinity mask. `SetThreadAffinityMask` returns the previous mask
/// atomically with the new application, so one call covers save +
/// apply. Returns 0 on failure (caller does not restore).
inline DWORD_PTR pinCurrentThreadAndSave(std::uint32_t cpuId) noexcept {
  const DWORD_PTR mask = static_cast<DWORD_PTR>(1) << (cpuId & 63U);
  return ::SetThreadAffinityMask(::GetCurrentThread(), mask);
}
#endif

} // namespace citor::detail

// ===== citor/detail/worker_state.h =====



namespace citor::detail {

/// Reserved slot for the per-worker Chase-Lev work-stealing deque. Holds a
/// `void *` placeholder so the `WorkerState` layout, sizing, and alignment
/// remain stable. The deque type owns the heap-allocated payload pointed to
/// by `storage`; the `void *` width keeps `WorkerState` trivially-zeroed
/// without coupling the engine to the deque header.
struct ChaseLevDequeSlot {
  /// Pointer to the heap-allocated deque payload, owned by the
  /// work-stealing deque type.
  void *storage = nullptr;
};

/// Per-worker state owned by the pool, one instance per participant.
///
/// `WorkerState` carries the worker's identity, mailbox publish/ack slot,
/// observability counters, and a pointer to the deque. Every contended
/// atomic sits on its own `kCacheLine`-sized line so a write on the
/// mailbox does not invalidate the worker's identity or counters.
///
/// Counters are relaxed-atomic so workers can update them on the hot path
/// without synchronizing other state; readers (telemetry, tests) accept
/// order-tolerant snapshots.
struct WorkerState {
  /// Per-worker mailbox: the producer's publish line for this slot, doubling
  /// as the worker's same-line DONE ack via `PoolControl::kDoneBit`.
  ///
  /// Producer publishes a new dispatch by writing this slot's mailbox to
  /// the dispatch's phase counter (bit 0 = shutdown, bits 2..63 = monotonic
  /// phase, matching `PoolControl::generation`'s layout). The worker spins
  /// on its own mailbox instead of the shared `generation`, eliminating the
  /// N-readers-on-one-line coherence storm under fan-out.
  ///
  /// Co-located with `mailboxDesc`: producer writes `mailboxDesc` first
  /// (relaxed) then `mailbox` (release) so the worker's acquire-load on
  /// `mailbox` synchronizes with both writes. Worker reads `mailboxDesc`
  /// directly off this private cache line instead of going through the
  /// shared `m_control.activeJob`, eliminating the N-readers shared-line
  /// read on every dispatch.
  ///
  /// After running its share the worker stamps `mailbox |= kDoneBit` (release)
  /// so the producer's join reads done state on the same cache line it just
  /// published the dispatch on. One line per worker on the join path replaces
  /// the old two-line publish + done-epoch protocol.
  ///
  /// Lives alone on a 128-byte line because the producer writes it on
  /// every dispatch and the worker reads it every spin iteration.
  alignas(kCacheLine) std::atomic<std::uint64_t> mailbox{0};

  /// Per-worker descriptor pointer co-located with `mailbox`. Producer
  /// writes this immediately before bumping `mailbox`; the worker's
  /// acquire-load on `mailbox` picks both up via release ordering.
  /// `nullptr` outside an active dispatch.
  void *mailboxDesc = nullptr;

  /// Identity, affinity, and link to topology metadata. `workerId` is the
  /// worker's slot index in the pool's vector; `cpuId` is the CPU id the
  /// worker was pinned to (when affinity is requested); `ccdId` is the CCD
  /// index resolved from topology.
  alignas(kCacheLine) std::uint32_t workerId = 0;

  /// CPU id the worker is pinned to; equals `UINT32_MAX` when affinity was
  /// not requested.
  std::uint32_t cpuId = UINT32_MAX;

  /// CCD (or shared-L3) group index, or `UINT32_MAX` when topology was
  /// unavailable.
  std::uint32_t ccdId = UINT32_MAX;

  /// Relaxed-atomic counters used for observability and tests. Each counter
  /// sits on its own line so observability traffic does not pollute another
  /// worker's hot path.
  alignas(kCacheLine) std::atomic<std::uint64_t> parks{0};

  /// Number of `FUTEX_WAKE_PRIVATE` calls observed by this worker.
  alignas(kCacheLine) std::atomic<std::uint64_t> wakes{0};

  /// Number of dispatches this worker participated in.
  alignas(kCacheLine) std::atomic<std::uint64_t> dispatches{0};

  /// Low-latency scope epoch most recently observed by this worker while
  /// idle.
  alignas(kCacheLine) std::atomic<std::uint64_t> hotSpinEpoch{0};

  /// Total steal probes attempted by this worker.
  alignas(kCacheLine) std::atomic<std::uint64_t> stealAttempts{0};

  /// Total steal probes that successfully dequeued a task.
  alignas(kCacheLine) std::atomic<std::uint64_t> stealSuccesses{0};

  /// Per-rank generation claim slot for producer/worker cold-collapse
  /// races. For dispatches that opt into cold-collapse
  /// (`JobDescriptor::workerStateBase != nullptr`), the producer and the
  /// worker race on this slot via `compare_exchange`: the side that bumps
  /// `claimedAt` from `<currentGen` to `currentGen` wins the right to run
  /// rank R's blocks. The loser observes the new value and skips its
  /// share. The winner stamps `mailbox = doneSentinel` after running the
  /// partition so the producer's join wait is satisfied.
  ///
  /// Lives alone on a 128-byte line because the producer's cold-collapse
  /// loop CAS-probes every background worker's slot in turn; co-locating
  /// with `mailbox` would invalidate the wakeup line during the probe.
  /// Default value `0` is below any real generation (workers' first
  /// dispatch sees gen >= `kPhaseStep` > 0).
  alignas(kCacheLine) std::atomic<std::uint64_t> claimedAt{0};

  /// Reserved deque slot.
  alignas(kCacheLine) ChaseLevDequeSlot deque{};
};

// Hot-path offsets must stay pinned: the dispatch loop indexes `mailbox`
// and the per-worker counters through these constants and a shift would
// silently move state into another cache line.
static_assert(offsetof(WorkerState, mailbox) == 0);
static_assert(offsetof(WorkerState, workerId) == kCacheLine);
static_assert(offsetof(WorkerState, parks) == kCacheLine * 2);
static_assert(offsetof(WorkerState, wakes) == kCacheLine * 3);
static_assert(offsetof(WorkerState, dispatches) == kCacheLine * 4);
static_assert(offsetof(WorkerState, hotSpinEpoch) == kCacheLine * 5);
static_assert(offsetof(WorkerState, stealAttempts) == kCacheLine * 6);
static_assert(offsetof(WorkerState, stealSuccesses) == kCacheLine * 7);
static_assert(offsetof(WorkerState, claimedAt) == kCacheLine * 8);
static_assert(offsetof(WorkerState, deque) == kCacheLine * 9);

// The full struct must fit comfortably in L2 across the worker fleet
// (16 workers x 4 KiB = 64 KiB).
static_assert(sizeof(WorkerState) <= 4096);

} // namespace citor::detail

// ===== citor/detail/dispatch_static.h =====



namespace citor::detail {

/// CAS-claim a generation on a worker's `claimedAt` slot.
///
/// Returns true when this caller successfully bumped `slot.claimedAt` from a
/// value `<currentGen` up to `currentGen`. Returns false when the slot is
/// already at `>= currentGen` (i.e. another caller -- producer cold-collapse
/// vs. the worker -- has already claimed this rank for the current dispatch).
[[gnu::always_inline]] inline bool
tryClaimRank(WorkerState &slot, std::uint64_t currentGen) noexcept {
  std::uint64_t expected = slot.claimedAt.load(std::memory_order_acquire);
  while (expected < currentGen) {
    if (slot.claimedAt.compare_exchange_weak(expected, currentGen,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
      return true;
    }
  }
  return false;
}

/// Sentinel returned by `BlockClaim::next` when no more blocks are available.
inline constexpr std::size_t kNoBlock = static_cast<std::size_t>(-1);

/// Per-balance block-claim policy.
///
/// The dispatch worker entry and slot-0 runner share a single iteration loop
/// that calls `BlockClaim<B>::next(...)` to obtain the next block id. Balance
/// variants differ only in this one function: `StaticUniform` rank-strides;
/// `DynamicChunked` races on the shared atomic.
///
/// Returning `kNoBlock` ends the loop. The hybrid Static/Dynamic distinction
/// the engine had before this refactor is now expressed entirely as the choice
/// of policy.
template <Balance B>
struct BlockClaim;

/// Static rank-strided iteration: rank R runs blocks `{R, R+P, R+2P, ...}`
/// until past `blockCount`. No atomic on the hot path.
template <>
struct BlockClaim<Balance::StaticUniform> {
  /// Returns the next block id this rank should run, or `kNoBlock` when
  /// the rank has run all of its statically assigned blocks. `|prevIdx|`
  /// is `kNoBlock` on the first call and the previously returned block id
  /// thereafter.
  [[gnu::always_inline]] static std::size_t
  next(JobDescriptor *desc, std::uint32_t rank, std::size_t prevIdx) noexcept {
    const std::size_t participants = desc->participants;
    const std::size_t blockCount = desc->blockCount;
    const std::size_t cand =
        (prevIdx == kNoBlock) ? rank : prevIdx + participants;
    return cand < blockCount ? cand : kNoBlock;
  }
};

/// Dynamic atomic-counter iteration: workers race `desc->nextBlock.fetch_add`
/// for blocks. The dispatcher initialises `nextBlock` to a starting offset
/// (either `0` for pure dynamic or `participants` for the strided-Phase-A +
/// atomic-tail hybrid the bench uses).
template <>
struct BlockClaim<Balance::DynamicChunked> {
  /// Returns the next block id this rank should run, or `kNoBlock` when
  /// the dispatch is drained. The first call (`|prevIdx| == kNoBlock`)
  /// claims the rank-indexed block without touching `desc->nextBlock`;
  /// subsequent calls drain the atomic tail.
  [[gnu::always_inline]] static std::size_t
  next(JobDescriptor *desc, std::uint32_t rank, std::size_t prevIdx) noexcept {
    const std::size_t blockCount = desc->blockCount;
    // Phase A: when this is the first call (`prevIdx == kNoBlock`) and `rank <
    // blockCount`, the rank claims its statically-assigned block (the index
    // equal to its rank). This mirrors `StaticUniform`'s first iteration on
    // non-oversubscribed dispatches: with `blockCount <= participants` every
    // rank takes one block in Phase A, no fetch_add fires, and the dispatch
    // incurs zero atomic-counter contention.
    const std::size_t participants = desc->participants;
    if (prevIdx == kNoBlock && rank < blockCount && rank < participants) {
      return rank;
    }
    // Phase B: drain the atomic tail. The dispatcher initialised `nextBlock` to
    // `participants` when the partition is oversubscribed, so a fetch_add
    // returning `< blockCount` yields a block id outside the strided coverage
    // of Phase A. On non-oversubscribed dispatches this returns `>= blockCount`
    // immediately and exits without contention.
    if (blockCount <= participants) {
      return kNoBlock;
    }
    const std::uint64_t claimed =
        desc->nextBlock.fetch_add(1, std::memory_order_relaxed);
    return claimed < blockCount ? static_cast<std::size_t>(claimed) : kNoBlock;
  }
};

/// Untyped block iteration loop shared by `runStaticPartition` and
/// `runDynamicCounter`.
///
/// Calls `desc.body(lo, hi)` (FunctionRef indirect) for each block returned by
/// `BlockClaim<B>::next`. Used when the body's static type is not available at
/// the call site (worker-side fallbacks; chain stages with type-erased bodies).
template <Balance B>
inline void runPartition(JobDescriptor &desc, std::uint32_t rank) noexcept {
  const std::size_t chunk = desc.chunk;
  const std::size_t first = desc.first;
  const std::size_t last = desc.last;

  for (std::size_t blockId = kNoBlock;
       (blockId = BlockClaim<B>::next(&desc, rank, blockId)) != kNoBlock;) {
    if (desc.firstException.load(std::memory_order_acquire) != nullptr)
        [[unlikely]] {
      return;
    }
    if (desc.token.stop_requested()) [[unlikely]] {
      return;
    }
    const std::size_t lo = first + (blockId * chunk);
    const std::size_t hi = std::min(lo + chunk, last);
    try {
      desc.body(lo, hi);
    } catch (...) {
      auto *eptr =
          new (std::nothrow) std::exception_ptr(std::current_exception());
      if (eptr == nullptr) {
        std::terminate();
      }
      std::exception_ptr *expected = nullptr; // NOLINT(misc-const-correctness)
      if (!desc.firstException.compare_exchange_strong(
              expected, eptr, std::memory_order_release,
              std::memory_order_acquire)) {
        delete eptr;
      } else {
        desc.exceptionWorkerId.store(rank, std::memory_order_release);
      }
      return;
    }
  }
}

/// Inlined typed counterpart to `BlockClaim<B>::next`. The compile-time
/// `Balance` selects the strided or atomic-tail policy without going
/// through the policy struct so the loop body inlines into the typed
/// worker entry.
template <Balance B>
[[gnu::always_inline]] inline std::size_t
nextTypedBlock(JobDescriptor &desc, std::size_t blockCount,
               std::size_t participants, std::uint32_t rank,
               std::size_t prevIdx) noexcept {
  if constexpr (B == Balance::StaticUniform) {
    const std::size_t cand =
        (prevIdx == kNoBlock) ? rank : prevIdx + participants;
    return cand < blockCount ? cand : kNoBlock;
  } else {
    if (prevIdx == kNoBlock && rank < blockCount && rank < participants) {
      return rank;
    }
    if (blockCount <= participants) {
      return kNoBlock;
    }
    const std::uint64_t claimed =
        desc.nextBlock.fetch_add(1, std::memory_order_relaxed);
    return claimed < blockCount ? static_cast<std::size_t>(claimed) : kNoBlock;
  }
}

/// Static-balance untyped runner: kept as a name alias for legacy callers.
inline void runStaticPartition(JobDescriptor &desc,
                               std::uint32_t rank) noexcept {
  runPartition<Balance::StaticUniform>(desc, rank);
}

/// Dynamic-balance untyped runner: kept as a name alias for legacy callers.
inline void runDynamicCounter(JobDescriptor &desc,
                              std::uint32_t rank) noexcept {
  // Cold-collapse CAS-claim: producer's join-wait may race the worker for this
  // rank's claim slot. Loser returns; producer (or the winning worker) is
  // responsible for stamping `mailbox = doneSentinel` so the join rendezvous
  // fires.
  if (desc.workerStateBase != nullptr) [[unlikely]] {
    auto *wsBase = static_cast<WorkerState *>(desc.workerStateBase);
    if (!tryClaimRank(wsBase[rank], desc.generation)) {
      return;
    }
  }
  runPartition<Balance::DynamicChunked>(desc, rank);
}

/// Typed slot-0 partition runner: same as `runPartition` but calls `fn(lo, hi)`
/// directly
///        instead of going through `desc.body`'s `FunctionRef` indirection.
///
/// Used by the producer's slot-0 path inside `dispatchOneStaticLockedBody` when
/// the caller has the body's static type available (parallelFor /
/// parallelReduce / bulkForQueries instantiations pass the lambda type as a
/// template parameter).
template <Balance B, class FOp>
inline void runPartitionTyped(JobDescriptor &desc, std::uint32_t rank,
                              FOp &fn) noexcept {
  const std::size_t chunk = desc.chunk;
  const std::size_t first = desc.first;
  const std::size_t last = desc.last;
  const std::size_t blockCount = desc.blockCount;
  const std::size_t participants = desc.participants;

  for (std::size_t blockId = kNoBlock;
       (blockId = nextTypedBlock<B>(desc, blockCount, participants, rank,
                                    blockId)) != kNoBlock;) {
    if (desc.firstException.load(std::memory_order_acquire) != nullptr)
        [[unlikely]] {
      return;
    }
    if (desc.token.stop_requested()) [[unlikely]] {
      return;
    }
    const std::size_t lo = first + (blockId * chunk);
    const std::size_t hi = std::min(lo + chunk, last);
    try {
      fn(lo, hi);
    } catch (...) {
      auto *eptr =
          new (std::nothrow) std::exception_ptr(std::current_exception());
      if (eptr == nullptr) {
        std::terminate();
      }
      std::exception_ptr *expected = nullptr; // NOLINT(misc-const-correctness)
      if (!desc.firstException.compare_exchange_strong(
              expected, eptr, std::memory_order_release,
              std::memory_order_acquire)) {
        delete eptr;
      } else {
        desc.exceptionWorkerId.store(rank, std::memory_order_release);
      }
      return;
    }
  }
}

/// Legacy alias for the typed Static slot-0 runner. Existing call sites use
/// this name.
template <class FOp>
[[gnu::always_inline]] inline void runStaticPartitionTyped(JobDescriptor &desc,
                                                           std::uint32_t rank,
                                                           FOp &fn) noexcept {
  runPartitionTyped<Balance::StaticUniform>(desc, rank, fn);
}

/// Single-block fast path used when `blockCount <= participants`. Runs at
/// most one block per rank with no claim loop and no atomic counter
/// touch. Cancellation and exception checks are elided when `HintsT` and
/// the body's noexcept-ness allow.
template <class HintsT, class FOp>
[[gnu::always_inline]] inline void
runSingleRankBlockTyped(JobDescriptor &desc, std::uint32_t rank, FOp &fn,
                        std::size_t blockCount, std::size_t chunk,
                        std::size_t first, std::size_t last) noexcept {
  if (rank >= blockCount) {
    return;
  }
  constexpr bool kCancellationActive = detail::kCancellationActive<HintsT>;
  constexpr bool kBodyNoexcept =
      std::is_nothrow_invocable_v<FOp &, std::size_t, std::size_t>;

  if constexpr (!kBodyNoexcept) {
    if (desc.firstException.load(std::memory_order_acquire) != nullptr)
        [[unlikely]] {
      return;
    }
  }
  if constexpr (kCancellationActive) {
    if (desc.token.stop_requested()) [[unlikely]] {
      return;
    }
  }
  const std::size_t lo = first + (static_cast<std::size_t>(rank) * chunk);
  const std::size_t hi = std::min(lo + chunk, last);
  if constexpr (kBodyNoexcept) {
    fn(lo, hi);
  } else {
    try {
      fn(lo, hi);
    } catch (...) {
      auto *eptr =
          new (std::nothrow) std::exception_ptr(std::current_exception());
      if (eptr == nullptr) {
        std::terminate();
      }
      std::exception_ptr *expected = nullptr; // NOLINT(misc-const-correctness)
      if (!desc.firstException.compare_exchange_strong(
              expected, eptr, std::memory_order_release,
              std::memory_order_acquire)) {
        delete eptr;
      } else {
        desc.exceptionWorkerId.store(rank, std::memory_order_release);
      }
    }
  }
}

/// Typed partition runner with `HintsT` compile-time gating. Routes to
/// the single-block fast path when `blockCount <= participants`; otherwise
/// runs the typed claim loop with cancellation and exception checks
/// elided per `HintsT`.
template <Balance B, class HintsT, class FOp>
[[gnu::always_inline]] inline void runPartitionTypedHinted(JobDescriptor &desc,
                                                           std::uint32_t rank,
                                                           FOp &fn) noexcept {
  const std::size_t chunk = desc.chunk;
  const std::size_t first = desc.first;
  const std::size_t last = desc.last;
  const std::size_t blockCount = desc.blockCount;
  const std::size_t participants = desc.participants;

  if (blockCount <= participants) {
    runSingleRankBlockTyped<HintsT>(desc, rank, fn, blockCount, chunk, first,
                                    last);
    return;
  }

  constexpr bool kCancellationActive = detail::kCancellationActive<HintsT>;
  constexpr bool kBodyNoexcept =
      std::is_nothrow_invocable_v<FOp &, std::size_t, std::size_t>;

  for (std::size_t blockId = kNoBlock;
       (blockId = nextTypedBlock<B>(desc, blockCount, participants, rank,
                                    blockId)) != kNoBlock;) {
    if constexpr (!kBodyNoexcept) {
      if (desc.firstException.load(std::memory_order_acquire) != nullptr)
          [[unlikely]] {
        return;
      }
    }
    if constexpr (kCancellationActive) {
      if (desc.token.stop_requested()) [[unlikely]] {
        return;
      }
    }
    const std::size_t lo = first + (blockId * chunk);
    const std::size_t hi = std::min(lo + chunk, last);
    if constexpr (kBodyNoexcept) {
      fn(lo, hi);
    } else {
      try {
        fn(lo, hi);
      } catch (...) {
        auto *eptr =
            new (std::nothrow) std::exception_ptr(std::current_exception());
        if (eptr == nullptr) {
          std::terminate();
        }
        // NOLINTNEXTLINE(misc-const-correctness)
        std::exception_ptr *expected = nullptr;
        if (!desc.firstException.compare_exchange_strong(
                expected, eptr, std::memory_order_release,
                std::memory_order_acquire)) {
          delete eptr;
        } else {
          desc.exceptionWorkerId.store(rank, std::memory_order_release);
        }
        return;
      }
    }
  }
}

/// Returns the contiguous `[begin, end)` block span owned by `|rank|`
/// under a balanced contiguous partition of `|blockCount|` blocks across
/// `|participants|` ranks. The first `|blockCount| % |participants|`
/// ranks each get one extra block.
[[gnu::always_inline]] inline std::pair<std::size_t, std::size_t>
contiguousRankBlockSpan(std::size_t blockCount, std::size_t participants,
                        std::uint32_t rank) noexcept {
  if (rank >= participants) [[unlikely]] {
    return {blockCount, blockCount};
  }
  const std::size_t rankU = rank;
  const std::size_t base = blockCount / participants;
  const std::size_t extra = blockCount % participants;
  const std::size_t begin = (rankU * base) + std::min(rankU, extra);
  const std::size_t end = begin + base + (rankU < extra ? 1U : 0U);
  return {begin, end};
}

/// Typed contiguous-partition runner driven by precomputed cached
/// parameters (`blockCount`, `participants`, `chunk`, `first`, `last`).
/// Each rank walks its `contiguousRankBlockSpan` once with no atomic
/// counter touch. Detects whether `|fn|` accepts a leading `blockId`
/// argument and dispatches accordingly.
template <class HintsT, class FOp>
[[gnu::always_inline]] inline void
// The `kBodyNoexcept` branch dispatches the no-throw body without a `try`
// block; the throwing branch wraps the call in `try` and stores the first
// exception into the descriptor without escaping. Tidy cannot see through
// the templated condition.
// NOLINTNEXTLINE(bugprone-exception-escape)
runContiguousRankPartitionTypedCached(JobDescriptor &desc, std::uint32_t rank,
                                      FOp &fn, std::size_t blockCount,
                                      std::size_t participants,
                                      std::size_t chunk, std::size_t first,
                                      std::size_t last) noexcept {
  const auto [begin, end] =
      contiguousRankBlockSpan(blockCount, participants, rank);

  constexpr bool kCancellationActive = detail::kCancellationActive<HintsT>;
  constexpr bool kPassBlockId =
      std::is_invocable_v<FOp &, std::size_t, std::size_t, std::size_t>;
  constexpr bool kBodyNoexcept =
      kPassBlockId
          ? std::is_nothrow_invocable_v<FOp &, std::size_t, std::size_t,
                                        std::size_t>
          : std::is_nothrow_invocable_v<FOp &, std::size_t, std::size_t>;

  auto runBlocks = [&]() {
    for (std::size_t blockId = begin; blockId < end; ++blockId) {
      if constexpr (kCancellationActive) {
        if (desc.token.stop_requested()) [[unlikely]] {
          return;
        }
      }
      const std::size_t lo = first + (blockId * chunk);
      const std::size_t hi = std::min(lo + chunk, last);
      if constexpr (kPassBlockId) {
        fn(blockId, lo, hi);
      } else {
        fn(lo, hi);
      }
    }
  };

  if constexpr (kBodyNoexcept) {
    runBlocks();
  } else {
    if (desc.firstException.load(std::memory_order_acquire) != nullptr)
        [[unlikely]] {
      return;
    }
    try {
      runBlocks();
    } catch (...) {
      auto *eptr =
          new (std::nothrow) std::exception_ptr(std::current_exception());
      if (eptr == nullptr) {
        std::terminate();
      }
      std::exception_ptr *expected = nullptr; // NOLINT(misc-const-correctness)
      if (!desc.firstException.compare_exchange_strong(
              expected, eptr, std::memory_order_release,
              std::memory_order_acquire)) {
        delete eptr;
      } else {
        desc.exceptionWorkerId.store(rank, std::memory_order_release);
      }
    }
  }
}

/// Convenience wrapper around `runContiguousRankPartitionTypedCached`
/// that reads the partition parameters from `|desc|` directly. Used when
/// the cached typed-for slot is not available (worker fallback paths).
template <class HintsT, class FOp>
[[gnu::always_inline]] inline void
runContiguousRankPartitionTyped(JobDescriptor &desc, std::uint32_t rank,
                                FOp &fn) noexcept {
  const std::size_t chunk = desc.chunk;
  const std::size_t first = desc.first;
  const std::size_t last = desc.last;
  const std::size_t blockCount = desc.blockCount;
  const std::size_t participants = desc.participants;

  runContiguousRankPartitionTypedCached<HintsT>(
      desc, rank, fn, blockCount, participants, chunk, first, last);
}

/// Per-(HintsT, F) cached job parameters for the typed worker entry.
/// Same-command reuse: when the producer detects an identical key vs the
/// previous dispatch, it bumps only mailbox.gen without re-publishing desc
/// fields. Worker reads cached values from TLS instead of the producer's TLS
/// desc cache line, eliminating that line transit on the hot path.
struct alignas(kCacheLine) CachedTypedForJob {
  /// Total number of blocks in the dispatch.
  std::size_t blockCount{0};
  /// Number of participating slots (producer plus workers).
  std::size_t participants{0};
  /// Block size in elements.
  std::size_t chunk{0};
  /// Inclusive first index of the iteration range.
  std::size_t first{0};
  /// Exclusive last index of the iteration range.
  std::size_t last{0};
  /// Type-erased pointer to the body callable. The worker reinterprets it
  /// as `F *` after the producer's reuse check matches.
  void *fnPtr{nullptr};
  /// Type-erased pointer to the pool's `WorkerState` array, used for the
  /// cold-collapse rank-claim CAS.
  void *workerStateBase{nullptr};
  /// True after the cache has been written at least once. The worker
  /// trusts cached values only when this is set.
  bool primed{false};
};

/// Returns the thread-local `CachedTypedForJob` cache for this `(HintsT,
/// F)` instantiation. Each typed worker entry has its own cache slot;
/// distinct body types do not collide.
template <class HintsT, class F>
inline CachedTypedForJob &cachedTypedForSlot() noexcept {
  static thread_local CachedTypedForJob cache;
  return cache;
}

/// Monomorphized typed worker entry, parameterized by balance.
///
/// One template body covers both `Balance::StaticUniform` and
/// `Balance::DynamicChunked`. The only per-balance difference -- which block id
/// the worker runs next -- is delegated to the `BlockClaim<B>` policy.
/// Compile-time elides:
///   - `desc->token.stop_requested()` when `HintsT::cancellationChecks` is
///   false
///   - try/catch frame when `F` is nothrow_invocable
///   - per-block exception load when `F` is nothrow_invocable
///
/// The `desc->nextBlock` value is read by `BlockClaim<DynamicChunked>::next`
/// directly off the descriptor, NOT from the TLS cache -- it is the only
/// mutable contended field per dispatch. Static iteration uses
/// `desc->participants`, which IS hoisted through the cached slot since it is
/// loop-invariant across blocks within a dispatch.
template <Balance B, class HintsT, class F>
inline void typedWorkerEntry(JobDescriptor *desc, std::uint32_t rankPacked,
                             std::uint64_t generation) noexcept {
  // High bit of rankPacked encodes producer's "reuse" hint: when set, the
  // producer's TLS key matched the previous dispatch (same fn / range / chunk /
  // participants), and the worker can safely skip reading desc fields entirely
  // -- they're identical to the cached values.
  constexpr std::uint32_t kReuseFlag = 0x80000000U;
  constexpr std::uint32_t kSkipClaimFlag = 0x40000000U;
  const bool reuse = (rankPacked & kReuseFlag) != 0U;
  const bool skipClaim = (rankPacked & kSkipClaimFlag) != 0U;
  const std::uint32_t rank = rankPacked & ~(kReuseFlag | kSkipClaimFlag);
  auto &cache = cachedTypedForSlot<HintsT, F>();
  if (!reuse || !cache.primed) [[unlikely]] {
    cache.blockCount = desc->blockCount;
    cache.participants = desc->participants;
    cache.chunk = desc->chunk;
    cache.first = desc->first;
    cache.last = desc->last;
    cache.fnPtr = desc->fnPtr;
    cache.workerStateBase = desc->workerStateBase;
    cache.primed = true;
  }
  // Cold-collapse CAS-claim: race the producer's join-wait fallback for the
  // right to run rank R's blocks. The loser returns immediately. The cache
  // refresh above runs unconditionally so workers that lose the race still have
  // a fresh cache for the next dispatch -- otherwise a string of cold
  // dispatches with reuse=true would leave the worker's TLS cache stale
  // forever.
  if (!skipClaim && cache.workerStateBase != nullptr) [[unlikely]] {
    auto *wsBase = static_cast<WorkerState *>(cache.workerStateBase);
    if (!tryClaimRank(wsBase[rank], generation)) {
      return;
    }
  }
  const std::size_t chunk = cache.chunk;
  const std::size_t first = cache.first;
  const std::size_t last = cache.last;
  const std::size_t blockCount = cache.blockCount;
  const std::size_t participants = cache.participants;
  auto &fn = *static_cast<F *>(cache.fnPtr);

  constexpr bool kCancellationActive = detail::kCancellationActive<HintsT>;
  constexpr bool kBodyNoexcept =
      std::is_nothrow_invocable_v<F &, std::size_t, std::size_t>;

  if (blockCount <= participants) {
    runSingleRankBlockTyped<HintsT>(*desc, rank, fn, blockCount, chunk, first,
                                    last);
    return;
  }

  for (std::size_t blockId = kNoBlock;
       (blockId = nextTypedBlock<B>(*desc, blockCount, participants, rank,
                                    blockId)) != kNoBlock;) {
    if constexpr (!kBodyNoexcept) {
      if (desc->firstException.load(std::memory_order_acquire) != nullptr)
          [[unlikely]] {
        return;
      }
    }
    if constexpr (kCancellationActive) {
      if (desc->token.stop_requested()) [[unlikely]] {
        return;
      }
    }
    const std::size_t lo = first + (blockId * chunk);
    const std::size_t hi = std::min(lo + chunk, last);
    if constexpr (kBodyNoexcept) {
      fn(lo, hi);
    } else {
      try {
        fn(lo, hi);
      } catch (...) {
        auto *eptr =
            new (std::nothrow) std::exception_ptr(std::current_exception());
        if (eptr == nullptr) {
          std::terminate();
        }
        // NOLINTNEXTLINE(misc-const-correctness)
        std::exception_ptr *expected = nullptr;
        if (!desc->firstException.compare_exchange_strong(
                expected, eptr, std::memory_order_release,
                std::memory_order_acquire)) {
          delete eptr;
        } else {
          desc->exceptionWorkerId.store(rank, std::memory_order_release);
        }
        return;
      }
    }
  }
}

/// Legacy alias for the typed Static worker entry. Existing call sites that
/// reference this name as the `desc.workerEntry` function pointer continue to
/// work.
template <class HintsT, class F>
inline void typedStaticUniformWorkerEntry(JobDescriptor *desc,
                                          std::uint32_t rankPacked,
                                          std::uint64_t generation) noexcept {
  typedWorkerEntry<Balance::StaticUniform, HintsT, F>(desc, rankPacked,
                                                      generation);
}

/// Typed worker entry for `Balance::StaticContiguous`. Each rank runs a
/// contiguous block span computed from its rank id; no claim CAS, no
/// atomic counter touch.
template <class HintsT, class F>
inline void
typedStaticContiguousWorkerEntry(JobDescriptor *desc, std::uint32_t rankPacked,
                                 std::uint64_t /*generation*/) noexcept {
  constexpr std::uint32_t kReuseFlag = 0x80000000U;
  constexpr std::uint32_t kSkipClaimFlag = 0x40000000U;
  const bool reuse = (rankPacked & kReuseFlag) != 0U;
  const std::uint32_t rank = rankPacked & ~(kReuseFlag | kSkipClaimFlag);
  auto &cache = cachedTypedForSlot<HintsT, F>();
  if (!reuse || !cache.primed) [[unlikely]] {
    cache.blockCount = desc->blockCount;
    cache.participants = desc->participants;
    cache.chunk = desc->chunk;
    cache.first = desc->first;
    cache.last = desc->last;
    cache.fnPtr = desc->fnPtr;
    cache.primed = true;
  }
  auto &fn = *static_cast<F *>(cache.fnPtr);
  runContiguousRankPartitionTypedCached<HintsT>(
      *desc, rank, fn, cache.blockCount, cache.participants, cache.chunk,
      cache.first, cache.last);
}

/// Typed worker entry for `Balance::DynamicChunked`. Used by parallelFor's
/// Dynamic dispatcher path; reuses every optimization on the typed path (TLS
/// cache, reuse-bit, cold-collapse, monomorphized direct call) via the unified
/// `typedWorkerEntry` template.
template <class HintsT, class F>
inline void typedDynamicChunkedWorkerEntry(JobDescriptor *desc,
                                           std::uint32_t rankPacked,
                                           std::uint64_t generation) noexcept {
  typedWorkerEntry<Balance::DynamicChunked, HintsT, F>(desc, rankPacked,
                                                       generation);
}

} // namespace citor::detail

// ===== citor/detail/dispatch_dynamic.h =====

// `Balance::DynamicChunked` shares its iteration / typed-entry / cold-collapse
// implementation with `Balance::StaticUniform` via the unified
// `typedWorkerEntry` and `runPartition` templates in `dispatch_static.h`. The
// only per-balance difference is the `BlockClaim<B>::next` policy.
//
// This header keeps a few legacy aliases callers historically referenced; new
// code should use the unified entries directly.


namespace citor::detail {

/// Typed slot-0 runner for `Balance::DynamicChunked`. Sibling of
/// `runStaticPartitionTyped`; both alias `runPartitionTyped<B>` parameterized
/// by balance.
template <class FOp>
[[gnu::always_inline]] inline void runDynamicCounterTyped(JobDescriptor &desc,
                                                          std::uint32_t rank,
                                                          FOp &fn) noexcept {
  runPartitionTyped<Balance::DynamicChunked>(desc, rank, fn);
}

} // namespace citor::detail

// ===== citor/detail/worker_loop.h =====


#if defined(_M_X64) && defined(_MSC_VER)
#include <intrin.h>
#endif


namespace citor::detail {

/// Spin-then-park budget applied after a bulk job completes.
///
/// Two bounds gate the spin: a PAUSE iteration count and a TSC cycle budget.
/// Both must trip before the worker invokes `FUTEX_WAIT_PRIVATE`. The PAUSE
/// bound captures co-tenancy cases where the scheduler de-schedules our
/// worker (TSC keeps advancing while iterations stay constant); the TSC
/// bound captures the inverse (busy spinning while iterations advance fast).
///
/// The Karlin 1991 SOSP optimum sets `maxCycles` to roughly twice the wakeup
/// latency.
struct SpinPolicy {
  /// Maximum PAUSE iterations before checking the TSC bound.
  std::uint32_t maxIters = 0;

  /// Maximum TSC cycles to spin before parking.
  std::uint64_t maxCycles = 0;
};

#ifndef CITOR_SPIN_AFTER_BULK_JOB_MAX_ITERS
#define CITOR_SPIN_AFTER_BULK_JOB_MAX_ITERS 8192U
#endif
#ifndef CITOR_SPIN_AFTER_BULK_JOB_MAX_CYCLES
#define CITOR_SPIN_AFTER_BULK_JOB_MAX_CYCLES 1000000ULL
#endif
/// Default spin budget applied to workers between jobs. The PAUSE and TSC
/// bounds together cover co-tenancy stalls and runaway loops; the cycle
/// budget sits above the typical back-to-back dispatch interval so workers
/// do not park between hot dispatches. A worker that idles past the budget
/// still parks promptly.
inline constexpr SpinPolicy kSpinAfterBulkJob{
    .maxIters = CITOR_SPIN_AFTER_BULK_JOB_MAX_ITERS,
    .maxCycles = CITOR_SPIN_AFTER_BULK_JOB_MAX_CYCLES};

/// Read the current TSC for spin-budget bookkeeping. Returns 0 on platforms
/// without a TSC; the caller's loop falls through to the iteration bound.
inline std::uint64_t readTsc() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
  unsigned int aux = 0;
#if defined(_M_X64) && defined(_MSC_VER)
  return __rdtscp(&aux);
#else
  return __builtin_ia32_rdtscp(&aux);
#endif
#else
  return 0ULL;
#endif
}

/// Check whether a generation observed by the worker indicates the pool
/// should exit.
///
/// The worker exits when the shutdown bit is set AND the active job slot is
/// empty. The active-job check guards the race where the producer publishes
/// a final job concurrently with the shutdown signal: workers must drain
/// that job before exiting.
inline bool shouldExit(const PoolControl &control) noexcept {
  const std::uint64_t gen = control.generation.load(std::memory_order_acquire);
  if ((gen & PoolControl::kShutdownBit) == 0) {
    return false;
  }
  return control.activeJob.load(std::memory_order_acquire) == nullptr;
}

/// Run the worker's share of the active job with the balance fixed at
/// compile time.
///
/// Producer call sites know the dispatch shape from the primitive's hint
/// type and use this overload to compile away the runtime branch on
/// `desc.balance`. Workers, which read the descriptor's runtime balance
/// after observing a new generation, use the runtime overload below.
template <Balance BalanceV>
inline void runActiveJobStatic(JobDescriptor &desc,
                               std::uint32_t rank) noexcept {
  if constexpr (BalanceV == Balance::StaticUniform) {
    runStaticPartition(desc, rank);
  } else {
    // Dynamic counter handles `DynamicChunked` directly; the work-stealing
    // tiers (`Steal`, `Recursive`) are reserved for opt-in primitives and
    // fall back here so the call still completes deterministically until
    // the deque-based tier ships.
    runDynamicCounter(desc, rank);
  }
}

/// Run the worker's share of the active job, dispatching by `Balance` kind
/// at runtime. Reads the published `JobDescriptor` from `control.activeJob`
/// (already acquire-loaded by the caller's logic) and runs the appropriate
/// dispatch tier. Static-uniform takes the worker-strided branch;
/// dynamic-chunked takes the relaxed-counter branch.
inline void runActiveJob(JobDescriptor &desc, std::uint32_t rank) noexcept {
  if (desc.balance == Balance::StaticUniform) {
    runActiveJobStatic<Balance::StaticUniform>(desc, rank);
    return;
  }
  runActiveJobStatic<Balance::DynamicChunked>(desc, rank);
}

/// Spin-then-park between dispatches, exiting cleanly on shutdown.
///
/// On every observed generation change the worker:
///   1. Loads `activeJob` (acquire) -- pairs with the producer's
///      release-store.
///   2. Runs its share of the job via `runActiveJob`.
///   3. Stamps `mailbox |= kDoneBit` (release) -- pairs with the
///      producer's acquire-load on the join path. Same-line ack: the
///      mailbox line carries publish + ack so the producer's join reads
///      one cache line per worker, not two.
///
/// Lost-wakeup defense: the worker reads `futexWord` BEFORE re-checking
/// `generation`, then passes the captured token to `futexWaitPrivate` so
/// the kernel rejects the wait with `EAGAIN` if a wake raced ahead. After
/// the syscall returns the worker re-checks `generation` again; spurious
/// wakes are correctness-neutral.
inline void workerMainLoop(WorkerState &self, PoolControl &control) noexcept {
  std::uint64_t lastSeenMailbox = 0;
  // Cache the worker's identity locally; `WorkerState::workerId` lives on
  // a read-only line but the local lets the compiler keep it in a register
  // across the dispatch+stamp loop.
  const std::uint32_t workerId = self.workerId;
  JobDescriptor *cachedDesc = nullptr; // NOLINT(misc-const-correctness)
  void (*cachedWorkerEntry)(JobDescriptor *, std::uint32_t,
                            std::uint64_t) noexcept = nullptr;
  bool cachedColdCollapse = false;
  std::uint64_t mailbox = self.mailbox.load(std::memory_order_acquire);

  while (true) {
    if ((mailbox & PoolControl::kShutdownBit) != 0) {
      // Drain any pending active job before exiting; shutdown is signaled
      // by stamping the worker's mailbox with the shutdown bit set.
      if (control.activeJob.load(std::memory_order_acquire) == nullptr) {
        return;
      }
    }

    // Same-line ack protocol: producer publishes new phase with DONE bit
    // CLEAR. The reuse bit (kReuseBit) is set by the producer when this
    // dispatch reuses cached params; the worker observes it and skips
    // reading desc fields, calling the typed runner's cached fn directly.
    // Mask DONE and ACKED out of the phase compare: neither bit is part
    // of dispatch identity, and a stale ACKED carried from a prior
    // cold-collapse race must not hide a real phase change.
    constexpr std::uint64_t kSeenPhaseMask =
        ~(PoolControl::kDoneBit | PoolControl::kAckedBit);
    const std::uint64_t mailboxPhase = mailbox & kSeenPhaseMask;
    const std::uint64_t lastPhase = lastSeenMailbox & kSeenPhaseMask;
    if (mailboxPhase != lastPhase) {
      const bool reuse = (mailbox & PoolControl::kReuseBit) != 0U;
      void *raw =
          reuse && cachedDesc != nullptr ? cachedDesc : self.mailboxDesc;
      bool coldCollapseDispatch = false;
      if (raw != nullptr) {
        auto *desc = static_cast<JobDescriptor *>(raw);
        auto *workerEntry = cachedWorkerEntry;
        bool coldCollapseCapable = cachedColdCollapse;
        if (!reuse || desc != cachedDesc) [[unlikely]] {
          workerEntry = desc->workerEntry;
          coldCollapseCapable = desc->workerStateBase != nullptr;
          cachedDesc = desc;
          cachedWorkerEntry = workerEntry;
          cachedColdCollapse = coldCollapseCapable;
        }
        coldCollapseDispatch =
            coldCollapseCapable && (mailbox & PoolControl::kSkipClaimBit) == 0U;
        if (workerEntry != nullptr) {
          // Pass kReuseBit-aware mailbox to the runner: when set, runner
          // uses TLS cache and skips reading desc fields, eliminating the
          // producer's TLS desc cache-line transit on steady-state
          // repeated calls. Branchless mailbox flags -> rankPacked high
          // bits. Same observable result as ternaries, one fewer
          // dependency in the call's arg.
          static_assert(PoolControl::kReuseBit == (1ULL << 2));
          static_assert(PoolControl::kSkipClaimBit == (1ULL << 3));
          const auto packedFlags = static_cast<std::uint32_t>(
              ((mailbox & PoolControl::kReuseBit) << 29) |
              ((mailbox & PoolControl::kSkipClaimBit) << 27));
          const std::uint64_t generation =
              mailbox & ~(PoolControl::kPhaseStep - 1ULL);
          workerEntry(desc, workerId | packedFlags, generation);
        } else {
          runActiveJob(*desc, workerId);
        }
      }
      // (mailbox & ~kDoneBit) | kDoneBit is identity to (mailbox |
      // kDoneBit) when kDoneBit is already clear in mailbox. The producer
      // publishes new phases with kDoneBit clear (the bit is reserved for
      // the worker's stamp), so this equality holds whenever we reach this
      // branch (i.e. the phase changed).
      const std::uint64_t doneVal = mailbox | PoolControl::kDoneBit;
      if (coldCollapseDispatch) [[unlikely]] {
        // Cold-collapse opt-in dispatches let the producer return as soon
        // as it has satisfied the join via a producer-side mailbox stamp,
        // which means the producer can publish the NEXT dispatch's
        // `publishedMb` before this worker finishes its own stamp. A
        // naive `mailbox.store(doneVal)` would clobber that next-gen
        // mailbox value and deadlock the next dispatch's join. CAS the
        // stamp instead: it commits only when the mailbox is still the
        // value this worker observed. If a later dispatch has overtaken
        // the slot, the CAS fails, `expected` holds the new value, and we
        // re-enter the loop to process that next dispatch. The CAS
        // overhead is gated on the rare path -- only primitives that
        // wired `workerStateBase` (currently parallelFor) pay it.
        std::uint64_t expected = mailbox;
        // Always set `kAckedBit` in the target so the producer's
        // next-dispatch publish-side ack-wait observes the worker's
        // release-store regardless of whether this CAS wins the race
        // against the producer's cold-collapse self-stamp or loses it.
        const std::uint64_t doneAcked = doneVal | PoolControl::kAckedBit;
        if (self.mailbox.compare_exchange_strong(expected, doneAcked,
                                                 std::memory_order_release,
                                                 std::memory_order_acquire)) {
          lastSeenMailbox = doneAcked;
          mailbox = doneAcked;
        } else {
          // CAS lost to another writer (cold-collapse self-stamp, a
          // later publish, or shutdown). `fetch_or(kAckedBit)` sets the
          // ack without clobbering whatever the writer left; an
          // unconditional store would re-overwrite their bits.
          (void)self.mailbox.fetch_or(PoolControl::kAckedBit,
                                      std::memory_order_acq_rel);
          // Keep `lastSeenMailbox` at the OLD `doneAcked` so the next
          // iteration's phase compare fires on the overtaking
          // generation; advancing to the racing value would mark it
          // already seen and the new dispatch would be dropped.
          lastSeenMailbox = doneAcked;
          mailbox = self.mailbox.load(std::memory_order_acquire);
        }
      } else {
        self.mailbox.store(doneVal, std::memory_order_release);
        lastSeenMailbox = doneVal;
        mailbox = self.mailbox.load(std::memory_order_acquire);
      }
      continue;
    }

    // No new work. Spin within the budget, then park on the futex.
    const bool lowLatencyActive =
        control.hotSpinDepth.load(std::memory_order_acquire) != 0U;
    if (lowLatencyActive) {
      const std::uint64_t hotEpoch =
          control.hotSpinEpoch.load(std::memory_order_acquire);
      if (self.hotSpinEpoch.load(std::memory_order_relaxed) != hotEpoch) {
        self.hotSpinEpoch.store(hotEpoch, std::memory_order_release);
      }
    }
    // Under low-latency scope the spin's cycle budget is dead weight: the
    // contract is "do not park", so an iter-cap exit just routes back into
    // the outer `while(true)` and re-enters spin. Skipping the serializing
    // `rdtscp` removes the ride-along stall the worker pays right after
    // stamping the same-line DONE ack and before observing the next
    // published mailbox.
    const std::uint64_t startTsc = lowLatencyActive ? 0ULL : readTsc();
    bool parkRequired = true;
    // Tight-first-N spin on the worker's own mailbox line. Skip PAUSE
    // briefly so the worker can detect a "publish already arrived" case
    // without adding backoff slack, then fall back to PAUSE-spin for the
    // longer-tail case.
    constexpr std::uint32_t kTightProbeIters = 8U;
    for (std::uint32_t iter = 0; iter < kTightProbeIters; ++iter) {
      const std::uint64_t spinMb = self.mailbox.load(std::memory_order_acquire);
      if (spinMb != lastSeenMailbox) {
        mailbox = spinMb;
        parkRequired = false;
        break;
      }
    }
    if (parkRequired && lowLatencyActive) {
      // Low-latency scope explicitly trades CPU burn for dispatch
      // response. Poll without PAUSE here: the guarded caller has already
      // requested a hot worker fleet, and PAUSE adds wake-detection slack
      // on the exact path this scope is meant to tighten.
      constexpr std::uint32_t kLowLatencyPollIters = 512U;
      for (std::uint32_t iter = 0; iter < kLowLatencyPollIters; ++iter) {
        const std::uint64_t spinMb =
            self.mailbox.load(std::memory_order_acquire);
        if (spinMb != lastSeenMailbox) {
          mailbox = spinMb;
          parkRequired = false;
          break;
        }
      }
    }
    if (parkRequired && !lowLatencyActive) {
      // `rdtscp` serializes the pipeline and costs roughly the same as a
      // small batch of pause-load-compare iterations on Zen, so reading
      // the TSC every iter doubled the spin loop body. Sample it every 64
      // iters instead.
      for (std::uint32_t iter = 0; iter < kSpinAfterBulkJob.maxIters; ++iter) {
        cpuRelax();
        const std::uint64_t spinMb =
            self.mailbox.load(std::memory_order_acquire);
        if (spinMb != lastSeenMailbox) {
          mailbox = spinMb;
          parkRequired = false;
          break;
        }
        if ((iter & 63U) == 63U &&
            readTsc() - startTsc >= kSpinAfterBulkJob.maxCycles) {
          break;
        }
      }
    }
    if (!parkRequired) {
      continue;
    }
    if (control.hotSpinDepth.load(std::memory_order_acquire) != 0U) {
      const std::uint64_t hotEpoch =
          control.hotSpinEpoch.load(std::memory_order_acquire);
      if (self.hotSpinEpoch.load(std::memory_order_relaxed) != hotEpoch) {
        self.hotSpinEpoch.store(hotEpoch, std::memory_order_release);
      }
      mailbox = self.mailbox.load(std::memory_order_acquire);
      continue;
    }

    // Capture parking token, re-check source-of-truth, then enter futex
    // wait. The double-check closes the lost-wakeup race: if a wake
    // happens between reading the token and the syscall, the kernel
    // returns EAGAIN immediately because the token's value moved.
    const std::uint32_t parkToken =
        control.futexWord.load(std::memory_order_relaxed);
    const std::uint64_t recheckMb =
        self.mailbox.load(std::memory_order_acquire);
    if (recheckMb != lastSeenMailbox) {
      mailbox = recheckMb;
      continue;
    }
    if ((recheckMb & PoolControl::kShutdownBit) != 0 &&
        control.activeJob.load(std::memory_order_acquire) == nullptr) {
      return;
    }
    self.parks.store(self.parks.load(std::memory_order_relaxed) + 1U,
                     std::memory_order_relaxed);
#ifdef __linux__
    (void)futexWaitPrivate(&control.futexWord, parkToken, nullptr);
#else
    (void)futexWaitPrivate(&control.futexWord, parkToken,
                           static_cast<const void *>(nullptr));
#endif
    self.wakes.store(self.wakes.load(std::memory_order_relaxed) + 1U,
                     std::memory_order_relaxed);
    mailbox = self.mailbox.load(std::memory_order_acquire);
    // Chain-wake propagation (oneTBB private_server.cpp wake_some /
    // propagate_chain_reaction pattern). When this worker's futex_wait
    // returns and the mailbox phase has advanced past `lastSeenMailbox`,
    // the producer dispatched a new generation while we were parked. Wake
    // up to two more parked workers via futex_wake(N=2) so the chain
    // doubles each hop and reaches every parked worker in log2(N)
    // syscalls instead of forcing the producer to issue one INT_MAX
    // broadcast on its critical path. Skip on shutdown -- shutdownAndJoin
    // already does a single INT_MAX broadcast that's sequenced-after the
    // descriptor cleanup, and we don't want a parked worker waking on
    // shutdown to spawn an INT_MAX-equivalent chain.
    //
    // Skip when `participants <= 2`: slot 0 is the producer, so no
    // other peer is parked to propagate to, and the syscall is dead
    // weight on the cold-dispatch budget the producer's join observes.
    const std::uint64_t newPhase = mailbox & ~PoolControl::kDoneBit;
    const std::uint64_t oldPhase = lastSeenMailbox & ~PoolControl::kDoneBit;
    if (newPhase != oldPhase && (mailbox & PoolControl::kShutdownBit) == 0 &&
        control.participants > 2U) {
      (void)futexWakePrivate(&control.futexWord, 2);
    }
  }
}

} // namespace citor::detail

// ===== citor/thread_pool.h =====


#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#endif



namespace citor {

class PoolGroup;

/// Origin tag for a `ThreadPool` instance.
///
/// `Standalone` is the default for user-created pools. `Arena` is set by
/// `PoolGroup` for the per-CCD pools it owns; the `Arena` tag activates the
/// cross-arena deadlock guard in every primitive's dispatch fast path.
enum class PoolKind : std::uint8_t {
  /// User-owned pool with no `PoolGroup` participation.
  Standalone,
  /// Owned by a `PoolGroup` and pinned to one CCD; participates in the
  /// cross-arena guard.
  Arena
};

// Per-instance worker pool owning the lifecycle of background pthreads.
//
// Construction spawns `participants - 1` background pthreads, pins them when
// affinity is requested, and parks them on a shared futex; destruction sets
// the shutdown bit, broadcasts a wake, and joins each pthread. Primitive
// entry points (`parallelFor`, `parallelReduce`, etc.) are member functions
// so they share this lifecycle.
//
// Non-copyable and non-movable: workers carry interior pointers to per-instance
// `WorkerState` slots, so any rebase of the pool's address would invalidate
// them.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
class ThreadPool {
public:
  /// Construct a pool with |participants| total participants (producer +
  /// background). Probes topology, truncates |participants| to at most the
  /// number of physical cores in the process affinity mask, then spawns
  /// `participants - 1` background pthreads pinned per |workerAffinity|.
  ///
  /// |workerAffinity| controls how each worker's CPU mask is configured:
  ///   * `Affinity::PerCpu` -- one CPU per worker. Strict; the kernel cannot
  ///     migrate the worker to any other CPU. Best for HPC / real-time use
  ///     where determinism matters more than the kernel's ability to
  ///     opportunistically rebalance.
  ///   * `Affinity::PerCluster` -- each worker is pinned to its CCD's full
  ///     logical-CPU set; the kernel may migrate within a CCD but not
  ///     across clusters. Preserves CCD-locality of caches while letting
  ///     the kernel route wakes via `select_idle_sibling` and rebalance
  ///     under intra-CCD load. Recommended default for memory-bound
  ///     primitives on multi-CCD parts.
  ///   * `Affinity::None` -- no per-worker pinning; workers inherit the
  ///     producer's process affinity mask. The kernel scheduler is free to
  ///     migrate at will.
  ///
  /// Throws `std::invalid_argument` when |participants| is 0; throws
  /// `std::system_error` when pthread attribute init, stack-size set, or
  /// thread create fails.
  explicit ThreadPool(std::size_t participants,
                      Affinity workerAffinity = Affinity::PerCpu)
      : m_workerAffinity(workerAffinity), m_topology(detail::detectTopology()),
        m_workers(nullptr, WorkerArrayDeleter{}),
        m_chainDoneSlots(nullptr, ChainDoneSlotDeleter{}),
        m_plexDoneSlots(nullptr, PlexDoneSlotDeleter{}) {
    if (participants == 0) {
      throw std::invalid_argument("ThreadPool: participants must be >= 1");
    }
    initWorkers(participants, m_topology.physicalCores);
  }

private:
  /// Tag selecting the per-CCD arena constructor used by `PoolGroup`.
  struct ArenaTag {};

  /// Construct an `Arena` pool pinned to a specific subset of CPU ids. Used
  /// exclusively by `PoolGroup` to spawn one pool per CCD. |cpuPins| overrides
  /// the topology-derived physical-core selection so workers stay inside one
  /// CCD; |arenaIndex| is recorded on every worker's `ThreadContext` so the
  /// cross-arena guard can fall through to inline-on-caller without consulting
  /// the `PoolGroup`.
  ThreadPool(ArenaTag /*tag*/, std::size_t participants,
             const std::vector<std::uint32_t> &cpuPins,
             std::uint32_t arenaIndex)
      : m_kind(PoolKind::Arena), m_arenaIndex(arenaIndex),
        m_topology(detail::detectTopology()),
        m_workers(nullptr, WorkerArrayDeleter{}),
        m_chainDoneSlots(nullptr, ChainDoneSlotDeleter{}),
        m_plexDoneSlots(nullptr, PlexDoneSlotDeleter{}) {
    if (participants == 0) {
      throw std::invalid_argument("ThreadPool: participants must be >= 1");
    }
    initWorkers(participants, cpuPins);
  }

  friend class PoolGroup;

  /// TLS auto-pin state per producer thread. Captured by `autoPinProducerOnce`
  /// on the first `parallelFor` from a thread for a given pool, restored by
  /// `autoPinRestoreIfOurs` from the pool's destructor when the destroying
  /// thread is the same producer. The `pool` field is a stable identity tag
  /// (compared, not dereferenced).
  struct AutoPinState {
#ifdef __linux__
    /// Producer's CPU affinity mask before auto-pin pinned it to slot 0's
    /// reserved CPU. Restored by the pool's destructor.
    cpu_set_t saved{};
#elif defined(_WIN32)
    /// Producer's affinity mask captured at auto-pin time; restored by
    /// the pool destructor. Stored as the Windows-native `uintptr_t`
    /// (DWORD_PTR) so the restore path can hand it straight to
    /// `SetThreadAffinityMask`. Zero means "no save captured."
    std::uintptr_t saved = 0U;
#endif
    /// Stable identity tag for the pool that captured `saved`. Compared
    /// for equality only; never dereferenced.
    const void *pool = nullptr;
    /// True between the auto-pin save and the matching restore.
    bool restorePending = false;
  };
  /// Returns the calling thread's `AutoPinState` slot. Each producer
  /// thread has one; the pool tag in the slot disambiguates which pool
  /// owns the pin.
  static AutoPinState &autoPinTls() noexcept {
    static thread_local AutoPinState s_state;
    return s_state;
  }

  // Pool-side mirror of the auto-pin record. The destructor uses this to
  // restore the producer's affinity when the destroyer thread is not the
  // producer (in which case the TLS path's `state.pool == this` check would
  // fail and the producer would stay pinned past pool lifetime).
#ifdef __linux__
  /// True while this pool owns an active producer pin. The destructor
  /// reads this with acquire semantics to decide whether to restore.
  std::atomic<bool> m_autoPinActive{false};
  /// `pthread_self()` of the producer that was auto-pinned by this pool.
  pthread_t m_autoPinProducer{};
  /// Producer's pre-pin affinity mask, mirrored from
  /// `AutoPinState::saved` so the destructor can restore it from any
  /// thread.
  cpu_set_t m_autoPinSaved{};
#elif defined(_WIN32)
  /// True while this pool owns an active producer pin. Same role as the
  /// Linux flag; destructor reads to decide whether to restore.
  std::atomic<bool> m_autoPinActive{false};
  /// Producer's Windows thread id at auto-pin time. Used by the
  /// destructor to `OpenThread(THREAD_SET_LIMITED_INFORMATION, ...)`
  /// when restore must run from a thread other than the producer.
  /// Zero means "no producer captured."
  std::uint32_t m_autoPinProducerTid = 0U;
  /// Producer's pre-pin affinity mask mirrored from `AutoPinState::saved`
  /// so the destructor can restore from any thread.
  std::uintptr_t m_autoPinSaved = 0U;
#endif

  /// First-call binder: detect whether the producer is already pinned
  /// (single-CPU mask) and leave it alone if so; otherwise save the current
  /// mask and pin to slot 0's reserved CPU. Idempotent per (thread, pool) via
  /// the TLS pool tag.
  [[gnu::cold]] void autoPinProducerOnce() const noexcept {
#ifdef __linux__
    const std::uint32_t cpuId = producerCpu();
    if (cpuId == UINT32_MAX) {
      return;
    }
    auto &state = autoPinTls();
    cpu_set_t curr;
    CPU_ZERO(&curr);
    if (pthread_getaffinity_np(pthread_self(), sizeof(curr), &curr) != 0) {
      return;
    }
    if (CPU_COUNT(&curr) <= 1) {
      // Producer already pinned. If the pin came from an earlier citor pool on
      // this same thread (state.restorePending == true), DO NOT clobber the
      // saved-affinity record; that record is the only path back to the
      // producer's pre-citor mask. We just take ownership of the pin from the
      // prior pool by retaining `state.saved` and updating `state.pool` to
      // ourselves, so this pool's destructor performs the eventual restore. If
      // the pin came from outside citor (state.restorePending == false), leave
      // saved untouched and just record the pool tag so subsequent calls skip
      // the cost.
      state.pool = static_cast<const void *>(this);
      return;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<std::size_t>(cpuId), &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0) {
      // Only capture `saved` when no prior pool has already done so on this
      // thread. Otherwise the prior pool's saved record is the right one to
      // preserve -- the earlier non-pinned mask, not the intermediate
      // single-CPU mask we are replacing.
      if (!state.restorePending) {
        state.saved = curr;
        state.restorePending = true;
      }
      state.pool = static_cast<const void *>(this);
      // Mirror the pin record on the pool so the destructor can restore
      // even when running on a thread other than the producer. Mutable
      // because the auto-pin runs through a `const` accessor; the
      // destructor reads these fields under the synchronous lifetime
      // contract (no in-flight dispatch races destruction).
      auto *self = const_cast<ThreadPool *>(this);
      self->m_autoPinSaved = state.saved;
      self->m_autoPinProducer = pthread_self();
      self->m_autoPinActive.store(true, std::memory_order_release);
    }
#elif defined(_WIN32)
    // Windows peer of the Linux path above; same contract, different syscalls.
    const std::uint32_t cpuId = producerCpu();
    if (cpuId == UINT32_MAX) {
      return;
    }
    auto &state = autoPinTls();
    // Round-trip `SetThreadAffinityMask(thread, ~0)` to capture the
    // caller's affinity; Windows has no read-only accessor. A
    // single-CPU pre-existing pin is restored and left alone.
    HANDLE thisThread = ::GetCurrentThread();
    const DWORD_PTR prevMask = ::SetThreadAffinityMask(
        thisThread, static_cast<DWORD_PTR>(~DWORD_PTR{0}));
    if (prevMask == 0U) {
      // SetThreadAffinityMask failed (process affinity tighter than ~0).
      // Bail without claiming the pool tag so the next call retries.
      return;
    }
    // popcount(prevMask) <= 1 means "thread was already single-CPU
    // pinned." Restore and leave the existing pin in place.
    const bool wasPinned = (prevMask & (prevMask - 1U)) == 0U;
    if (wasPinned) {
      (void)::SetThreadAffinityMask(thisThread, prevMask);
      state.pool = static_cast<const void *>(this);
      return;
    }
    // Apply our pin to slot 0's CPU.
    const DWORD_PTR newMask = static_cast<DWORD_PTR>(1) << (cpuId & 63U);
    if (::SetThreadAffinityMask(thisThread, newMask) == 0U) {
      (void)::SetThreadAffinityMask(thisThread, prevMask);
      return;
    }
    if (!state.restorePending) {
      state.saved = static_cast<std::uintptr_t>(prevMask);
      state.restorePending = true;
    }
    state.pool = static_cast<const void *>(this);
    // Mirror to pool-side so the destructor can restore even from a
    // thread other than the producer.
    auto *self = const_cast<ThreadPool *>(this);
    self->m_autoPinSaved = state.saved;
    self->m_autoPinProducerTid =
        static_cast<std::uint32_t>(::GetCurrentThreadId());
    self->m_autoPinActive.store(true, std::memory_order_release);
#endif
  }

  /// Per-thread shared `JobDescriptor` storage used by `parallelFor` and
  /// `dispatchReduceJobTyped`. Hoisted out of the template so distinct
  /// lambda types do not each create their own TLS descriptor (256 B
  /// each, aligned to 128 B). With many unique lambdas the
  /// per-template-instantiation cost showed up as TLS bloat across
  /// long-lived threads. The desc is type-erased (its `body` is a
  /// `FunctionRef`), so sharing across instantiations is safe.
  [[gnu::always_inline]] static detail::JobDescriptor &
  sharedParallelForDesc() noexcept {
    thread_local detail::JobDescriptor desc;
    return desc;
  }

  /// Per-thread shared `JobDescriptor` for reduce dispatches. Same TLS
  /// sharing rationale as `sharedParallelForDesc`; kept on a separate
  /// instance so reduce and `parallelFor` do not stomp each other when a
  /// chained call sequence reuses the cached `desc` between dispatches.
  [[gnu::always_inline]] static detail::JobDescriptor &
  sharedReduceDesc() noexcept {
    thread_local detail::JobDescriptor desc;
    return desc;
  }

  /// Hot-path TLS gate: a quick `state.pool == this` check, fall through to
  /// the cold pinner only on a pool transition. Used by every chunk==0
  /// primitive so the syscall fires once per (thread, pool) regardless of
  /// which primitive made the first call.
  [[gnu::always_inline]] void
  ensureProducerPinnedForChunkZero() const noexcept {
    auto &state = autoPinTls();
    if (state.pool != static_cast<const void *>(this)) [[unlikely]] {
      autoPinProducerOnce();
    }
  }

  /// Pool-destruction restore: undoes the auto-pin recorded by
  /// `autoPinProducerOnce`. Uses the pool-side mirror (`m_autoPinProducer`,
  /// `m_autoPinSaved`) so the restore lands on the producer thread regardless
  /// of which thread invokes the destructor. The TLS record is cleared as
  /// well when the destroyer happens to be the producer, so a subsequent
  /// pool ctor on the same thread sees a clean state.
  [[gnu::cold, gnu::noinline]] void autoPinRestoreIfOurs() const noexcept {
    // Restoration goes through the pool-side `m_autoPinProducer` /
    // `m_autoPinSaved` mirror so it lands on the producer regardless
    // of which thread runs the destructor. The TLS cleanup below is
    // the only step that must check ownership: under multi-pool LIFO
    // a later pool may have stolen the TLS tag and owns it now.
    auto &state = autoPinTls();
    const bool weOwnTlsTag = state.pool == static_cast<const void *>(this);
#ifdef __linux__
    if (m_autoPinActive.load(std::memory_order_acquire)) {
      (void)pthread_setaffinity_np(m_autoPinProducer, sizeof(m_autoPinSaved),
                                   &m_autoPinSaved);
    }
    if (weOwnTlsTag) {
      state.restorePending = false;
      state.pool = nullptr;
    }
#elif defined(_WIN32)
    if (m_autoPinActive.load(std::memory_order_acquire) &&
        m_autoPinProducerTid != 0U && m_autoPinSaved != 0U) {
      // Best-effort restore via `OpenThread(THREAD_SET_LIMITED_INFORMATION)`.
      // Windows recycles TIDs, so a missing producer skips restore
      // rather than risk clobbering an unrelated thread.
      HANDLE handle = ::OpenThread(THREAD_SET_LIMITED_INFORMATION, FALSE,
                                   m_autoPinProducerTid);
      if (handle != nullptr) {
        (void)::SetThreadAffinityMask(handle,
                                      static_cast<DWORD_PTR>(m_autoPinSaved));
        ::CloseHandle(handle);
      }
    }
    if (weOwnTlsTag) {
      state.restorePending = false;
      state.pool = nullptr;
    }
#endif
  }

  /// Producer-CCD-first reorder without SMT exceptions. Standalone
  /// pools spawn workers on slot 0's CCD so the producer auto-pin
  /// makes user buffers first-touch there; `preferredCcd` keeps the
  /// choice deterministic on V-Cache parts. Arena pools skip the
  /// reorder because `PoolGroup` already filtered the pin list.
  std::vector<std::uint32_t>
  reserveProducerCpuFirstCcdOnly(const std::vector<std::uint32_t> &cpuPins,
                                 bool standalone) const {
    std::vector<std::uint32_t> pins = cpuPins;
    if (!standalone || pins.size() <= 1U) {
      return pins;
    }
    const std::uint32_t targetCcd = m_topology.preferredCcd;
    if (targetCcd >= m_topology.ccdGroups.size()) {
      return pins;
    }
    std::vector<std::uint32_t> reordered;
    reordered.reserve(pins.size());
    for (const std::uint32_t cpu : pins) {
      if (cpu < m_topology.ccdOfCpu.size() &&
          m_topology.ccdOfCpu[cpu] == targetCcd) {
        reordered.push_back(cpu);
      }
    }
    for (const std::uint32_t cpu : pins) {
      if (cpu >= m_topology.ccdOfCpu.size() ||
          m_topology.ccdOfCpu[cpu] != targetCcd) {
        reordered.push_back(cpu);
      }
    }
    if (reordered.size() == pins.size()) {
      pins = std::move(reordered);
    }
    return pins;
  }

  /// Shared body for both constructors: spawn |participants| workers pinned to
  /// |cpuPins|. Placement of the participant pins is delegated to
  /// `detail::reserveProducerCpuFirst`; the function handles the producer-CCD
  /// reorder for standalone pools and applies the SMT-aware exceptions
  /// (slot 1 = producer's SMT sibling at j=2, SMT-sibling overflow when
  /// oversubscribed). The SMT-aware exceptions are gated on
  /// `Affinity::PerCpuSmtPair`; every other `Affinity` value keeps the
  /// distinct-physical-core layout the caller supplied.
  void initWorkers(std::size_t participants,
                   const std::vector<std::uint32_t> &cpuPins) {
    const bool standalone = m_kind == PoolKind::Standalone;
    std::vector<std::uint32_t> participantPins;
    if (m_workerAffinity == Affinity::PerCpuSmtPair) {
      participantPins = detail::reserveProducerCpuFirst(cpuPins, participants,
                                                        standalone, m_topology);
    } else {
      participantPins = reserveProducerCpuFirstCcdOnly(cpuPins, standalone);
    }
    std::size_t maxByTopology = participants;
    if (!participantPins.empty()) {
      maxByTopology = participantPins.size();
    } else if (m_topology.physicalCount > 0U) {
      maxByTopology = m_topology.physicalCount;
    }
    const std::size_t effective =
        participants <= maxByTopology ? participants : maxByTopology;

    m_control.participants = static_cast<std::uint32_t>(effective);
    // Pre-compute the join scan's pending bitmask. The bitmask join path is
    // gated on `participants <= 64` at the call site; for larger pools the
    // mask is unused and join falls back to the per-slot scan over
    // `m_workers`. The construction below clamps to 64 bits to avoid the
    // `1ULL << effective` shift becoming UB when `effective > 64` on
    // many-core machines. Producer slot 0 is excluded so workers `[1, n)`
    // are the only bits set.
    if (effective >= 64U) {
      m_control.pendingMaskBits = ~std::uint64_t{0} & ~std::uint64_t{1};
    } else {
      m_control.pendingMaskBits =
          ((std::uint64_t{1} << effective) - 1U) & ~std::uint64_t{1};
    }

    void *raw = ::operator new(sizeof(detail::WorkerState) * effective,
                               std::align_val_t{kCacheLine});
    auto *first = static_cast<detail::WorkerState *>(raw);
    for (std::size_t i = 0; i < effective; ++i) {
      ::new (static_cast<void *>(first + i)) detail::WorkerState();
    }
    m_workers = std::unique_ptr<detail::WorkerState, WorkerArrayDeleter>(
        first, WorkerArrayDeleter{effective});

    // Pre-allocate the per-worker `done` slot block reused by every
    // `parallelChain` dispatch. The block is owned by the pool so chain calls
    // pay no allocator round trip on the dispatch hot path; each call
    // zero-resets the slots at entry to honour the chain's fresh-epoch
    // contract.
    void *rawSlots = ::operator new(sizeof(detail::ChainDoneSlot) * effective,
                                    std::align_val_t{kCacheLine});
    auto *firstSlot = static_cast<detail::ChainDoneSlot *>(rawSlots);
    for (std::size_t i = 0; i < effective; ++i) {
      ::new (static_cast<void *>(firstSlot + i)) detail::ChainDoneSlot();
    }
    m_chainDoneSlots =
        std::unique_ptr<detail::ChainDoneSlot, ChainDoneSlotDeleter>(
            firstSlot, ChainDoneSlotDeleter{effective});

    // Pre-allocate the per-worker plex `done` slot block. Mirrors the chain
    // block; each plex call borrows it through `state.doneSlots` and reserves a
    // fresh interval in `m_plexEpochBase`, so calls never `operator new` /
    // zero-reset on the dispatch hot path.
    void *rawPlexSlots = ::operator new(
        sizeof(detail::PlexDoneSlot) * effective, std::align_val_t{kCacheLine});
    auto *firstPlexSlot = static_cast<detail::PlexDoneSlot *>(rawPlexSlots);
    for (std::size_t i = 0; i < effective; ++i) {
      ::new (static_cast<void *>(firstPlexSlot + i)) detail::PlexDoneSlot();
    }
    m_plexDoneSlots =
        std::unique_ptr<detail::PlexDoneSlot, PlexDoneSlotDeleter>(
            firstPlexSlot, PlexDoneSlotDeleter{effective});

    for (std::size_t i = 0; i < effective; ++i) {
      auto *w = m_workers.get() + i;
      w->workerId = static_cast<std::uint32_t>(i);
      const std::uint32_t cpu =
          (i < participantPins.size()) ? participantPins[i] : UINT32_MAX;
      w->cpuId = cpu;
      if (cpu != UINT32_MAX && cpu < m_topology.ccdOfCpu.size()) {
        w->ccdId = m_topology.ccdOfCpu[cpu];
      }
    }

    // Per-worker Chase-Lev work-stealing deques. Allocated up-front (one per
    // participant) so the forkJoin steal probe pays no allocator round-trip on
    // its hot path. Each deque stores `Task *` payloads pointing at descriptors
    // that live on the producer's stack (root tasks) or on a recursive worker's
    // stack (children spawned during a body).
    m_workerDeques.reserve(effective);
    for (std::size_t i = 0; i < effective; ++i) {
      m_workerDeques.emplace_back(
          std::make_unique<detail::ChaseLevDeque<detail::Task *>>());
    }

    // Cache the per-slot CCD index in a contiguous array so the forkJoin
    // victim-selection probe skips the indirection through `WorkerState::ccdId`
    // on the steal hot path.
    m_ccdOfSlot.assign(effective, UINT32_MAX);
    for (std::size_t i = 0; i < effective; ++i) {
      m_ccdOfSlot[i] = (m_workers.get() + i)->ccdId;
    }

    // Cache background-worker CPU ids for `bindProducerSlot`'s exclusion logic.
    // Built once at construction so the producer-side `ProducerAffinityGuard`
    // does not allocate per scope.
    m_workerCpus.clear();
    m_workerCpus.reserve(effective > 0 ? effective - 1 : 0);
    for (std::size_t i = 1; i < effective; ++i) {
      const std::uint32_t cpu = (m_workers.get() + i)->cpuId;
      if (cpu != UINT32_MAX) {
        m_workerCpus.push_back(cpu);
      }
    }

    // Precompute steal-victim lists for `forkJoin::trySteal`: the same-CCD ring
    // (used when the call requests CCD-local affinity), the cross-CCD ring (the
    // CCD-local fallback), and the all-victim ring (used when the call did not
    // request CCD-local affinity). Storing slot indices directly removes the
    // per-step `% participants` modulo from the inner steal loop and the
    // per-step CCD comparison; the cross-CCD ring complements the same-CCD ring
    // so the CCD-local fallback never re-probes victims the same-CCD pass
    // already exhausted.
    m_sameCcdVictims.assign(effective, {});
    m_crossCcdVictims.assign(effective, {});
    m_allVictims.assign(effective, {});
    for (std::size_t self = 0; self < effective; ++self) {
      const std::uint32_t selfCcd = m_ccdOfSlot[self];
      m_sameCcdVictims[self].reserve(effective - 1);
      m_crossCcdVictims[self].reserve(effective - 1);
      m_allVictims[self].reserve(effective - 1);
      for (std::size_t v = 0; v < effective; ++v) {
        if (v == self) {
          continue;
        }
        m_allVictims[self].push_back(static_cast<std::uint32_t>(v));
        if (selfCcd != UINT32_MAX && m_ccdOfSlot[v] == selfCcd) {
          m_sameCcdVictims[self].push_back(static_cast<std::uint32_t>(v));
        } else {
          m_crossCcdVictims[self].push_back(static_cast<std::uint32_t>(v));
        }
      }
    }

    if (effective <= 1) {
      return;
    }
#ifdef __linux__
    m_workerThreads.resize(effective - 1);
#endif
    m_workerSpawnArgs.reserve(effective - 1);
    for (std::size_t i = 1; i < effective; ++i) {
      auto &arg = m_workerSpawnArgs.emplace_back();
      arg.pool = this;
      arg.workerIndex = i;
    }

#ifdef __linux__
    pthread_attr_t attrs;
    const int initRc = pthread_attr_init(&attrs);
    if (initRc != 0) {
      shutdownAndJoin();
      throw std::system_error(initRc, std::generic_category(),
                              "ThreadPool: pthread_attr_init");
    }
    // Worker pthread stack size. Build-time configurable via the
    // `CITOR_WORKER_STACK_KIB` CMake option (default 1024 KiB). Sized for
    // recursive forkJoin bodies under TSan/ASan shadow stacks
    // -- a 256 KiB default historically overflowed UTS-style workloads under
    // sanitizer. Production builds with shallow recursion can drop this knob to
    // 256. The attribute is set once and reused for every worker; non-zero
    // return is propagated as `std::system_error` rather than silently ignored.
    // `pthread_attr_setstacksize` may reject sub-`PTHREAD_STACK_MIN` or
    // non-page-multiple sizes, so we surface the rc.
#ifndef CITOR_WORKER_STACK_KIB
#define CITOR_WORKER_STACK_KIB 8192
#endif
    const std::size_t kWorkerStackBytes =
        std::size_t{CITOR_WORKER_STACK_KIB} * 1024U;
    const int stackRc = pthread_attr_setstacksize(&attrs, kWorkerStackBytes);
    if (stackRc != 0) {
      (void)pthread_attr_destroy(&attrs);
      shutdownAndJoin();
      throw std::system_error(stackRc, std::generic_category(),
                              "ThreadPool: pthread_attr_setstacksize");
    }
    // Pre-bind worker affinity in pthread_attr before `pthread_create` so the
    // kernel schedules the new thread directly onto its target CPU. The
    // pre-existing `bindAffinityOnce` in `workerEntry` then becomes a no-op
    // confirmation.
    for (std::size_t i = 1; i < effective; ++i) {
      pthread_attr_t localAttrs;
      const int locInitRc = pthread_attr_init(&localAttrs);
      if (locInitRc != 0) {
        (void)pthread_attr_destroy(&attrs);
        shutdownAndJoin();
        throw std::system_error(locInitRc, std::generic_category(),
                                "ThreadPool: pthread_attr_init");
      }
      const int locStackRc =
          pthread_attr_setstacksize(&localAttrs, kWorkerStackBytes);
      if (locStackRc != 0) {
        (void)pthread_attr_destroy(&localAttrs);
        (void)pthread_attr_destroy(&attrs);
        shutdownAndJoin();
        throw std::system_error(locStackRc, std::generic_category(),
                                "ThreadPool: pthread_attr_setstacksize");
      }
      const std::uint32_t cpu = (m_workers.get() + i)->cpuId;
      if (cpu != UINT32_MAX && m_workerAffinity != Affinity::None) {
        cpu_set_t affSet;
        CPU_ZERO(&affSet);
        if (m_workerAffinity == Affinity::PerCluster) {
          // Pin to every CPU in this worker's CCD (shared L3 group). The
          // kernel can migrate the worker within the CCD but not across
          // clusters; preserves CCD locality while letting `wake_affine`
          // and idle-balance opportunistically rebalance under transient
          // intra-CCD load.
          const std::uint32_t ccdIdx = (m_workers.get() + i)->ccdId;
          if (ccdIdx != UINT32_MAX && ccdIdx < m_topology.ccdGroups.size() &&
              !m_topology.ccdGroups[ccdIdx].empty()) {
            for (auto c : m_topology.ccdGroups[ccdIdx]) {
              CPU_SET(c, &affSet);
            }
          } else {
            CPU_SET(cpu, &affSet);
          }
        } else {
          // Affinity::PerCpu (strict, single-CPU pin). Original behaviour.
          CPU_SET(cpu, &affSet);
        }
        const int affRc =
            pthread_attr_setaffinity_np(&localAttrs, sizeof(affSet), &affSet);
        if (affRc != 0) {
          (void)pthread_attr_destroy(&localAttrs);
          (void)pthread_attr_destroy(&attrs);
          shutdownAndJoin();
          throw std::system_error(affRc, std::generic_category(),
                                  "ThreadPool: pthread_attr_setaffinity_np");
        }
      }
      const int rc =
          pthread_create(&m_workerThreads[i - 1], &localAttrs,
                         &ThreadPool::workerEntry, &m_workerSpawnArgs[i - 1]);
      (void)pthread_attr_destroy(&localAttrs);
      if (rc != 0) {
        m_workerThreads.resize(i - 1);
        m_workerSpawnArgs.resize(i - 1);
        (void)pthread_attr_destroy(&attrs);
        shutdownAndJoin();
        throw std::system_error(rc, std::generic_category(),
                                "ThreadPool: pthread_create");
      }
    }
    (void)pthread_attr_destroy(&attrs);
#else
    m_fallbackThreads.reserve(effective - 1);
    for (std::size_t i = 1; i < effective; ++i) {
      try {
        m_fallbackThreads.emplace_back(&ThreadPool::workerEntryStdThread, this,
                                       i);
      } catch (...) {
        shutdownAndJoin();
        throw;
      }
    }
#endif
    // Constructor barrier: spin until every worker has finished its trampoline
    // and is committed to entering `workerMainLoop`. Without this, the first
    // dispatch's join can wait on a worker that has not yet been scheduled past
    // its startup, which inflates cold-fan-out latency.
    const auto workersExpected = static_cast<std::uint32_t>(effective - 1);
    while (m_workersReady.load(std::memory_order_acquire) < workersExpected) {
      detail::cpuRelax();
    }
    // Pin the constructor (= producer) to slot 0's CPU NOW, before the caller
    // has a chance to allocate workload buffers. Otherwise the buffers
    // first-touch on whichever CPU the kernel happened to schedule the
    // constructor on, while workers may sit on a different CCD; on a 2-CCD
    // host that thrashes the IO die for every cache line and the workload
    // stalls 5-10x. Standalone pools only -- Arena pools are owned by
    // PoolGroup and the producer is shared across arenas, so a per-arena pin
    // would be wrong.
    if (m_kind == PoolKind::Standalone) {
      autoPinProducerOnce();
    }
    // One-time coherence probe. Measures the constant factor by which a
    // cache-line ping-pong between two cores on different CCDs is slower
    // than a ping-pong between two cores on the same CCD. Primitives that
    // benefit from CCD-aware partitioning (`parallelScan`, future
    // tile-decoupled scans, ...) read this ratio to size their cross-CCD
    // share without any hardware-specific constants in the engine.
    //
    // Gate the probe on `effective > 2`. The earlier `ccdCount > 1`
    // gate hid hybrid single-L3 Intel parts whose P/E split is visible
    // only through the latency matrix; the new gate still excludes
    // single-worker pools (no pairs) and arenas (PoolGroup owns the
    // cross-arena cost model).
    if (m_kind == PoolKind::Standalone && effective > 2U) {
      // Build the flat CPU list the probe should walk: producer CPU plus
      // every worker's pinned CPU. Skip CPUs we could not pin (UINT32_MAX
      // sentinel from `initWorkers` on a permission-restricted host) so
      // the probe never tries to thread-spawn on a CPU it cannot reach.
      std::vector<std::uint32_t> probeCpus;
      probeCpus.reserve(effective);
      for (std::size_t i = 0; i < effective; ++i) {
        const std::uint32_t cpu = (m_workers.get() + i)->cpuId;
        if (cpu != UINT32_MAX) {
          probeCpus.push_back(cpu);
        }
      }
      // 1024 round-trips per pair amortises ping-pong jitter; the probe
      // runs in N-1 disjoint-pair-parallel rounds so wall time scales
      // with (cpus - 1) * single-pair-probe-time. The cached variant
      // returns an existing matrix for identical `probeCpus` sets so
      // test suites and bench harnesses that build many pools pay the
      // probe cost once.
      m_coherenceProbe =
          detail::cachedCoherenceProbe(probeCpus, m_topology.ccdGroups);
    }
    // Pre-resolve the parallelScan-specific topology fields. Inputs are all
    // pool-immutable post-ctor (slot CCDs, cpu ids, probe cluster ids and
    // ratio); resolving them here turns the per-call topology resolution in
    // `runScanParallel` into O(1) field reads.
    initScanScratch(effective);
    // Best-effort lock the WorkerState array into RAM. The hot-path `mailbox` /
    // `doneEpoch` lines must never page-fault: a fault on the producer's
    // acquire-spin would replace cache traffic with a kernel round-trip and the
    // join would silently stall. The locked region is small enough for normal
    // process limits. EPERM (no CAP_IPC_LOCK) and ENOMEM (rlimit exhausted) are
    // non-fatal: the pool runs correctly without the lock, just exposed to the
    // page-fault tail risk.
#ifdef __linux__
    if (m_workers != nullptr) {
      (void)mlock(m_workers.get(), sizeof(detail::WorkerState) * effective);
    }
#elif defined(_WIN32)
    // Windows peer of `mlock`. `VirtualLock` ensures the page is resident
    // in physical RAM so a worker that wakes mid-dispatch never pays a
    // page-fault penalty observing its mailbox. The locked region is
    // small (a few cache lines per worker); failures (e.g. process
    // working-set quota too tight) are non-fatal.
    if (m_workers != nullptr) {
      (void)::VirtualLock(m_workers.get(),
                          sizeof(detail::WorkerState) * effective);
    }
#endif
  }

  /// Pre-resolve the parallelScan-specific topology fields. Inputs are all
  /// pool-immutable post-ctor: the per-slot CCD vector, each worker's
  /// `cpuId`, the coherence probe's matrix CPU list, the probe's per-CPU
  /// cluster ids, and the probe's max cross-over-intra ratio. Output is
  /// stored on the pool so the scan path can replace its per-call topology
  /// resolution (slot -> cluster mapping, contiguity check, asymmetric-bias
  /// derivation) with O(1) field reads.
  void initScanScratch(std::size_t participants) noexcept {
    m_scanClusterIdOfSlot.assign(participants, 0U);
    m_scanClusterFirstSlot.clear();
    m_scanClusterSlotCount.clear();
    m_scanNumClusters = 0;
    m_scanUseHierarchical = false;
    m_scanAsymmetricNum = 8U;
    m_scanProducerCcd = UINT32_MAX;
    m_scanSlotsOnProducerCcd = 0;

    if (m_ccdOfSlot.empty()) {
      return;
    }
    const std::uint32_t producerCcd = m_ccdOfSlot[0];
    std::uint32_t slotsOnProducer = 0;
    bool foundCross = false;
    for (std::size_t s = 0; s < participants; ++s) {
      if (s < m_ccdOfSlot.size() && m_ccdOfSlot[s] == producerCcd) {
        ++slotsOnProducer;
      } else {
        foundCross = true;
      }
    }
    m_scanProducerCcd = producerCcd;
    m_scanSlotsOnProducerCcd = slotsOnProducer;

    if (foundCross && slotsOnProducer > 0U && slotsOnProducer < participants) {
      const std::uint32_t crossSlots =
          static_cast<std::uint32_t>(participants) - slotsOnProducer;
      const double biasFactor =
          m_coherenceProbe.valid &&
                  m_coherenceProbe.maxCrossOverIntraRatio > 1.0
              ? m_coherenceProbe.maxCrossOverIntraRatio
              : 2.0;
      const auto producerWeight = static_cast<std::uint64_t>(
          std::llround(biasFactor * static_cast<double>(slotsOnProducer)));
      const std::uint64_t totalWeight =
          producerWeight + static_cast<std::uint64_t>(crossSlots);
      std::uint32_t derived = 8U;
      if (totalWeight > 0U) {
        derived =
            static_cast<std::uint32_t>((16ULL * producerWeight) / totalWeight);
      }
      m_scanAsymmetricNum =
          std::clamp(derived, std::uint32_t{9U}, std::uint32_t{15U});
    }

    // Hierarchical requires: probe found multiple clusters, enough
    // participants to amortise the per-cluster reduce, and crucially that
    // slots actually span more than one CCD. The last gate guards against
    // false-positive clustering when every worker sits on a single CCD --
    // a 4-CPU same-CCD probe has occasionally clustered into 2-3 spurious
    // groups due to per-pair latency jitter, and routing the scan through
    // the multi-cluster path under that condition deadlocks on the exception
    // path (a non-leader cluster's leader can throw before publishing its
    // cluster stamp, blocking the producer's cross-cluster wait).
    if (!m_coherenceProbe.valid || m_coherenceProbe.clusters.numClusters < 2U ||
        participants < 4U || !foundCross || slotsOnProducer >= participants) {
      return;
    }

    bool resolveOk = true;
    for (std::size_t s = 0; s < participants; ++s) {
      const std::uint32_t cpuId = (m_workers.get() + s)->cpuId;
      std::uint32_t cpuIdx = UINT32_MAX;
      for (std::size_t i = 0; i < m_coherenceProbe.matrix.cpus.size(); ++i) {
        if (m_coherenceProbe.matrix.cpus[i] == cpuId) {
          cpuIdx = static_cast<std::uint32_t>(i);
          break;
        }
      }
      if (cpuIdx == UINT32_MAX) {
        resolveOk = false;
        break;
      }
      m_scanClusterIdOfSlot[s] =
          m_coherenceProbe.clusters.clusterIdOfCpuIndex[cpuIdx];
    }
    if (!resolveOk) {
      m_scanClusterIdOfSlot.assign(participants, 0U);
      return;
    }

    const std::uint32_t numClusters = m_coherenceProbe.clusters.numClusters;
    m_scanClusterFirstSlot.assign(numClusters, UINT32_MAX);
    m_scanClusterSlotCount.assign(numClusters, 0U);
    for (std::size_t s = 0; s < participants; ++s) {
      const auto k = m_scanClusterIdOfSlot[s];
      if (k >= numClusters) {
        return;
      }
      if (m_scanClusterFirstSlot[k] == UINT32_MAX) {
        m_scanClusterFirstSlot[k] = static_cast<std::uint32_t>(s);
      }
      ++m_scanClusterSlotCount[k];
    }
    bool contiguous = true;
    for (std::uint32_t k = 0; k < numClusters; ++k) {
      if (m_scanClusterSlotCount[k] == 0U) {
        continue;
      }
      const std::uint32_t first = m_scanClusterFirstSlot[k];
      for (std::uint32_t i = 0; i < m_scanClusterSlotCount[k]; ++i) {
        if (first + i >= participants ||
            m_scanClusterIdOfSlot[first + i] != k) {
          contiguous = false;
          break;
        }
      }
      if (!contiguous) {
        break;
      }
    }
    std::uint32_t nonEmpty = 0;
    for (std::uint32_t k = 0; k < numClusters; ++k) {
      if (m_scanClusterSlotCount[k] > 0U) {
        ++nonEmpty;
      }
    }
    const bool producerInClusterZero = m_scanClusterIdOfSlot[0] == 0U;
    if (contiguous && nonEmpty >= 2U && producerInClusterZero) {
      m_scanUseHierarchical = true;
      m_scanNumClusters = numClusters;
    }
  }

public:
  // Scoped affinity guard for the caller acting as the pool's producer slot.
  //
  // Workers are pinned to participant slots 1..N. Slot 0 is the caller, so a
  // caller that wants full-core participation without displacing a pinned
  // worker can bind itself to the CPU reserved for slot 0 outside a hot
  // dispatch loop. The guard restores the previous affinity mask on
  // destruction.
  class ProducerAffinityGuard {
  public:
    ProducerAffinityGuard() noexcept = default;

    /// Saves the caller's affinity mask and pins the caller to `|cpuId|`
    /// (or, when SMT siblings are exposed, to the same-CCD subset of the
    /// caller's mask excluding `|workerCpus|`). The destructor restores
    /// the saved mask. `|workerCpus|` and `|workerCpuCount|` describe the
    /// pool's worker pin list so the producer never displaces a hot
    /// worker.
    ProducerAffinityGuard(std::uint32_t cpuId, const detail::Topology &topo,
                          const std::uint32_t *workerCpus,
                          std::size_t workerCpuCount) noexcept {
#ifdef __linux__
      if (cpuId == UINT32_MAX) {
        return;
      }
      CPU_ZERO(&m_saved);
      if (pthread_getaffinity_np(pthread_self(), sizeof(m_saved), &m_saved) !=
          0) {
        return;
      }
      // When the target CPU's SMT sibling is also inside the current mask, the
      // SMT pair is closed (no out-of-mask kthread can land on the sibling).
      // Pin to the single CPU. Otherwise the sibling is exposed to
      // kthreads/IRQs and a single-CPU pin would be bimodal under SMT
      // contention. Fall back to the same-CCD subset of the current mask,
      // excluding worker-pinned CPUs so the producer never displaces a
      // hot-spinning worker off its own CPU while the kernel migrates the
      // producer freely inside the worker-free CCD subset to dodge transient
      // SMT collisions.
      const std::vector<std::uint32_t> siblings = detail::readCpuList(
          "/sys/devices/system/cpu/cpu" + std::to_string(cpuId) +
          "/topology/thread_siblings_list");
      bool siblingExposed = false;
      cpu_set_t set;
      for (const std::uint32_t sib : siblings) {
        if (sib == cpuId) {
          continue;
        }
        if (CPU_ISSET(static_cast<std::size_t>(sib), &m_saved)) {
          continue;
        }
        siblingExposed = true;
        break;
      }
      CPU_ZERO(&set);
      if (!siblingExposed) {
        CPU_SET(static_cast<std::size_t>(cpuId), &set);
      } else if (cpuId < topo.ccdOfCpu.size()) {
        const std::uint32_t ccd = topo.ccdOfCpu[cpuId];
        if (ccd < topo.ccdGroups.size()) {
          for (const std::uint32_t peer : topo.ccdGroups[ccd]) {
            if (!CPU_ISSET(static_cast<std::size_t>(peer), &m_saved)) {
              continue;
            }
            bool isWorkerPin = false;
            for (std::size_t i = 0; i < workerCpuCount; ++i) {
              if (workerCpus[i] == peer) {
                isWorkerPin = true;
                break;
              }
            }
            if (isWorkerPin) {
              continue;
            }
            CPU_SET(static_cast<std::size_t>(peer), &set);
          }
        }
        if (CPU_COUNT(&set) == 0) {
          // Every CCD peer is either out-of-mask or worker-pinned; fall back to
          // the single-CPU pin on the producer's reserved slot.
          CPU_SET(static_cast<std::size_t>(cpuId), &set);
        }
      } else {
        return;
      }
      if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0) {
        m_restore = true;
      }
#elif defined(_WIN32)
      // Windows pin: single-CPU pin on slot 0's reserved CPU. The Linux
      // branch's SMT-sibling exclusion is skipped because the Windows
      // topology probe does not populate per-CPU sibling tables; slot 0's
      // CPU is producer-reserved, so the worker-pin and CCD-subset paths
      // above do not apply here.
      (void)topo;
      (void)workerCpus;
      (void)workerCpuCount;
      if (cpuId == UINT32_MAX) {
        return;
      }
      const DWORD_PTR saved = detail::pinCurrentThreadAndSave(cpuId);
      if (saved != 0U) {
        m_savedMask = static_cast<std::uintptr_t>(saved);
      }
#else
      (void)cpuId;
      (void)topo;
      (void)workerCpus;
      (void)workerCpuCount;
#endif
    }

    ~ProducerAffinityGuard() {
#ifdef __linux__
      if (m_restore) {
        (void)pthread_setaffinity_np(pthread_self(), sizeof(m_saved), &m_saved);
      }
#elif defined(_WIN32)
      if (m_savedMask != 0U) {
        // Restore on the same thread that constructed the guard. The
        // guard is non-movable / non-copyable so the dtor runs on the
        // producer; `GetCurrentThread()` is the producer's thread here.
        (void)::SetThreadAffinityMask(::GetCurrentThread(),
                                      static_cast<DWORD_PTR>(m_savedMask));
      }
#endif
    }

    ProducerAffinityGuard(const ProducerAffinityGuard &) = delete;
    ProducerAffinityGuard &operator=(const ProducerAffinityGuard &) = delete;
    ProducerAffinityGuard(ProducerAffinityGuard &&) = delete;
    ProducerAffinityGuard &operator=(ProducerAffinityGuard &&) = delete;

  private:
#ifdef __linux__
    /// Caller's affinity mask captured at construction; restored by the
    /// destructor when `m_restore` is true.
    cpu_set_t m_saved{};
    /// True when the constructor successfully applied a new pin and the
    /// destructor must restore `m_saved`.
    bool m_restore = false;
#elif defined(_WIN32)
    /// Caller's affinity mask captured at construction; restored by the
    /// destructor. Stored as the Windows-native `DWORD_PTR` so the
    /// restore path can hand it straight to `SetThreadAffinityMask`.
    /// Zero means "no save captured" (constructor failed or skipped).
    std::uintptr_t m_savedMask = 0U;
#endif
  };

  /// Per-thread owner pointer for `LowLatencyGuard`, naming the pool
  /// whose guard this thread holds. `DispatchLease`'s skip-gate fires
  /// only when the calling thread owns LL on the SAME pool the
  /// dispatch targets; the LL contract is single-producer per pool, so
  /// a thread holding LL on pool A must still take pool B's gate.
  /// Same-pool nesting is preserved through `hotSpinDepth`.
  static ThreadPool *&lowLatencyOwnerPoolTls() noexcept {
    // NOLINTNEXTLINE(misc-const-correctness)
    static thread_local ThreadPool *s_pool = nullptr;
    return s_pool;
  }

  // Scoped mode for latency-sensitive producer loops.
  //
  // While at least one guard is alive, idle workers keep spinning instead of
  // parking and dispatch skips the per-call futex wake. The constructor wakes
  // parked workers once; the destructor restores the normal spin-then-park
  // policy.
  class LowLatencyGuard {
  public:
    LowLatencyGuard() noexcept = default;

    /// Engages low-latency mode on `|pool|`. Bumps the pool's hot-spin
    /// epoch, wakes parked workers, and waits for every worker to
    /// acknowledge the new epoch. The destructor restores the normal
    /// spin-then-park policy.
    explicit LowLatencyGuard(ThreadPool &pool) noexcept : m_pool(&pool) {
      m_prevOwnerPool = lowLatencyOwnerPoolTls();
      lowLatencyOwnerPoolTls() = &pool;
      auto &control = m_pool->m_control;
      const std::uint64_t epoch =
          control.hotSpinEpoch.fetch_add(1U, std::memory_order_acq_rel) + 1U;
      control.hotSpinDepth.fetch_add(1U, std::memory_order_acq_rel);
      const auto wakeWorkers = [&control]() noexcept {
        const std::uint32_t nextFutex =
            control.futexWord.load(std::memory_order_relaxed) + 1U;
        control.futexWord.store(nextFutex, std::memory_order_release);
        (void)detail::futexWakePrivate(&control.futexWord, INT_MAX);
      };
      wakeWorkers();
      auto *workers = m_pool->m_workers.get();
      const std::uint32_t participants = control.participants;
      // Workers store the LATEST observed `control.hotSpinEpoch` into their
      // slot. When two guard ctors stack, worker storage is monotonic in the
      // global epoch, so the loser's exact-equality wait would deadlock on a
      // slot already advanced past it. Wait for
      // `>= epoch` so any worker that has seen our epoch (or a later one)
      // satisfies the ack. The contract a guard cares about is "every worker
      // has acknowledged hotSpin mode at least as recently as my engagement,"
      // which `>=` captures exactly.
      //
      // The constructor is setup, not dispatch. If a worker is descheduled
      // during the transition, a pure spin can block the producer from giving
      // the scheduler a chance to run the worker that must publish the ack.
      // Retry the wake and yield after bounded spin batches.
      constexpr std::uint32_t kAckSpinBatch = 256U;
      for (std::uint32_t slot = 1; slot < participants; ++slot) {
        std::uint32_t spins = 0;
        while ((workers + slot)->hotSpinEpoch.load(std::memory_order_acquire) <
               epoch) {
          detail::cpuRelax();
          if ((++spins & (kAckSpinBatch - 1U)) == 0U) {
            wakeWorkers();
            std::this_thread::yield();
          }
        }
      }
    }

    ~LowLatencyGuard() {
      if (m_pool != nullptr) {
        m_pool->m_control.hotSpinDepth.fetch_sub(1U, std::memory_order_acq_rel);
        lowLatencyOwnerPoolTls() = m_prevOwnerPool;
      }
    }

    LowLatencyGuard(const LowLatencyGuard &) = delete;
    LowLatencyGuard &operator=(const LowLatencyGuard &) = delete;
    LowLatencyGuard(LowLatencyGuard &&) = delete;
    LowLatencyGuard &operator=(LowLatencyGuard &&) = delete;

  private:
    /// Pool the guard engaged hot-spin mode on. Null when the guard was
    /// default-constructed.
    ThreadPool *m_pool = nullptr;
    /// Owner pool the TLS named on the way in. Restored on the way out
    /// so cross-pool nesting (outer guard on pool A, inner on pool B)
    /// leaves the outer guard's owner identity intact.
    ThreadPool *m_prevOwnerPool = nullptr;
  };

  // Destroy the pool: drain pending work, signal shutdown, wake every worker,
  // then join. Detached tasks are drained first so state captured by a
  // still-running body is torn down inside the body, not under us. The
  // synchronous-worker shutdown path runs only after the detached counter
  // reaches zero.
  ~ThreadPool() {
    waitForDetachedDrain();
    shutdownAndJoin();
    autoPinRestoreIfOurs();
  }

  // Pools own raw pthreads with interior pointers; copy/move would invalidate
  // them mid-flight.
  ThreadPool(const ThreadPool &) = delete;
  ThreadPool &operator=(const ThreadPool &) = delete;
  ThreadPool(ThreadPool &&) = delete;
  ThreadPool &operator=(ThreadPool &&) = delete;

  /// Effective number of participants (producer + background workers); always
  /// at least 1.
  [[nodiscard]] std::size_t participants() const noexcept {
    return m_control.participants;
  }

  /// CPU reserved for the caller's slot-0 producer participation.
  /// Returns `UINT32_MAX` when affinity pinning is unavailable. Stable for the
  /// pool's lifetime; target used by `bindProducerSlot`.
  [[nodiscard]] std::uint32_t producerCpu() const noexcept {
    return m_workers ? m_workers->cpuId : UINT32_MAX;
  }

  /// Bind the calling thread to the pool's slot-0 CPU until the returned guard
  /// is destroyed. The dispatch hot path never changes caller affinity, but
  /// benchmarks and long-lived producer threads can opt into the same placement
  /// contract the background workers use.
  [[nodiscard]] ProducerAffinityGuard bindProducerSlot() const noexcept {
    return {producerCpu(), m_topology, m_workerCpus.data(),
            m_workerCpus.size()};
  }

  /// Read the pool's diagnostic counters as a non-atomic snapshot.
  ///
  /// Each field loads with `std::memory_order_relaxed`; the snapshot is
  /// approximate and not consistent across fields beyond per-counter
  /// monotonicity. Counters are cumulative for the pool's lifetime.
  ///
  /// Pool-level fields (`dispatches`, `inlineFallbacks`, `cancellationStops`)
  /// are zero unless `CITOR_ENABLE_POOL_COUNTERS` is defined; when off, the
  /// dispatch hot path pays no extra atomics. Worker-aggregated fields
  /// (`futexParks`, `futexWakes`, `stealAttempts`, `stealSuccesses`) are always
  /// populated.
  [[nodiscard]] detail::PoolCountersSnapshot snapshotCounters() const noexcept {
    detail::PoolCountersSnapshot s;
#ifdef CITOR_ENABLE_POOL_COUNTERS
    s.dispatches = m_counters.dispatches.load(std::memory_order_relaxed);
    s.inlineFallbacks =
        m_counters.inlineFallbacks.load(std::memory_order_relaxed);
    s.cancellationStops =
        m_counters.cancellationStops.load(std::memory_order_relaxed);
#endif
    // Worker-aggregated counters are always available because `WorkerState`
    // carries them unconditionally (they fire only on park/wake/steal events,
    // not on every dispatch).
    const std::size_t n = m_control.participants;
    for (std::size_t i = 0; i < n; ++i) {
      const auto *w = m_workers.get() + i;
      s.futexParks += w->parks.load(std::memory_order_relaxed);
      s.futexWakes += w->wakes.load(std::memory_order_relaxed);
      s.stealAttempts += w->stealAttempts.load(std::memory_order_relaxed);
      s.stealSuccesses += w->stealSuccesses.load(std::memory_order_relaxed);
    }
    return s;
  }

  /// Keep workers hot for a scoped latency-sensitive producer loop. Use around
  /// a burst of synchronous primitive calls when the caller prefers CPU burn
  /// over the futex wake round trip. The returned guard must outlive the
  /// dispatch burst.
  [[nodiscard]] LowLatencyGuard lowLatencyScope() noexcept {
    const auto &ctx = tlsContext();
    if (ctx.insidePoolWorker && ctx.pool == this) {
      return {};
    }
    return LowLatencyGuard(*this);
  }

  /// Number of CCD (or shared-L3) groups detected by the topology probe;
  /// always at least 1 on a host with one physical core.
  [[nodiscard]] std::uint32_t ccdCount() const noexcept {
    return m_topology.ccdCount;
  }

  /// Origin tag of this pool: `Standalone` (user-owned) or `Arena`
  /// (`PoolGroup`-owned).
  ///
  /// Read at the entry of every primitive's dispatch path: when the calling
  /// thread is a worker on a different `Arena` pool, the dispatch falls
  /// through to inline-on-caller execution to avoid the cross-arena deadlock
  /// that would result from a worker blocking on another arena's queue.
  [[nodiscard]] PoolKind kind() const noexcept { return m_kind; }

  /// Index of this pool inside its owning `PoolGroup`. Returns `0` for
  /// `Standalone` pools and for the first arena. Used as the participant token
  /// carried in `ThreadContext::arenaIndex` so the cross-arena guard can
  /// compare without holding a back-pointer to the `PoolGroup`.
  [[nodiscard]] std::uint32_t arenaIndex() const noexcept {
    return m_arenaIndex;
  }

  /// Slot index of the calling thread within any pool. Returns 0 when the
  /// calling thread is not currently inside a pool worker body.
  [[nodiscard]] static std::size_t workerIndex() noexcept {
    return tlsContext().slot;
  }

  /// True when the calling thread is currently inside a pool worker body.
  [[nodiscard]] static bool insidePoolWorker() noexcept {
    return tlsContext().insidePoolWorker;
  }

  /// Calling thread's arena participant token. Used by
  /// `PoolGroup::localArena()` to pick the arena the calling thread is
  /// currently a worker of. Returns `static_cast<std::uint32_t>(-1)` when the
  /// thread is not a worker on any `Arena` pool.
  [[nodiscard]] static std::uint32_t currentArenaIndexHint() noexcept {
    const auto &ctx = tlsContext();
    if (!ctx.insidePoolWorker || ctx.kind != PoolKind::Arena) {
      return static_cast<std::uint32_t>(-1);
    }
    return ctx.arenaIndex;
  }

  /// Run |fn| over `[first, last)` in parallel using the policy from |HintsT|.
  ///
  /// The body is invoked once per block as `fn(blockFirst, blockAfterLast)`.
  /// Synchronous: returns only after every block has completed (or after the
  /// first thrown exception is propagated).
  ///
  /// - `Balance::StaticUniform` runs the worker-strided block partition (no
  ///   atomics on the hot path); `Balance::DynamicChunked` races on the
  ///   relaxed `nextBlock` counter; the other two tiers fall back to dynamic.
  /// - `chunk == 0` derives a default from `(last - first) / participants`.
  /// - `n * estimatedItemNs * 1e-3 < minTaskUs * participants` runs the body
  ///   inline on the producer's thread; no worker is woken.
  /// The descriptor keeps the previous callable address between dispatches;
  /// it is overwritten before reuse and never dereferenced after the
  /// synchronous call returns. `tok` stays by value so owned tokens can move
  /// into the descriptor without a shared_ptr retain.
  /// NOLINTBEGIN(performance-unnecessary-value-param,
  /// clang-analyzer-core.StackAddressEscape)
  template <class HintsT, class F>
  [[gnu::hot, gnu::flatten]] void
  parallelFor(std::size_t first, std::size_t last, F &&fn,
              CancellationToken tok = CancellationToken{}) {
    if (last <= first) {
      return;
    }
    if constexpr (detail::kCancellationActive<HintsT>) {
      if (!tok.canStop()) {
        using NoCancellationHintsT = detail::NoCancellationHints<HintsT>;
        parallelFor<NoCancellationHintsT>(first, last, std::forward<F>(fn),
                                          CancellationToken{});
        return;
      }
    }
    if (shouldFallThroughCrossArena()) {
      const auto &ctx = tlsContext();
      // Nested same-pool path is safe for both throwing and noexcept
      // bodies. The forkJoinAll nested dispatch handles exception capture
      // through the standard ForkJoinState slot, and noexcept bodies just
      // bypass the catch frames entirely. Earlier code disabled noexcept
      // here as `!is_nothrow_invocable_v<F&, ...>` which silently
      // collapsed noexcept nested loops to single-threaded inline.
      constexpr bool kAllowNestedSamePool = true;
      if constexpr (kAllowNestedSamePool) {
        if (ctx.pool == this) {
          const std::size_t n = last - first;
          const std::size_t participants = m_control.participants;
          if (detail::shouldRunInlineHinted<HintsT>(n, participants)) {
            runInlineChunked(first, last, HintsT::chunk,
                             detail::kCancellationActive<HintsT>,
                             std::forward<F>(fn), tok);
            return;
          }

          constexpr bool kCancellationActiveForNestedToken =
              detail::kCancellationActive<HintsT>;

          std::size_t chunk = HintsT::chunk;
          if (chunk == 0) {
            chunk = ceilDiv(n, participants * 2U);
            if (chunk == 0) {
              chunk = 1;
            }
          }
          const std::size_t blockCount = ceilDiv(n, chunk);
          if (blockCount <= 1U) {
            runInline(first, last, std::forward<F>(fn), tok);
            return;
          }
          auto nestedBody = [&](std::size_t block) {
            if constexpr (kCancellationActiveForNestedToken) {
              if (tok.stop_requested()) {
                return;
              }
            }
            const std::size_t lo = first + (block * chunk);
            const std::size_t hi = std::min(lo + chunk, last);
            if (lo < hi) {
              fn(lo, hi);
            }
          };
          forkJoinAll<HintsT>(blockCount, nestedBody);
          return;
        }
      }
      runInlineChunked(first, last, HintsT::chunk,
                       detail::kCancellationActive<HintsT>, std::forward<F>(fn),
                       tok);
      return;
    }
    const std::size_t n = last - first;
    const std::size_t participants = m_control.participants;

    if (detail::shouldRunInlineHinted<HintsT>(n, participants)) {
      runInlineChunked(first, last, HintsT::chunk,
                       detail::kCancellationActive<HintsT>, std::forward<F>(fn),
                       tok);
      return;
    }

    constexpr bool kCancellationActiveForToken =
        detail::kCancellationActive<HintsT>;

    // Lazy producer-CPU pin for partition shapes that put real work on slot 0.
    // Auto-derived chunks (`chunk == 0`) signal "split the range across all
    // participants" -- the slot-0 block is the same size as a worker block, and
    // producer drift onto a worker-pinned CPU halves slot 0 via SMT/scheduler
    // contention.
    if constexpr (HintsT::chunk == 0) {
      ensureProducerPinnedForChunkZero();
    }

    // `static thread_local` lifts the descriptor out of the stack frame:
    // storage allocated once per thread, atomics keep their value across calls
    // (no per-dispatch zero-init storm), address stable so the descriptor body
    // line stays warm in producer L1 across back-to-back dispatches. Descriptor
    // write elision: keep desc cache line in MESI-Shared state across
    // back-to-back dispatches when fields are unchanged (steady-state bench
    // loop, where stack-allocated wrapper has stable address, hints are
    // constexpr, and range/chunk are loop-invariant). Each `if (desc.X != X)`
    // guard replaces an unconditional store with a read+predicted-branch; the
    // producer's L1 read keeps the line Shared instead of dirtying it to
    // Modified, so workers observe the descriptor body line without forcing a
    // Modified-to-Shared coherence transition.
    //
    // `dispatchOneStaticLockedBody` writes nullptr into firstException after
    // capturing and rethrowing any exception, so the slot is null at every call
    // entry.
    detail::JobDescriptor &desc = sharedParallelForDesc();
    auto *fnAddr =
        const_cast<void *>(static_cast<const void *>(std::addressof(fn)));

    // Acquire-load the per-worker mailboxes for any cold-stamped slot from a
    // prior dispatch; the worker's ack fetch_or happens-before the desc
    // writes below.
    drainColdStampedAcks();

    // Piggyback same-command reuse detection on the existing write-elision
    // guards: each guard already compares; if every guard takes the "no-write"
    // branch the dispatch's parameters match the previous one and we can
    // publish with kReuseBit set so workers skip desc reads.
    bool keyMatches = true;
    if (desc.first != first) {
      desc.first = first;
      keyMatches = false;
    }
    if (desc.last != last) {
      desc.last = last;
      keyMatches = false;
    }
    if (desc.participants != participants) {
      desc.participants = static_cast<std::uint32_t>(participants);
      keyMatches = false;
    }
    // desc.balance is consulted only by runActiveJob's runtime balance switch
    // (used by the untyped worker fallback). Under StaticUniform with the typed
    // worker entry, both the worker and slot-0 paths are template-dispatched
    // and never read this field; elide the compare/store.
    if constexpr (HintsT::balance != Balance::StaticUniform) {
      if (desc.balance != HintsT::balance) {
        desc.balance = HintsT::balance;
        keyMatches = false;
      }
    }
    if (desc.priority != HintsT::priority) {
      desc.priority = HintsT::priority;
      keyMatches = false;
    }
    if (!desc.preWakeCompletionProbe) {
      desc.preWakeCompletionProbe = true;
      keyMatches = false;
    }
    // desc.body is unused under the typed StaticUniform / DynamicChunked
    // entries: workers read desc->fnPtr (typed runner) and slot-0 calls *slot0
    // directly. Elide FunctionRef construction and compare/store on those
    // paths. Fallback balances still need desc.body for runActiveJob.
    if constexpr (HintsT::balance != Balance::StaticUniform &&
                  HintsT::balance != Balance::DynamicChunked) {
      const FunctionRef<void(std::size_t, std::size_t)> body{fn};
      if (desc.body != body) {
        desc.body = body;
        keyMatches = false;
      }
    }
    // Cancellation-off compile-time elision: when HintsT::cancellationChecks is
    // false neither the worker runner nor slot-0 read desc.token, so the
    // producer's compare+move is dead.
    if constexpr (kCancellationActiveForToken) {
      if (desc.token != tok) {
        desc.token = std::move(tok);
        keyMatches = false;
      }
    } else {
      (void)tok;
    }

    if constexpr (HintsT::balance == Balance::StaticUniform) {
      if (desc.fnPtr != fnAddr) {
        desc.fnPtr = fnAddr;
        keyMatches = false;
      }
      auto *expectedEntry =
          &detail::typedStaticUniformWorkerEntry<HintsT,
                                                 std::remove_reference_t<F>>;
      if (desc.workerEntry != expectedEntry) {
        desc.workerEntry = expectedEntry;
        keyMatches = false;
      }
      // Producer cold-collapse opt-in: hand the worker entry the address of
      // this pool's `WorkerState` array so the CAS-claim path can resolve
      // `claimedAt` per rank. The pointer is pool-stable across this thread's
      // reuse window; the keyMatches gate elides the store on back-to-back
      // same-pool dispatches.
      auto *workersRaw = static_cast<void *>(m_workers.get());
      if (desc.workerStateBase != workersRaw) {
        desc.workerStateBase = workersRaw;
        keyMatches = false;
      }
    } else if constexpr (HintsT::balance == Balance::DynamicChunked) {
      if (desc.fnPtr != fnAddr) {
        desc.fnPtr = fnAddr;
        keyMatches = false;
      }
      auto *expectedEntry =
          &detail::typedDynamicChunkedWorkerEntry<HintsT,
                                                  std::remove_reference_t<F>>;
      if (desc.workerEntry != expectedEntry) {
        desc.workerEntry = expectedEntry;
        keyMatches = false;
      }
      // DynamicChunked opts into cold-collapse: the producer's join-wait can
      // stamp parked ranks directly via `tryClaimRank` after slot-0 drains
      // the shared atomic counter.
      auto *workersRaw = static_cast<void *>(m_workers.get());
      if (desc.workerStateBase != workersRaw) {
        desc.workerStateBase = workersRaw;
        keyMatches = false;
      }
    } else {
      if (desc.fnPtr != nullptr) {
        desc.fnPtr = nullptr;
        keyMatches = false;
      }
      if (desc.workerEntry != nullptr) {
        desc.workerEntry = nullptr;
        keyMatches = false;
      }
      // Steal balance does not engage cold-collapse (workers re-enter via
      // their local deque rather than a single counter).
      if (desc.workerStateBase != nullptr) {
        desc.workerStateBase = nullptr;
        keyMatches = false;
      }
    }
    if (!keyMatches || desc.chunk == 0U || desc.blockCount == 0U) {
      const std::size_t hintChunk = HintsT::chunk;
      // Static doesn't steal, so 1x oversubscription gives each rank a
      // single contiguous range. Dynamic needs 2x so a fast worker can
      // absorb a straggler's tail.
      constexpr bool kOversubscribe =
          HintsT::balance == Balance::DynamicChunked;
      fillBlockShape(desc, n, participants, hintChunk, kOversubscribe);
    }

    if constexpr (HintsT::balance == Balance::DynamicChunked) {
      // Phase B atomic tail starts at `participants`; Phase A's rank-strided
      // assignment owns `[0, participants)`. Defer the store until after
      // `fillBlockShape` so we can skip it when the dispatch will collapse
      // to inline (`blockCount <= 1`) or when there is no oversubscription
      // (`blockCount <= participants`, where Phase B is never read).
      if (desc.blockCount > participants) {
        desc.nextBlock.store(participants, std::memory_order_relaxed);
      }
    }

    if (desc.blockCount <= 1U) {
      if constexpr (kCancellationActiveForToken) {
        runInline(first, last, std::forward<F>(fn), desc.token);
      } else {
        runInline(first, last, std::forward<F>(fn), CancellationToken{});
      }
      return;
    }

    // Reuse decision: keyMatches is set above by piggybacking on the
    // write-elision guards. No extra compares: the guards above already do
    // field-by-field equality. Eligible only when the worker runner is
    // monomorphized (StaticUniform + lvalue F + nothrow body + no
    // cancellation), AND fillBlockShape didn't change chunk/blockCount this
    // call.
    constexpr bool kCancellationOff = !detail::kCancellationActive<HintsT>;
    constexpr bool kReuseEligible =
        (HintsT::balance == Balance::StaticUniform ||
         HintsT::balance == Balance::DynamicChunked) &&
        std::is_lvalue_reference_v<F &&> &&
        std::is_nothrow_invocable_v<std::remove_reference_t<F> &, std::size_t,
                                    std::size_t> &&
        kCancellationOff;

    if constexpr (HintsT::balance == Balance::StaticUniform) {
      const bool reuseHint = kReuseEligible && keyMatches;
      dispatchOneStaticTypedSlot0Hinted<HintsT>(desc, fn, reuseHint);
    } else if constexpr (HintsT::balance == Balance::DynamicChunked) {
      const bool reuseHint = kReuseEligible && keyMatches;
      dispatchOneDynamicTypedSlot0Hinted<HintsT>(desc, fn, reuseHint);
    } else {
      dispatchOneStatic<HintsT::balance>(desc);
    }
  }
  // NOLINTEND(performance-unnecessary-value-param,
  // clang-analyzer-core.StackAddressEscape)

  /// Runtime-hint sibling of `parallelFor<HintsT>` for benchmark / CLI
  /// consumers. Mirrors the member-template surface but accepts a runtime
  /// |hints| value; the dispatch decision is made from a runtime field rather
  /// than a compile-time constant.
  template <class F>
  void parallelForRuntime(std::size_t first, std::size_t last, F &&fn,
                          const Hints &hints,
                          CancellationToken tok = CancellationToken{}) {
    if (last <= first) {
      return;
    }
    // Honor `hints.cancellationChecks`: when false, clear the caller's
    // token in place so no downstream worker polls it. Single predicted
    // branch; assigning a default token is allocation-free and equivalent
    // to the compile-time `kCancellationActive<HintsT>` elision used by
    // `parallelFor<HintsT>`.
    if (!hints.cancellationChecks) {
      tok = CancellationToken{};
    }
    if (shouldFallThroughCrossArena()) {
      runInlineChunked(first, last, hints.chunk, hints.cancellationChecks,
                       std::forward<F>(fn), tok);
      return;
    }
    const std::size_t n = last - first;
    const std::size_t participants = m_control.participants;

    if (detail::shouldRunInline(n, participants, hints.estimatedItemNs,
                                hints.minTaskUs)) {
      runInlineChunked(first, last, hints.chunk, hints.cancellationChecks,
                       std::forward<F>(fn), tok);
      return;
    }

    auto wrapper = [&fn](std::size_t lo, std::size_t hi) { fn(lo, hi); };
    const FunctionRef<void(std::size_t, std::size_t)> body{wrapper};

    detail::JobDescriptor desc;
    desc.first = first;
    desc.last = last;
    desc.participants = static_cast<std::uint32_t>(participants);
    desc.balance = hints.balance;
    desc.priority = hints.priority;
    desc.preWakeCompletionProbe = true;
    desc.body = body;
    desc.token = std::move(tok);

    fillBlockShape(desc, n, participants, hints.chunk);

    if (desc.blockCount <= 1U) {
      runInlineChunked(first, last, hints.chunk, hints.cancellationChecks,
                       std::forward<F>(fn), desc.token);
      return;
    }

    // Skip the atomic store when `blockCount <= participants`: Phase B's
    // atomic tail is never read in that shape (BlockClaim<Dynamic>::next
    // returns kNoBlock immediately).
    if (hints.balance == Balance::DynamicChunked &&
        desc.blockCount > participants) {
      desc.nextBlock.store(participants, std::memory_order_relaxed);
    }

    dispatchOne(desc);
  }

  /// Deterministic reduction over `[first, last)` using the policy from
  /// |HintsT|.
  ///
  /// Each block produces a partial value via |map|, partials are stored in a
  /// stack-resident vector indexed by stable chunk id, and the producer
  /// combines them with |combine| via a pairwise reduction tree in chunk-id
  /// order (NOT completion order). Under `Determinism::KahanCompensated` the
  /// per-chunk accumulation and tree combine wrap the user partial in a
  /// `KahanPair` so FP cancellation is compensated through every interior node.
  ///
  /// Determinism contract:
  /// - Chunk size is a function of `(n, HintsT)` only, NOT of participant
  ///   count. The chunk count is stable across `j`, so the tree shape is
  ///   `n`-determined.
  /// - Workers write only to their own `partials[chunkId]` slots; the
  ///   producer's join-loop establishes happens-before via the acquire-load on
  ///   `doneEpoch` so reading partials post-join is race-free.
  /// - Combining in chunk-id pairwise tree order makes parallel FP reduction
  ///   bit-reproducible (Demmel-Nguyen TOMS 2014).
  ///
  /// Empty range returns |init| unchanged. Cancellation throws
  /// `cancelled_value_exception<T>` whose `partial_value` is the deterministic
  /// combine of all chunks that completed before the stop; if zero completed,
  /// `partial_value` is |init| unchanged (we never combine against a
  /// default-constructed `T{}` since `combine` is not assumed to satisfy
  /// `combine(x, T{}) == x`).
  ///
  /// Requirements on `T`: default-constructible, copyable or movable, and
  /// trivially copyable in the Kahan path (per-chunk value is converted to
  /// `double` before the tree combine).
  template <class HintsT, class T, class Map, class Combine>
  [[nodiscard]] T parallelReduce(std::size_t first, std::size_t last, T init,
                                 Map &&map, Combine &&combine,
                                 CancellationToken tok = CancellationToken{}) {
    if (last <= first) {
      return init;
    }
    if constexpr (detail::kCancellationActive<HintsT>) {
      if (!tok.canStop()) {
        using NoCancellationHintsT = detail::NoCancellationHints<HintsT>;
        return parallelReduce<NoCancellationHintsT>(
            first, last, std::move(init), std::forward<Map>(map),
            std::forward<Combine>(combine), CancellationToken{});
      }
    }
    if (shouldFallThroughCrossArena()) {
      return runReduceInline<HintsT>(first, last, std::move(init),
                                     std::forward<Map>(map),
                                     std::forward<Combine>(combine), tok);
    }
    const std::size_t n = last - first;
    const std::size_t participants = m_control.participants;

    if (detail::shouldRunInlineHinted<HintsT>(n, participants)) {
      return runReduceInline<HintsT>(first, last, std::move(init),
                                     std::forward<Map>(map),
                                     std::forward<Combine>(combine), tok);
    }

    // Single-chunk fast path: reduce of a tiny range collapses to one map call
    // + combine, skipping the dispatch round-trip and the per-chunk slot vector
    // allocation entirely.
    {
      const std::size_t chunk = reduceChunkSize(n, HintsT::chunk);
      if (ceilDiv(n, chunk) <= 1U) {
        return runReduceInline<HintsT>(first, last, std::move(init),
                                       std::forward<Map>(map),
                                       std::forward<Combine>(combine), tok);
      }
    }

    if constexpr (HintsT::chunk == 0) {
      ensureProducerPinnedForChunkZero();
    }

    return runReduceParallel<HintsT>(
        first, last, std::move(init), std::forward<Map>(map),
        std::forward<Combine>(combine), std::move(tok), participants);
  }

  /// Runtime-hint sibling of `parallelReduce<HintsT>` for benchmark / CLI
  /// consumers. The runtime branch on `hints.determinism` selects between the
  /// compensated and uncompensated reduction shapes via one tag-switch.
  template <class T, class Map, class Combine>
  [[nodiscard]] T
  parallelReduceRuntime(std::size_t first, std::size_t last, T init, Map &&map,
                        Combine &&combine, const Hints &hints,
                        CancellationToken tok = CancellationToken{}) {
    if (last <= first) {
      return init;
    }
    if (shouldFallThroughCrossArena()) {
      return runReduceInlineRuntime(first, last, std::move(init),
                                    std::forward<Map>(map),
                                    std::forward<Combine>(combine), hints, tok);
    }
    const std::size_t n = last - first;
    const std::size_t participants = m_control.participants;

    if (detail::shouldRunInline(n, participants, hints.estimatedItemNs,
                                hints.minTaskUs)) {
      return runReduceInlineRuntime(first, last, std::move(init),
                                    std::forward<Map>(map),
                                    std::forward<Combine>(combine), hints, tok);
    }
    {
      const std::size_t chunk = reduceChunkSize(n, hints.chunk);
      if (ceilDiv(n, chunk) <= 1U) {
        return runReduceInlineRuntime(
            first, last, std::move(init), std::forward<Map>(map),
            std::forward<Combine>(combine), hints, tok);
      }
    }

    return runReduceParallelRuntime(
        first, last, std::move(init), std::forward<Map>(map),
        std::forward<Combine>(combine), hints, std::move(tok), participants);
  }

  /// Run |phaseFn| for |nPhases| phases using the persistent-worker plex
  /// protocol.
  ///
  /// The producer participates as slot 0 and drives the phase epoch;
  /// background workers spin-wait in user space between phases (no futex
  /// round-trip per phase). Each phase invokes
  /// `phaseFn(phaseIdx, slot, lo, hi)` once per participant, where
  /// `(lo, hi)` is the slot's contiguous range over `[0, n)`.
  ///
  /// Optional pre-phase hook: when supplied, `prePhaseFn(phaseIdx)` runs
  /// serially on the producer BEFORE publishing the next phase, with
  /// happens-before to every worker's per-phase body. Use it to read the
  /// previous phase's per-slot results (synchronized by the prior join) and
  /// update shared state the upcoming phase reads.
  ///
  /// Determinism: phase epochs are produced in `[0, nPhases)` order and each
  /// `phaseFn(p, slot, ...)` call is invoked exactly once per `(p, slot)` pair.
  /// The producer's slot-0 work for phase `p` runs after publishing
  /// `currentPhase = p + 1` and before joining on every other slot's
  /// `done >= p + 1`.
  ///
  /// Cancellation: a stopped |tok| flips the plex's cancellation flag at the
  /// next phase boundary. Worker bodies observe the flag between phases and
  /// return cleanly; the call still rendezvous with every worker before
  /// returning.
  ///
  /// Exception handling: the first thrown `phaseFn` exception is captured and
  /// rethrown by the producer after join. Subsequent throws drop.
  template <class HintsT, class Phase, class PrePhase>
  void runPlex(std::size_t nPhases, std::size_t n, Phase &&phaseFn,
               PrePhase &&prePhaseFn,
               CancellationToken tok = CancellationToken{}) {
    if (nPhases == 0) {
      return;
    }
    const std::size_t participants = m_control.participants;

    if (participants <= 1 || shouldFallThroughCrossArena()) {
      runPlexInline(nPhases, n, std::forward<Phase>(phaseFn),
                    std::forward<PrePhase>(prePhaseFn), tok);
      return;
    }

    runPlexParallel(nPhases, n, std::forward<Phase>(phaseFn),
                    std::forward<PrePhase>(prePhaseFn), std::move(tok),
                    participants);
  }

  /// Plex form without a pre-phase hook. Equivalent to the four-argument
  /// overload with a no-op pre-phase callable. Use when the plex needs no
  /// inter-phase serial bookkeeping.
  template <class HintsT, class Phase>
  void runPlex(std::size_t nPhases, std::size_t n, Phase &&phaseFn,
               CancellationToken tok = CancellationToken{}) {
    auto noPrePhase = [](std::size_t /*phaseIdx*/) noexcept {};
    runPlex<HintsT>(nPhases, n, std::forward<Phase>(phaseFn), noPrePhase,
                    std::move(tok));
  }

  /// Runtime-hint sibling of `runPlex<HintsT>` for benchmark / CLI consumers.
  template <class Phase, class PrePhase>
  void runPlexRuntime(std::size_t nPhases, std::size_t n, Phase &&phaseFn,
                      PrePhase &&prePhaseFn, const Hints & /*hints*/,
                      CancellationToken tok = CancellationToken{}) {
    if (nPhases == 0) {
      return;
    }
    const std::size_t participants = m_control.participants;

    if (participants <= 1 || shouldFallThroughCrossArena()) {
      runPlexInline(nPhases, n, std::forward<Phase>(phaseFn),
                    std::forward<PrePhase>(prePhaseFn), tok);
      return;
    }

    runPlexParallel(nPhases, n, std::forward<Phase>(phaseFn),
                    std::forward<PrePhase>(prePhaseFn), std::move(tok),
                    participants);
  }

  /// Runtime-hint runPlex without a pre-phase hook.
  template <class Phase>
  void runPlexRuntime(std::size_t nPhases, std::size_t n, Phase &&phaseFn,
                      const Hints &hints,
                      CancellationToken tok = CancellationToken{}) {
    auto noPrePhase = [](std::size_t /*phaseIdx*/) noexcept {};
    runPlexRuntime(nPhases, n, std::forward<Phase>(phaseFn), noPrePhase, hints,
                   std::move(tok));
  }

  /// Run |fns|... in parallel as a recursive fork-join, joining once every
  /// task retires.
  ///
  /// Each task callable is wrapped into a stack-resident `detail::Task`
  /// descriptor and pushed onto a participating worker's Chase-Lev
  /// work-stealing deque. Workers pop from their own deque first; on empty
  /// they steal from another worker's deque, biased toward same-CCD victims
  /// when `HintsT::stealPolicy == StealPolicy::ClusterLocal`. The producer
  /// participates as slot 0 and joins on the outstanding-task counter reaching
  /// zero.
  ///
  /// Recursive children: tasks may call back into `forkJoin` from inside their
  /// bodies. The nested call detects it is running on one of this pool's
  /// workers, allocates its own outstanding-task counter, and pushes children
  /// onto the calling worker's own deque. The nested join condition is its
  /// own counter reaching zero.
  ///
  /// Cancellation: workers observe |tok| at task boundaries. A stopped token
  /// causes participating workers to short-circuit unstarted bodies
  /// (decrementing the counter without running the body) so the join still
  /// rendezvous.
  ///
  /// Exception handling: the first thrown task body's exception is captured
  /// and rethrown from the producer after every outstanding task has retired.
  /// Subsequent throws drop. The cancellation flag is set as part of the
  /// first-exception capture path so the join finishes promptly.
  template <class HintsT, class... TaskFns>
  // Pass-by-value disambiguates against the no-token overload when the call
  // site forwards a `CancellationToken` as the first argument.
  // NOLINTNEXTLINE(performance-unnecessary-value-param)
  void forkJoin(CancellationToken tok, TaskFns &&...fns) {
    forkJoinImpl<HintsT, /*HasToken=*/true>(tok, std::forward<TaskFns>(fns)...);
  }

  /// Internal implementation shared by `forkJoin` and the no-token
  /// overload. `HasToken` is a compile-time tag that elides token reads
  /// when the caller did not pass a token.
  template <class HintsT, bool HasToken, class... TaskFns>
  void forkJoinImpl(const CancellationToken &tok, TaskFns &&...fns) {
    constexpr std::size_t kNTasks = sizeof...(TaskFns);
    if constexpr (kNTasks == 0) {
      (void)tok;
      return;
    } else if constexpr (kNTasks == 1) {
      // Single-task fork-join is a degenerate case: there is no fan-out, no
      // deque traffic, no generation publish. Run it directly on the caller and
      // observe the token at entry, but only when the caller passed a real
      // token; the no-token overload bypasses the check entirely via
      // compile-time gate.
      if constexpr (HasToken) {
        if (tok.stop_requested()) {
          return;
        }
      } else {
        (void)tok;
      }
      std::tuple<std::decay_t<TaskFns>...> single{
          std::forward<TaskFns>(fns)...};
      std::get<0>(single)();
      return;
    } else {
      // Materialize each task callable in a tuple so the producer's stack owns
      // the storage that every `FunctionRef` points into. The tuple outlives
      // the descriptors by construction -- the synchronous call joins before
      // this scope exits.
      std::tuple<std::decay_t<TaskFns>...> closures{
          std::forward<TaskFns>(fns)...};

      // Detect a recursive call from inside this pool's worker drain loop and
      // take the in-place nested-recursion path. Outside the pool (or from a
      // different pool's worker), route through the dispatch path that wakes
      // every background worker.
      auto &ctx = tlsContext();
      if (ctx.pool == this && ctx.insidePoolWorker) {
        // Typed-tail fast path: build N-1 Task descriptors for the deferred
        // children and invoke the last child directly via its statically-typed
        // callable. Skips the FunctionRef thunk and the per-task runOneTask
        // wrapper that would otherwise mediate the inline call. The deque still
        // holds N-1 descriptors so peers steal them through the standard victim
        // probe.
        std::array<detail::Task, kNTasks - 1> deferred{};
        fillTaskBodies(closures, deferred,
                       std::make_index_sequence<kNTasks - 1>{});
        runForkJoinTypedTailNested<HasToken>(
            deferred.data(), kNTasks - 1, std::get<kNTasks - 1>(closures), tok,
            stealPolicyFromHints<HintsT>(),
            static_cast<std::uint32_t>(ctx.slot));
        return;
      }
      std::array<detail::Task, kNTasks> tasks{};
      fillTaskBodies(closures, tasks, std::index_sequence_for<TaskFns...>{});
      ensureProducerPinnedForChunkZero();
      runForkJoinOuter<HasToken>(tasks.data(), kNTasks, tok,
                                 stealPolicyFromHints<HintsT>());
    }
  }

  /// No-token convenience overload for `forkJoin`. Forwards to the four-arg
  /// overload with a default-constructed `CancellationToken` so call sites
  /// that do not need cancellation pay no syntactic cost.
  template <class HintsT, class... TaskFns>
  void forkJoin(TaskFns &&...fns) {
    // No token passed: route through the compile-time `HasToken=false` impl so
    // the cancel-gate's `state.token.stop_requested()` poll, the per-call
    // `state.token = ...` assignment, and the `kNTasks == 1` early-stop check
    // are all elided at compile time. Saves one `shared_ptr` deref per spawn on
    // the canonical (no-cancel) recursive path.
    forkJoinImpl<HintsT, /*HasToken=*/false>(CancellationToken{},
                                             std::forward<TaskFns>(fns)...);
  }

  /// Runtime-N fork-join: spawn |n| tasks indexed `[0, n)` and join.
  /// Generalizes the variadic `forkJoin<HintsT>(fns...)` to a count known only
  /// at runtime.
  template <class HintsT, class BodyFn>
  void forkJoinAll(std::size_t n, BodyFn body) {
    if (n == 0U) {
      return;
    }
    if (n == 1U) {
      // Single-task fork-join is a degenerate case: no fan-out, no deque
      // traffic, no state.
      body(0);
      return;
    }
    constexpr std::size_t kStackTaskBudget = 32U;
    using Body = std::decay_t<BodyFn>;
    Body &bodyRef = body;
    struct IndexedClosure {
      Body *bodyPtr;
      std::size_t index;
      void operator()() const { (*bodyPtr)(index); }
    };
    // Uninitialized aligned storage. Default-init `std::array<...>{}` would
    // zero 32 elements every call; we only ever touch slots [0, n). Skipping
    // the 32-element fill removes the memset_avx512 hot spot from the runtime-N
    // hot path.
    alignas(IndexedClosure)
        std::array<std::byte, sizeof(IndexedClosure) * kStackTaskBudget>
            stackClosureBuf;
    alignas(detail::Task)
        std::array<std::byte, sizeof(detail::Task) * kStackTaskBudget>
            stackTaskBuf;
    std::vector<IndexedClosure> heapClosures;
    std::vector<detail::Task> heapTasks;
    auto *closures = reinterpret_cast<IndexedClosure *>(stackClosureBuf.data());
    auto *tasks = reinterpret_cast<detail::Task *>(stackTaskBuf.data());
    if (n > kStackTaskBudget) {
      heapClosures.resize(n);
      heapTasks.resize(n);
      closures = heapClosures.data();
      tasks = heapTasks.data();
    }

    auto &ctx = tlsContext();
    if (ctx.pool == this && ctx.insidePoolWorker) {
      // Typed-tail nested fast path: push n-1 tasks for peers to steal, invoke
      // `body(n-1)` directly on this thread via its statically-typed callable.
      // Skips the FunctionRef thunk and one runOneTask wrapper at every
      // recursion frame. Mirrors the variadic forkJoin's typed-tail path for
      // the runtime-N spawn shape.
      const std::size_t deferred = n - 1U;
      for (std::size_t i = 0; i < deferred; ++i) {
        if (n > kStackTaskBudget) {
          closures[i] = IndexedClosure{.bodyPtr = &bodyRef, .index = i};
          tasks[i].body = FunctionRef<void()>(closures[i]);
          tasks[i].state = nullptr;
        } else {
          ::new (closures + i) IndexedClosure{.bodyPtr = &bodyRef, .index = i};
          ::new (tasks + i) detail::Task{};
          tasks[i].body = FunctionRef<void()>(closures[i]);
        }
      }
      runForkJoinTypedTailNested</*HasToken=*/false>(
          tasks, deferred, [&]() { bodyRef(n - 1U); }, CancellationToken{},
          stealPolicyFromHints<HintsT>(), static_cast<std::uint32_t>(ctx.slot));
      if (n <= kStackTaskBudget) {
        // Trivially-destructible types: no destructor calls needed.
        static_assert(std::is_trivially_destructible_v<IndexedClosure>);
        static_assert(std::is_trivially_destructible_v<detail::Task>);
      }
      return;
    }

    // Outer (top-level) path: full materialization, no inline tail.
    for (std::size_t i = 0; i < n; ++i) {
      if (n > kStackTaskBudget) {
        closures[i] = IndexedClosure{.bodyPtr = &bodyRef, .index = i};
        tasks[i].body = FunctionRef<void()>(closures[i]);
        tasks[i].state = nullptr;
      } else {
        ::new (closures + i) IndexedClosure{.bodyPtr = &bodyRef, .index = i};
        ::new (tasks + i) detail::Task{};
        tasks[i].body = FunctionRef<void()>(closures[i]);
      }
    }
    runForkJoinOuter</*HasToken=*/false>(tasks, n, CancellationToken{},
                                         stealPolicyFromHints<HintsT>());
  }

  // Friend overload routing the `citor::forkJoin` CPO to this pool. Both
  // surfaces (member call and CPO call) share the same engine and monomorphize
  // identically.
  template <class HintsT, class... TaskFns>
  friend void tag_invoke(ForkJoinTag /*cpo*/, ThreadPool &self,
                         CancellationToken tok, HintsT /*hints*/,
                         TaskFns &&...fns) {
    self.template forkJoin<HintsT>(std::move(tok),
                                   std::forward<TaskFns>(fns)...);
  }

  // Adapter so the CPO's `ForkJoinFn::operator()` can call our friend
  // overload. `ForkJoinFn::operator()` invokes `tag_invoke(*this, pool, tok,
  // HintsT{}, fns...)` passing the CPO's own type, not `ForkJoinTag`. The
  // adapter forwards from the CPO type to the canonical `ForkJoinTag`
  // overload above.
  template <class HintsT, class... TaskFns>
  friend void tag_invoke(const detail::ForkJoinFn & /*cpo*/, ThreadPool &self,
                         CancellationToken tok, HintsT /*hints*/,
                         TaskFns &&...fns) {
    self.template forkJoin<HintsT>(std::move(tok),
                                   std::forward<TaskFns>(fns)...);
  }

  /// Run |fn| over `[0, q)` query indices in parallel using the policy from
  /// |HintsT|.
  ///
  /// The body is invoked once per chunk as `fn(qFirst, qLast)`; the body must
  /// process every query index in `[qFirst, qLast)` in any order, writing into
  /// a per-query output slot keyed on the query index. Synchronous.
  ///
  /// Reuses the `parallelFor` engine; this is the named entry point for "many
  /// independent queries" workloads. Sites with skewed per-query cost should
  /// override `HintsT::balance` to `Balance::DynamicChunked` to amortize
  /// traversal-depth skew across workers.
  ///
  /// Output stability: per-query results are bit-identical regardless of
  /// dispatch order so long as the body keys its writes on the query index.
  template <class HintsT, class QueryFn>
  void bulkForQueries(std::size_t q, QueryFn &&fn,
                      CancellationToken tok = CancellationToken{}) {
    parallelFor<HintsT>(std::size_t{0}, q, std::forward<QueryFn>(fn),
                        std::move(tok));
  }

  /// Runtime-hint sibling of `bulkForQueries<HintsT>` for benchmark / CLI
  /// consumers. Forwards through `parallelForRuntime`.
  template <class QueryFn>
  void bulkForQueriesRuntime(std::size_t q, QueryFn &&fn, const Hints &hints,
                             CancellationToken tok = CancellationToken{}) {
    parallelForRuntime(std::size_t{0}, q, std::forward<QueryFn>(fn), hints,
                       std::move(tok));
  }

  // Friend overload routing the `citor::bulkForQueries` CPO to this pool. The
  // CPO surface monomorphizes identically to the member call.
  template <class HintsT, class QueryFn>
  friend void tag_invoke(BulkForQueriesTag /*cpo*/, ThreadPool &self,
                         std::size_t q, QueryFn &&fn, HintsT /*hints*/,
                         CancellationToken tok) {
    self.template bulkForQueries<HintsT>(q, std::forward<QueryFn>(fn),
                                         std::move(tok));
  }

  // Adapter so the CPO's `BulkForQueriesFn::operator()` can call our friend
  // overload. Forwards from the CPO type to the canonical `BulkForQueriesTag`
  // overload above.
  template <class HintsT, class QueryFn>
  friend void tag_invoke(const detail::BulkForQueriesFn & /*cpo*/,
                         ThreadPool &self, std::size_t q, QueryFn &&fn,
                         HintsT /*hints*/, CancellationToken tok) {
    self.template bulkForQueries<HintsT>(q, std::forward<QueryFn>(fn),
                                         std::move(tok));
  }

  // Friend overload routing the `citor::parallelFor` CPO to this pool. The CPO
  // surface monomorphizes identically to the member call.
  template <class HintsT, class F>
  friend void tag_invoke(ParallelForTag /*cpo*/, ThreadPool &self,
                         std::size_t first, std::size_t last, F &&fn,
                         HintsT /*hints*/, CancellationToken tok) {
    self.template parallelFor<HintsT>(first, last, std::forward<F>(fn),
                                      std::move(tok));
  }

  // Friend overload routing the `citor::parallelReduce` CPO to this pool.
  // Both surfaces monomorphize identically with `HintsT` baked in at compile
  // time.
  template <class HintsT, class T, class Map, class Combine>
  friend T tag_invoke(ParallelReduceTag /*cpo*/, ThreadPool &self,
                      std::size_t first, std::size_t last, T init, Map &&map,
                      Combine &&combine, HintsT /*hints*/,
                      CancellationToken tok) {
    return self.template parallelReduce<HintsT>(
        first, last, std::move(init), std::forward<Map>(map),
        std::forward<Combine>(combine), std::move(tok));
  }

  // Adapter so the `parallelReduce` CPO function-object can call our friend
  // overload. Forwards from the CPO type to the canonical `ParallelReduceTag`
  // overload above.
  template <class HintsT, class T, class Map, class Combine>
  friend T tag_invoke(const detail::ParallelReduceFn & /*cpo*/,
                      ThreadPool &self, std::size_t first, std::size_t last,
                      T init, Map &&map, Combine &&combine, HintsT /*hints*/,
                      CancellationToken tok) {
    return self.template parallelReduce<HintsT>(
        first, last, std::move(init), std::forward<Map>(map),
        std::forward<Combine>(combine), std::move(tok));
  }

  // Adapter so the CPO's `ParallelForFn::operator()` can call our friend
  // overload. Forwards from the CPO type to the canonical `ParallelForTag`
  // overload above.
  template <class HintsT, class F>
  friend void tag_invoke(const detail::ParallelForFn & /*cpo*/,
                         ThreadPool &self, std::size_t first, std::size_t last,
                         F &&fn, HintsT /*hints*/, CancellationToken tok) {
    self.template parallelFor<HintsT>(first, last, std::forward<F>(fn),
                                      std::move(tok));
  }

  // Friend overload routing the `citor::runPlex` CPO to this pool. The CPO
  // surface monomorphizes identically to the member call.
  template <class HintsT, class Phase>
  friend void tag_invoke(RunPlexTag /*cpo*/, ThreadPool &self,
                         std::size_t nPhases, std::size_t n, Phase &&phaseFn,
                         HintsT /*hints*/, CancellationToken tok) {
    self.template runPlex<HintsT>(nPhases, n, std::forward<Phase>(phaseFn),
                                  std::move(tok));
  }

  // Adapter so the CPO's `RunPlexFn::operator()` can call our friend overload.
  // Forwards from the CPO type to the canonical `RunPlexTag` overload above.
  template <class HintsT, class Phase>
  friend void tag_invoke(const detail::RunPlexFn & /*cpo*/, ThreadPool &self,
                         std::size_t nPhases, std::size_t n, Phase &&phaseFn,
                         HintsT /*hints*/, CancellationToken tok) {
    self.template runPlex<HintsT>(nPhases, n, std::forward<Phase>(phaseFn),
                                  std::move(tok));
  }

  /// Run an ordered sequence of stages with the declared barrier between
  /// consecutive stages.
  ///
  /// Each stage is a `Stage` value carrying a callable plus a compile-time
  /// `BarrierKind`. The producer participates as slot 0 across every stage;
  /// background workers run their slice of each stage in parallel. The
  /// post-stage barrier controls how the next stage begins:
  ///
  /// - `None`: each worker proceeds without waiting on others.
  /// - `Global` / `DeterministicReduce`: every worker waits until every slot
  ///   has finished the prior stage before any worker begins the next.
  /// - `ProducerSerial`: only slot 0 runs the next stage's body; other workers
  ///   spin on slot 0's completion before proceeding.
  ///
  /// Determinism: stages run in submission order. In the default same-chunk
  /// mode, each `(stage, slot)` pair is invoked exactly once per call (or
  /// zero times for non-producer slots after a `ProducerSerial` barrier).
  /// Per-slot ranges are stable functions of `(n, slot, participants)`.
  /// Dynamic-chain hints can opt all-global stage packs into per-stage chunk
  /// claiming.
  ///
  /// Cancellation: the implicit cancellation token bound through the CPO
  /// surface is observed at stage boundaries. A stopped token causes every
  /// slot to skip the remaining stages cleanly so the producer's join still
  /// rendezvous.
  ///
  /// Exception handling: the first thrown stage body's exception is captured,
  /// the chain's cancellation flag is set, and the producer rethrows once
  /// every slot has retired. Slots that observe the cancellation flag stamp
  /// their `done` epoch to the final stage so other slots' barrier waits
  /// complete.
  ///
  /// Empty stage pack is a no-op; empty range (`n == 0`) still runs each
  /// stage with `slotLo == slotHi`.
  template <class ChainHintsT, class... Stages>
  void parallelChain(std::size_t n, Stages &&...stages) {
    parallelChainWithToken<ChainHintsT>(n, CancellationToken{},
                                        std::forward<Stages>(stages)...);
  }

  /// `parallelChain` overload that accepts an explicit cancellation token.
  /// Named differently from the no-token form because variadic packs absorb
  /// the leading token argument when the two overloads share a name.
  template <class ChainHintsT, class... Stages>
  void parallelChainWithToken(std::size_t n, const CancellationToken &tok,
                              Stages &&...stages) {
    parallelChainImpl<ChainHintsT>(n, tok, std::forward<Stages>(stages)...);
  }

  /// Runtime-hint sibling of `parallelChain<ChainHintsT>` for benchmark / CLI
  /// consumers. The runtime branch on stage barriers is compile-time because
  /// each stage's `BarrierKind` is encoded in its type. |hints.priority| is
  /// honored on the dispatch lease; other hint fields (balance, chunk,
  /// pipelineSameChunk, cancellationChecks) currently fall back to the
  /// engine defaults; the per-stage runner uses `ChainHintsT` as a type
  /// marker for `if constexpr` selection, so threading them runtime would
  /// require structural changes to the per-stage dispatcher.
  template <class... Stages>
  void parallelChainRuntime(std::size_t n, const ChainHints &hints,
                            const CancellationToken &tok, Stages &&...stages) {
    parallelChainImplWithRuntimeHints<ChainHints>(
        n, &hints, tok, std::forward<Stages>(stages)...);
  }

  // Friend overload routing the `citor::parallelChain` CPO to this pool. The
  // CPO surface monomorphizes identically to the member call.
  template <class ChainHintsT, class... Stages>
  friend void tag_invoke(ParallelChainTag /*cpo*/, ThreadPool &self,
                         std::size_t n, [[maybe_unused]] ChainHintsT hints,
                         const CancellationToken &tok, Stages &&...stages) {
    self.template parallelChainWithToken<ChainHintsT>(
        n, tok, std::forward<Stages>(stages)...);
  }

  // Adapter so the CPO's `ParallelChainFn::operator()` can call our friend
  // overload. Forwards from the CPO type to the canonical `ParallelChainTag`
  // overload above.
  template <class ChainHintsT, class... Stages>
  friend void tag_invoke(const detail::ParallelChainFn & /*cpo*/,
                         ThreadPool &self, std::size_t n,
                         [[maybe_unused]] ChainHintsT hints,
                         const CancellationToken &tok, Stages &&...stages) {
    self.template parallelChainWithToken<ChainHintsT>(
        n, tok, std::forward<Stages>(stages)...);
  }

  /// Blelloch two-pass parallel prefix scan over `[0, n)` using the policy
  /// from |HintsT|.
  ///
  /// Two-pass dispatch shape:
  ///   1. Pass 1: each slot computes its chunk's partial sum by invoking
  ///      `body(slot, slotLo, slotHi, identity, nullptr)`. The body returns
  ///      the chunk's partial; the worker stamps `partials[slot]` and stamps
  ///      its done epoch to `1` (release).
  ///   2. Sequential reduce: the producer waits until every chunk's
  ///      `done >= 1`, then computes
  ///      `prefix[c] = combine(prefix[c-1], partials[c-1])` in chunk-id order
  ///      with `prefix[0] = identity`. It overwrites `partials[c]` with
  ///      `prefix[c]` and publishes `prefixesPublished = 1` (release).
  ///   3. Pass 2: each slot acquire-loads `prefixesPublished`, then invokes
  ///      `body(slot, slotLo, slotHi, partials[slot], nullptr)`; the body
  ///      writes the per-element scan into a buffer it captured itself and
  ///      returns the same partial sum (ignored). Each slot stamps `done = 2`.
  ///   4. The producer joins on every slot's `done >= 2` and returns the
  ///      inclusive accumulator at the right edge:
  ///      `combine(prefix[participants - 1], partial[participants - 1])`.
  ///
  /// Body contract for distinguishing passes: `out` is `nullptr` in BOTH
  /// passes; the body must distinguish them itself. Recommended pattern:
  ///
  ///   std::atomic<int> callIdx{0};
  ///   auto body = [&](...) {
  ///     const int idx = callIdx.fetch_add(1, std::memory_order_acq_rel);
  ///     if (idx < pool.participants()) { /* Pass 1 */ }
  ///     else                            { /* Pass 2 */ }
  ///   };
  ///
  /// The `std::atomic<int>` captured by reference is the only race-free way
  /// to thread a pass index through the body, since multiple workers run
  /// each pass concurrently. A plain `int` counter captured by reference is
  /// a data race; use the atomic shape above. See `parallel_scan_test.cpp`
  /// for a working example.
  ///
  /// Alternative, brittle: branch on `initial != identity`. Slot 0's Pass 2
  /// receives `initial = identity` (its exclusive prefix IS identity), so
  /// this form loses the distinction for slot 0; prefer the atomic counter.
  ///
  /// Determinism: chunk identity is a stable function of
  /// `(n, slot, participants)`; the producer's sequential reduce visits
  /// chunks in `[0, participants)` order so output is bit-stable across worker
  /// counts only when |prefix| is associative AND the body's per-element fold
  /// is left-to-right within a chunk.
  ///
  /// Empty range returns |identity| without dispatching. Single participant
  /// walks inline: the body is invoked once with `initial = identity` and the
  /// call returns the body's return value combined with |identity| through
  /// |prefix|.
  ///
  /// Cancellation: stop-requests at pass boundaries cause every slot to stamp
  /// `done = 2` and exit cleanly; the producer's join still rendezvous. The
  /// return value reflects whatever completed chunks computed.
  ///
  /// Exception handling: the first thrown body exception is captured and
  /// rethrown by the producer after every slot has retired. Subsequent throws
  /// drop.
  template <class HintsT, class T, class BodyFn, class PrefixFn>
  [[nodiscard]] T parallelScan(std::size_t n, T identity, BodyFn &&body,
                               PrefixFn &&prefix,
                               CancellationToken tok = CancellationToken{}) {
    if (n == 0) {
      return identity;
    }
    const std::size_t participants = m_control.participants;

    // Inline threshold: a two-pass scan needs two body invocations per slot, a
    // sequential reduce, and two joins. For tiny `n`, the dispatch round-trip
    // dominates the actual scan work, so collapse to the inline branch.
    // Threshold matches the chunk default used by `parallelFor`'s inline gate
    // and keeps the contract observable (single body call with `initial =
    // identity`).
    static constexpr std::size_t kScanInlineThreshold = 32U;
    if (participants <= 1 || n <= kScanInlineThreshold ||
        shouldFallThroughCrossArena()) {
      // Inline fallback: one chunk, body invoked once with `initial =
      // identity`. The body's return value is the chunk's partial sum; the
      // inclusive accumulator at the right edge is `combine(identity, partial)`
      // which equals `combine(prefix[0], partial[0])` in the multi-participant
      // shape. The cross-arena guard takes the same branch: a worker on a
      // different arena must not block on this arena's queue. A pre-cancelled
      // token short- circuits before the body runs, returning the identity
      // unchanged.
      if (tok.stop_requested()) {
        return identity;
      }
      T partial =
          std::forward<BodyFn>(body)(std::size_t{0}, std::size_t{0}, n,
                                     identity, static_cast<T *>(nullptr));
      return prefix(std::move(identity), std::move(partial));
    }

    if constexpr (HintsT::chunk == 0) {
      ensureProducerPinnedForChunkZero();
    }

    return runScanParallel<HintsT>(
        n, std::move(identity), std::forward<BodyFn>(body),
        std::forward<PrefixFn>(prefix), std::move(tok), participants);
  }

  // Friend overload routing the `citor::parallelScan` CPO to this pool. The
  // CPO surface monomorphizes identically to the member call.
  template <class HintsT, class T, class BodyFn, class PrefixFn>
  friend T tag_invoke(ParallelScanTag /*cpo*/, ThreadPool &self, std::size_t n,
                      T identity, BodyFn &&body, PrefixFn &&prefix,
                      HintsT /*hints*/, CancellationToken tok) {
    return self.template parallelScan<HintsT>(
        n, std::move(identity), std::forward<BodyFn>(body),
        std::forward<PrefixFn>(prefix), std::move(tok));
  }

  // Adapter so the CPO's `ParallelScanFn::operator()` can call our friend
  // overload. Forwards from the CPO type to the canonical `ParallelScanTag`
  // overload above.
  template <class HintsT, class T, class BodyFn, class PrefixFn>
  friend T tag_invoke(const detail::ParallelScanFn & /*cpo*/, ThreadPool &self,
                      std::size_t n, T identity, BodyFn &&body,
                      PrefixFn &&prefix, HintsT /*hints*/,
                      CancellationToken tok) {
    return self.template parallelScan<HintsT>(
        n, std::move(identity), std::forward<BodyFn>(body),
        std::forward<PrefixFn>(prefix), std::move(tok));
  }

  /// Buffer-to-buffer inclusive prefix scan. Engine owns the inner loop
  /// (no user body), so it can use the most aggressive memory-traffic
  /// shape -- decoupled-lookback single-pass with `PREFETCHW` ahead of
  /// the writes, per-cluster lookback chains on multi-CCD parts -- to
  /// hit the hardware bandwidth floor.
  ///
  /// `in` and `out` are caller-owned spans of equal length; aliasing
  /// (in.data() == out.data()) is safe because the engine reads `in[i]`
  /// before writing `out[i]` for every `i`. `identity` is the monoid
  /// identity (e.g. 0 for plus). `prefix` is the associative combiner
  /// (must be associative; need not be commutative).
  ///
  /// Returns the inclusive total at the right edge:
  /// `prefix(prefix(... prefix(identity, in[0]) ...), in[n-1])`.
  template <class HintsT, class T, class PrefixFn>
  [[nodiscard]] T
  inclusiveScan(std::span<const T> in, std::span<T> out, T identity,
                PrefixFn &&prefix,
                const CancellationToken &tok = CancellationToken{}) {
    return runInclusiveScanLookback<HintsT, T>(
        in, out, std::move(identity), std::forward<PrefixFn>(prefix), tok);
  }

  // Friend overload routing the `citor::inclusiveScan` CPO to this pool.
  template <class HintsT, class T, class PrefixFn>
  friend T tag_invoke(InclusiveScanTag /*cpo*/, ThreadPool &self,
                      std::span<const T> in, std::span<T> out, T identity,
                      PrefixFn &&prefix, HintsT /*hints*/,
                      CancellationToken tok) {
    return self.template inclusiveScan<HintsT>(in, out, std::move(identity),
                                               std::forward<PrefixFn>(prefix),
                                               std::move(tok));
  }

  template <class HintsT, class T, class PrefixFn>
  friend T tag_invoke(const detail::InclusiveScanFn & /*cpo*/, ThreadPool &self,
                      std::span<const T> in, std::span<T> out, T identity,
                      PrefixFn &&prefix, HintsT /*hints*/,
                      const CancellationToken &tok) {
    return self.template inclusiveScan<HintsT>(
        in, out, std::move(identity), std::forward<PrefixFn>(prefix), tok);
  }

  /// Submit |fn| for fire-and-forget execution; returns without joining.
  ///
  /// The pool tracks every in-flight detached task via a counter. The
  /// destructor blocks until the counter drops to zero, so a body that
  /// outruns its caller is guaranteed to finish before the pool is destroyed.
  /// The body runs on a dedicated `std::thread` spawned per call; the pool's
  /// persistent worker fleet is reserved for the synchronous primitives.
  ///
  /// Cancellation: a pre-cancelled |tok| short-circuits the body before any
  /// user code runs. The body observes the token cooperatively; the pool does
  /// not preempt.
  ///
  /// Exception handling: an exception escaping |fn| is captured into a
  /// per-pool slot via `std::current_exception` and surfaced by
  /// `lastDetachedException`. The slot latches on the first throw; subsequent
  /// throws drop. A detached throw never propagates into the destructor.
  template <class HintsT, class TaskFn>
  void submitDetached(TaskFn fn, CancellationToken tok = CancellationToken{}) {
    // Token already stopped at submit time: the trampoline would observe
    // `stop_requested()` before invoking the body and decrement the in-flight
    // counter without doing useful work. Skip the heap allocation, the
    // std::thread spawn, and the counter round-trip; the user's contract is
    // "body runs unless cancellation observed", which is satisfied trivially
    // here.
    if (tok.stop_requested()) [[unlikely]] {
      return;
    }
    // Heap-allocate the closure owner on the cold submit path under a
    // try/catch; the body and token are owned by the spawned thread and
    // released after the body returns. A per-pool slab would shave the
    // allocation but is unnecessary while detached submission stays cold.
    auto owner = std::make_unique<DetachedTask>();
    owner->body = std::function<void()>(std::move(fn));
    owner->token = std::move(tok);

    m_detachedInFlight.fetch_add(1, std::memory_order_acq_rel);
    try {
      // The thread takes ownership of the heap closure via the unique_ptr
      // release below. The `runDetached` trampoline observes cancellation, runs
      // the body, captures any thrown exception, and decrements the in-flight
      // counter on the way out.
      DetachedTask *raw = owner.release();
      try {
        std::thread t(&ThreadPool::runDetached, this, raw);
        // Detach on the std::thread side so the OS thread cleans up on its own;
        // the join-on-destroy contract is satisfied by the in-flight counter,
        // not by holding the std::thread.
        t.detach();
      } catch (...) {
        // Spawn failed after release: reclaim ownership so the closure is
        // destroyed and rethrow.
        owner.reset(raw);
        throw;
      }
    } catch (...) {
      // Failed to spawn: roll back the counter so the destructor does not wait
      // forever, and re-throw so the caller sees the system error.
      m_detachedInFlight.fetch_sub(1, std::memory_order_acq_rel);
      {
        const std::scoped_lock<std::mutex> lock(m_detachedMutex);
        m_detachedDone.notify_all();
      }
      throw;
    }
  }

  /// Snapshot of the most recently captured exception thrown by a detached
  /// task. The slot latches on the first detached throw; subsequent throws do
  /// not overwrite. Returns `nullptr` if no detached task has thrown.
  [[nodiscard]] std::exception_ptr lastDetachedException() const noexcept {
    const std::scoped_lock<std::mutex> lock(m_detachedMutex);
    return m_detachedException;
  }

  // Friend overload routing the `citor::submitDetached` CPO to this pool.
  // Both surfaces share the same engine and monomorphize identically.
  template <class HintsT, class TaskFn>
  friend void tag_invoke(SubmitDetachedTag /*cpo*/, ThreadPool &self,
                         HintsT /*hints*/, TaskFn &&fn, CancellationToken tok) {
    self.template submitDetached<HintsT>(std::forward<TaskFn>(fn),
                                         std::move(tok));
  }

  // Adapter so the CPO's `SubmitDetachedFn::operator()` can call our friend
  // overload. Forwards from the CPO type to the canonical `SubmitDetachedTag`
  // overload above.
  template <class HintsT, class TaskFn>
  friend void tag_invoke(const detail::SubmitDetachedFn & /*cpo*/,
                         ThreadPool &self, HintsT /*hints*/, TaskFn &&fn,
                         CancellationToken tok) {
    self.template submitDetached<HintsT>(std::forward<TaskFn>(fn),
                                         std::move(tok));
  }

private:
  /// Per-thread context tracking pool participation; consulted by
  /// `workerIndex` and friends.
  struct ThreadContext {
    /// Slot index of the calling thread (0 outside any pool).
    std::size_t slot = 0;

    /// Whether the calling thread is currently inside a pool worker body.
    bool insidePoolWorker = false;

    /// Pool the calling thread is currently a worker of, or `nullptr` outside
    /// any pool. Used by `forkJoin` to detect a recursive call from inside the
    /// pool's own worker drain loop and take the in-place nested-recursion
    /// path instead of dispatching a fresh generation.
    ThreadPool *pool = nullptr;

    /// Origin tag of the pool this thread is participating in. Mirrors
    /// `ThreadPool::kind()`; set only while `insidePoolWorker` is true.
    PoolKind kind = PoolKind::Standalone;

    /// Arena index of the owning `PoolGroup` arena; meaningful only when
    /// `kind == PoolKind::Arena`. The cross-arena guard reads this token to
    /// fall through to inline-on-caller execution when a worker tries to call
    /// into a different arena's queue.
    std::uint32_t arenaIndex = 0;
  };

  /// Access (and lazily zero-initialize) the calling thread's pool context.
  static ThreadContext &tlsContext() noexcept {
    thread_local ThreadContext ctx;
    return ctx;
  }

  /// Detect a synchronous dispatch that would deadlock and force the
  /// inline-on-caller path. Returns `true` in three scenarios:
  ///
  /// 1. Cross-arena call. The caller is a worker on a different `Arena` pool.
  ///    Submitting here would block the caller's own arena worker on a queue
  ///    it does not service.
  /// 2. Same-arena reentrancy. The caller is already a worker on this `Arena`
  ///    pool. A primitive without a nested same-pool path would block on its
  ///    own join condition.
  /// 3. Same-pool reentrancy on a `Standalone` pool. The producer or a worker
  ///    already inside a synchronous primitive of THIS pool would re-enter
  ///    `dispatchOneStatic` and try to re-acquire the non-recursive
  ///    `m_dispatchMutex`, deadlocking against the outer dispatch.
  ///
  /// Primitives with a same-pool nested path (`forkJoin`, compile-time
  /// `parallelFor`) detect that case at their entry point and bypass this
  /// guard.
  [[nodiscard]] bool
  shouldFallThroughCrossArena(const ThreadContext &ctx) const noexcept {
    if (!ctx.insidePoolWorker) {
      return false;
    }
    // Same-pool reentrancy: a primitive that reaches this shared guard has no
    // nested same-pool path, so dispatching would block on m_dispatchMutex
    // while the outer dispatch holds it. Inline on the caller instead.
    if (ctx.pool == this) {
      return true;
    }
    // Cross-pool: a worker on any other pool (Arena or Standalone) calling
    // synchronously here would block its own pool's worker on this pool's
    // queue. Inline on the caller. Without this fall-through, two
    // standalone pools whose workers synchronously call each other deadlock
    // (worker A waits for B, worker B waits for A).
    return true;
  }

  /// Convenience overload that reads the calling thread's `ThreadContext`
  /// from TLS.
  [[nodiscard]] bool shouldFallThroughCrossArena() const noexcept {
    return shouldFallThroughCrossArena(tlsContext());
  }

  /// Argument bundle handed to `workerEntry`; lives in `m_workerSpawnArgs` for
  /// the pool's lifetime.
  struct WorkerSpawnArg {
    /// Pool owning this worker; non-owning pointer.
    ThreadPool *pool = nullptr;
    /// Slot index assigned to this worker (`>= 1`; producer is slot 0).
    std::size_t workerIndex = 0;
  };

  /// Heap-allocated owner for a single fire-and-forget detached task. The pool
  /// spawns one `std::thread` per task that takes ownership of the closure
  /// and the cancellation token; the owner is destroyed after the body
  /// returns (or short-circuits on a pre-cancelled token).
  struct DetachedTask {
    /// Closure run by the spawned thread. Empty when the body has been
    /// moved away or never assigned.
    std::function<void()> body;
    /// Cancellation token consulted before the body runs and again at
    /// any cooperation points the body chooses to insert.
    CancellationToken token;
  };

  /// Trampoline executed on the spawned detached-task thread. Observes
  /// cancellation, runs the body, captures any thrown exception into the
  /// pool's slot, and decrements the in-flight counter under the destructor's
  /// wait condition.
  static void runDetached(ThreadPool *self, DetachedTask *raw) noexcept {
    std::unique_ptr<DetachedTask> owner(raw);
    if (!owner->token.stop_requested()) {
      try {
        if (owner->body) {
          owner->body();
        }
      } catch (...) {
        const std::scoped_lock<std::mutex> lock(self->m_detachedMutex);
        if (!self->m_detachedException) {
          self->m_detachedException = std::current_exception();
        }
      }
    }
    // Decrement the in-flight count BEFORE destroying the closure. If the
    // closure owns the last `shared_ptr<ThreadPool>`, destroying it triggers
    // `~ThreadPool()` synchronously on this detached thread. The destructor
    // calls `waitForDetachedDrain()`, which would self-deadlock if the count
    // were still 1.
    //
    // Hold `m_detachedMutex` across the fetch_sub + notify so a waiter cannot
    // miss the wake-up between its predicate check and its `wait()`. Once the
    // lock is released the closure may destroy the pool object on this same
    // thread; do NOT touch `self` after `owner.reset()`.
    {
      const std::scoped_lock<std::mutex> lock(self->m_detachedMutex);
      self->m_detachedInFlight.fetch_sub(1, std::memory_order_acq_rel);
      self->m_detachedDone.notify_all();
    }
    owner.reset();
  }

  /// Block until every in-flight detached task has decremented the counter.
  /// Idempotent; called from the destructor before the synchronous-worker
  /// shutdown path runs so detached bodies never race the pool's teardown.
  void waitForDetachedDrain() noexcept {
    std::unique_lock<std::mutex> lock(m_detachedMutex);
    m_detachedDone.wait(lock, [this]() noexcept {
      return m_detachedInFlight.load(std::memory_order_acquire) == 0U;
    });
  }

  /// pthread entry trampoline that pins the worker, registers TLS, and runs
  /// the main loop.
  static void *workerEntry(void *raw) noexcept {
    auto *arg = static_cast<WorkerSpawnArg *>(raw);
    auto &self = *(arg->pool->m_workers.get() + arg->workerIndex);
    // Mirror the pre-spawn `pthread_attr_setaffinity_np` gate: only bind
    // when the user actually asked for pinning. Unconditional rebind
    // silently defeats `Affinity::None`.
    if (self.cpuId != UINT32_MAX &&
        arg->pool->m_workerAffinity != Affinity::None) {
      detail::bindAffinityOnce(self.cpuId);
    }
    auto &ctx = tlsContext();
    ctx.slot = arg->workerIndex;
    ctx.insidePoolWorker = true;
    ctx.pool = arg->pool;
    ctx.kind = arg->pool->m_kind;
    ctx.arenaIndex = arg->pool->m_arenaIndex;
    arg->pool->m_workersReady.fetch_add(1, std::memory_order_release);
    detail::workerMainLoop(self, arg->pool->m_control);
    ctx.slot = 0;
    ctx.insidePoolWorker = false;
    ctx.pool = nullptr;
    ctx.kind = PoolKind::Standalone;
    ctx.arenaIndex = 0;
    return nullptr;
  }

#ifndef __linux__
  /// `std::thread` entry used by the non-Linux fallback path; mirrors
  /// `workerEntry`. Pins the worker to its slot's reserved CPU when one is
  /// recorded (Windows path resolves real CPU ids via `detectTopology`).
  static void workerEntryStdThread(ThreadPool *self,
                                   std::size_t workerIndex) noexcept {
    auto &state = *(self->m_workers.get() + workerIndex);
    // Same gate as the pthread trampoline: respect `Affinity::None`.
    if (state.cpuId != UINT32_MAX && self->m_workerAffinity != Affinity::None) {
      detail::bindAffinityOnce(state.cpuId);
    }
    auto &ctx = tlsContext();
    ctx.slot = workerIndex;
    ctx.insidePoolWorker = true;
    ctx.pool = self;
    ctx.kind = self->m_kind;
    ctx.arenaIndex = self->m_arenaIndex;
    self->m_workersReady.fetch_add(1, std::memory_order_release);
    detail::workerMainLoop(state, self->m_control);
    ctx.slot = 0;
    ctx.insidePoolWorker = false;
    ctx.pool = nullptr;
    ctx.kind = PoolKind::Standalone;
    ctx.arenaIndex = 0;
  }
#endif

  /// Inline fallback: run the body over `[first, last)` directly on the
  /// producer thread. Used when the pool has only one participant or when the
  /// inline-fallback gate (`shouldRunInline`) fires. A pre-cancelled |tok|
  /// short-circuits the body. Producer TLS context is left untouched so
  /// nested-call detection still reports the outer worker's slot.
  ///
  /// Single-call flavor: invokes `fn(first, last)` once. Used when the user
  /// did not request an explicit chunk size.
  template <class F>
  void runInline(std::size_t first, std::size_t last, F &&fn,
                 const CancellationToken &tok) {
    CITOR_COUNTERS_INC(inlineFallbacks);
    if (tok.stop_requested()) {
      CITOR_COUNTERS_INC(cancellationStops);
      return;
    }
    fn(first, last);
  }

  /// Chunked inline fallback: invokes `fn(lo, hi)` for each chunk-sized span
  /// in `[first, last)`. Used by call sites that propagate a non-zero
  /// `HintsT::chunk` so the inline path observes the same chunk boundaries
  /// the parallel path would have produced. Token is polled between chunks
  /// when `cancelChecks` is true so a long range can still be cancelled
  /// mid-flight.
  template <class F>
  void runInlineChunked(std::size_t first, std::size_t last, std::size_t chunk,
                        bool cancelChecks, F &&fn,
                        const CancellationToken &tok) {
    CITOR_COUNTERS_INC(inlineFallbacks);
    if (tok.stop_requested()) {
      CITOR_COUNTERS_INC(cancellationStops);
      return;
    }
    if (chunk == 0U || chunk >= (last - first)) {
      fn(first, last);
      return;
    }
    for (std::size_t lo = first; lo < last; lo += chunk) {
      if (cancelChecks && tok.stop_requested()) {
        CITOR_COUNTERS_INC(cancellationStops);
        return;
      }
      const std::size_t hi = std::min(lo + chunk, last);
      fn(lo, hi);
    }
  }

  /// Overflow-safe ceiling division: returns `ceil(a / b)` without computing
  /// `a + b - 1`. Used wherever the chunk-shape math could otherwise wrap
  /// when |a| is near `SIZE_MAX`. |b| must be non-zero.
  static constexpr std::size_t ceilDiv(std::size_t a, std::size_t b) noexcept {
    return (a / b) + ((a % b) != 0U ? 1U : 0U);
  }

  /// Computes and stores the block shape (`chunk`, `blockCount`) on
  /// `|desc|`. The hint's `chunk` overrides when non-zero; otherwise
  /// defaults to `ceil(n / participants)` so each worker handles one
  /// contiguous block. `|oversubscribe|` selects 2x oversubscription
  /// (DynamicChunked) versus 1x (StaticUniform).
  [[gnu::always_inline]] static void
  fillBlockShape(detail::JobDescriptor &desc, std::size_t n,
                 std::size_t participants, std::size_t hintChunk,
                 bool oversubscribe = true) noexcept {
    // Callers gate `n == 0` upstream (parallelFor's `last <= first` early
    // return) and `participants` is constructor-initialised to >= 1, so
    // `ceilDiv(n, P*2)` / `ceilDiv(n, P)` and the subsequent
    // `ceilDiv(n, chunk)` are both always >= 1; the post-divide
    // `chunk == 0` / `blockCount == 0` guards the previous version carried
    // are dead.
    std::size_t chunk = hintChunk;
    if (chunk == 0) {
      // Auto-derive `chunk`. DynamicChunked benefits from 2x oversubscription
      // (the second block is the unit a fast worker absorbs when a peer
      // straggles); StaticUniform doesn't steal so 1x is enough and gives
      // every rank a single contiguous range, saving one FunctionRef call
      // per rank on cheap-body workloads.
      const std::size_t denom =
          oversubscribe ? participants * 2U : participants;
      chunk = ceilDiv(n, denom);
    }
    const std::size_t blockCount = ceilDiv(n, chunk);
    // Check-before-write keeps the desc cache line MESI-Shared across
    // iterations when the shape is loop-invariant (steady-state bench).
    if (desc.chunk != chunk) {
      desc.chunk = chunk;
    }
    if (desc.blockCount != blockCount) {
      desc.blockCount = blockCount;
    }
  }

  /// Maximum number of chunks the default reduce-chunk derivation produces.
  /// Must remain independent of the participant count so cross-`nJobs`
  /// bit-identity holds. Sized roughly twice the largest `participants` value
  /// the pool can hold so each worker gets at least two chunks for partition
  /// stability.
  static constexpr std::size_t kReduceMaxChunks = 64;
  /// Reserved stack-scratch budget for the reduce partials buffer. The
  /// reducer falls back to heap allocation when `kReduceMaxChunks *
  /// sizeof(T)` exceeds this byte budget.
  static constexpr std::size_t kReduceStackScratchBytes =
      std::size_t{32U} * 1024U;

  /// Derive a deterministic chunk size for `parallelReduce` calls. Purely a
  /// function of |n| and the optional hint chunk override and is independent of
  /// participant count. This invariant makes the chunk-id pairwise tree shape
  /// stable across worker counts (Demmel-Nguyen TOMS 2014). Never zero when
  /// `n > 0`.
  static std::size_t reduceChunkSize(std::size_t n,
                                     std::size_t hintChunk) noexcept {
    if (hintChunk != 0) {
      return hintChunk;
    }
    if (n <= kReduceMaxChunks) {
      return 1;
    }
    return ceilDiv(n, kReduceMaxChunks);
  }

  /// Inline-fallback reducer used when the pool gate routes the call to the
  /// producer. Walks the range using the same chunk schedule as the parallel
  /// path (chunk size is `f(n, HintsT)` only) so FixedBlockOrder /
  /// KahanCompensated reductions remain bit-identical to the worker-fanned-out
  /// path.
  /// Inline-fallback reducer used when the pool's participant count is one
  /// (cold path; the parallel engine takes the multi-participant path). |chunk|
  /// and |determinism| may come from a compile-time `HintsT` (typed entry) or
  /// a runtime `Hints` instance (untyped entry); the body is the same loop +
  /// pairwise-tree combine in either case.
  template <class T, class Map, class Combine>
  T runReduceInlineImpl(std::size_t first, std::size_t last, T init, Map &&map,
                        Combine &&combine, std::size_t chunkHint,
                        Determinism determinism, const CancellationToken &tok) {
    if (tok.stop_requested()) {
      throw cancelled_value_exception<T>(std::move(init));
    }
    const std::size_t n = last - first;
    const std::size_t chunk = reduceChunkSize(n, chunkHint);
    const std::size_t nChunks = ceilDiv(n, chunk);
    // Per-chunk cancellation poll, gated on `tok.canStop()` so the
    // never-stopped sentinel pays one null-pointer test. The parallel
    // path compiles the check away through `HintsT::cancellationChecks`;
    // the inline impl is shared with the runtime entry, so the gate is here.
    const bool canStop = tok.canStop();

    if (determinism == Determinism::KahanCompensated) {
      std::vector<detail::KahanPair> partials;
      partials.reserve(nChunks);
      for (std::size_t b = 0; b < nChunks; ++b) {
        if (canStop && tok.stop_requested()) [[unlikely]] {
          break;
        }
        const std::size_t lo = first + (b * chunk);
        const std::size_t hi = std::min(lo + chunk, last);
        const T mapped = map(lo, hi);
        partials.push_back(
            detail::kahanAdd(detail::KahanPair{}, static_cast<double>(mapped)));
      }
      if (partials.empty()) {
        throw cancelled_value_exception<T>(std::move(init));
      }
      const detail::KahanPair treeResult = detail::pairwiseTreeCombine(
          partials, [](detail::KahanPair a, detail::KahanPair b) {
            return detail::kahanCombine(a, b);
          });
      const auto treeT = static_cast<T>(treeResult.sum);
      T combined = combine(std::move(init), treeT);
      if (partials.size() < nChunks) {
        throw cancelled_value_exception<T>(std::move(combined));
      }
      return combined;
    }
    std::vector<T> partials;
    partials.reserve(nChunks);
    for (std::size_t b = 0; b < nChunks; ++b) {
      if (canStop && tok.stop_requested()) [[unlikely]] {
        break;
      }
      const std::size_t lo = first + (b * chunk);
      const std::size_t hi = std::min(lo + chunk, last);
      partials.push_back(map(lo, hi));
    }
    if (partials.empty()) {
      throw cancelled_value_exception<T>(std::move(init));
    }
    auto wrappedCombine = [&combine](T a, T b) {
      return combine(std::move(a), std::move(b));
    };
    T treeResult = detail::pairwiseTreeCombine(partials, wrappedCombine);
    T combined = combine(std::move(init), std::move(treeResult));
    if (partials.size() < nChunks) {
      throw cancelled_value_exception<T>(std::move(combined));
    }
    return combined;
  }

  /// Compile-time-hinted wrapper over `runReduceInlineImpl` that pulls
  /// `chunk` and `determinism` from `HintsT`.
  template <class HintsT, class T, class Map, class Combine>
  T runReduceInline(std::size_t first, std::size_t last, T init, Map &&map,
                    Combine &&combine, const CancellationToken &tok) {
    return runReduceInlineImpl(first, last, std::move(init),
                               std::forward<Map>(map),
                               std::forward<Combine>(combine), HintsT::chunk,
                               HintsT::determinism, tok);
  }

  /// Runtime-hint wrapper over `runReduceInlineImpl` that forwards the
  /// runtime `|hints|` chunk and determinism instead of a compile-time
  /// `HintsT`.
  template <class T, class Map, class Combine>
  T runReduceInlineRuntime(std::size_t first, std::size_t last, T init,
                           Map &&map, Combine &&combine, const Hints &hints,
                           const CancellationToken &tok) {
    return runReduceInlineImpl(
        first, last, std::move(init), std::forward<Map>(map),
        std::forward<Combine>(combine), hints.chunk, hints.determinism, tok);
  }

  /// Parallel reducer engine for a compile-time `HintsT`. Allocates a
  /// `std::vector<PartialSlot>` (per-element cache-line padded to avoid
  /// false-sharing across adjacent chunk slots) sized to `nChunks`, dispatches
  /// a `void(lo, hi)` worker body that computes the partial via |map| and
  /// stores into `partials[blockId]`, then combines via the chunk-id pairwise
  /// tree. The Kahan path wraps `T` in `KahanPair`.
  template <class HintsT, class T, class Map, class Combine>
  T runReduceParallel(std::size_t first, std::size_t last, T init, Map &&map,
                      Combine &&combine, CancellationToken tok,
                      std::size_t participants) {
    const std::size_t n = last - first;
    const std::size_t chunk = reduceChunkSize(n, HintsT::chunk);
    const std::size_t nChunks = ceilDiv(n, chunk);
    constexpr bool kCancellationActive = detail::kCancellationActive<HintsT>;

    if constexpr (HintsT::determinism == Determinism::KahanCompensated) {
      if constexpr (!kCancellationActive) {
        auto runWithPartials = [&](auto &partials,
                                   std::size_t partialCount) -> T {
          auto bodyWrapper = [&](std::size_t blockId, std::size_t lo,
                                 std::size_t hi) {
            const T mapped = map(lo, hi);
            partials[blockId] = detail::kahanAdd(detail::KahanPair{},
                                                 static_cast<double>(mapped));
          };
          dispatchReduceJobTyped<HintsT>(first, last, bodyWrapper,
                                         std::move(tok), participants, chunk,
                                         nChunks);

          std::size_t step = 1;
          while (step < partialCount) {
            const std::size_t stride = step * 2U;
            for (std::size_t i = 0; i + step < partialCount; i += stride) {
              partials[i] =
                  detail::kahanCombine(partials[i], partials[i + step]);
            }
            step = stride;
          }
          const auto treeT = static_cast<T>(partials.front().sum);
          return combine(std::move(init), treeT);
        };

        if constexpr (sizeof(detail::KahanPair) * kReduceMaxChunks <=
                      kReduceStackScratchBytes) {
          if (nChunks <= kReduceMaxChunks) {
            std::array<detail::KahanPair, kReduceMaxChunks> partials;
            return runWithPartials(partials, nChunks);
          }
        }
        std::vector<detail::KahanPair> partials(nChunks);
        return runWithPartials(partials, nChunks);
      }

      // Kahan path: accumulate per chunk into a KahanPair then
      // compensate-combine across chunks. The user's combine is treated as `+`;
      // for non-additive Kahan use cases the caller picks FixedBlockOrder
      // instead. The completion flag lives on the same padded line as the
      // partial so workers writing different chunks never share a cache line.
      struct alignas(kCacheLine) KahanSlot {
        detail::KahanPair value{};
        std::uint8_t done = 0;
      };

      auto runWithPartials = [&](auto &partials,
                                 std::size_t partialCount) -> T {
        auto bodyWrapper = [&](std::size_t blockId, std::size_t lo,
                               std::size_t hi) {
          const T mapped = map(lo, hi);
          partials[blockId].value = detail::kahanAdd(
              detail::KahanPair{}, static_cast<double>(mapped));
          partials[blockId].done = 1;
        };
        dispatchReduceJobTyped<HintsT>(first, last, bodyWrapper, std::move(tok),
                                       participants, chunk, nChunks);

        if constexpr (!kCancellationActive) {
          std::size_t step = 1;
          while (step < partialCount) {
            const std::size_t stride = step * 2U;
            for (std::size_t i = 0; i + step < partialCount; i += stride) {
              partials[i].value = detail::kahanCombine(
                  partials[i].value, partials[i + step].value);
            }
            step = stride;
          }
          const auto treeT = static_cast<T>(partials.front().value.sum);
          return combine(std::move(init), treeT);
        }

        // Single pass over `partials[]`: collect completed chunks into `flat[]`
        // and remember whether any chunk was missed in one cache-line walk
        // instead of two.
        std::vector<detail::KahanPair> flat;
        flat.reserve(partialCount);
        bool anyMissed = false;
        for (std::size_t i = 0; i < partialCount; ++i) {
          if (partials[i].done != 0U) {
            flat.push_back(partials[i].value);
          } else {
            anyMissed = true;
          }
        }
        // Empty partials (every chunk cancelled before completing): return
        // |init| unchanged via the cancelled-value exception. We do NOT
        // combine `init` with `T{}` because the user |combine| is not
        // required to satisfy `combine(x, T{}) == x`.
        if (flat.empty()) {
          throw cancelled_value_exception<T>(std::move(init));
        }
        const detail::KahanPair treeResult = detail::pairwiseTreeCombine(
            flat, [](detail::KahanPair a, detail::KahanPair b) {
              return detail::kahanCombine(a, b);
            });
        const auto treeT = static_cast<T>(treeResult.sum);
        T combined = combine(std::move(init), treeT);
        if (anyMissed) {
          throw cancelled_value_exception<T>(std::move(combined));
        }
        return combined;
      };

      if constexpr (sizeof(KahanSlot) * kReduceMaxChunks <=
                    kReduceStackScratchBytes) {
        if (nChunks <= kReduceMaxChunks) {
          std::array<KahanSlot, kReduceMaxChunks> partials;
          return runWithPartials(partials, nChunks);
        }
      }
      std::vector<KahanSlot> partials(nChunks);
      return runWithPartials(partials, nChunks);
    } else {
      if constexpr (!kCancellationActive) {
        auto runWithPartials = [&](auto &partials,
                                   std::size_t partialCount) -> T {
          auto bodyWrapper = [&](std::size_t blockId, std::size_t lo,
                                 std::size_t hi) {
            partials[blockId] = map(lo, hi);
          };
          dispatchReduceJobTyped<HintsT>(first, last, bodyWrapper,
                                         std::move(tok), participants, chunk,
                                         nChunks);

          auto wrappedCombine = [&combine](T a, T b) {
            return combine(std::move(a), std::move(b));
          };
          std::size_t step = 1;
          while (step < partialCount) {
            const std::size_t stride = step * 2U;
            for (std::size_t i = 0; i + step < partialCount; i += stride) {
              partials[i] = wrappedCombine(std::move(partials[i]),
                                           std::move(partials[i + step]));
            }
            step = stride;
          }
          return combine(std::move(init), std::move(partials.front()));
        };

        if constexpr (sizeof(T) * kReduceMaxChunks <=
                      kReduceStackScratchBytes) {
          if (nChunks <= kReduceMaxChunks) {
            std::array<T, kReduceMaxChunks> partials;
            return runWithPartials(partials, nChunks);
          }
        }
        std::vector<T> partials(nChunks);
        return runWithPartials(partials, nChunks);
      }

      // FixedBlockOrder / OrderTolerant share the same dispatch shape; the only
      // difference is whether the caller's combine is
      // bit-reproducible-friendly. We still use the chunk-id pairwise tree so
      // FixedBlockOrder is bit-identical across worker counts.
      struct alignas(kCacheLine) Slot {
        T value;
        std::uint8_t done = 0;
      };

      auto runWithPartials = [&](auto &partials,
                                 std::size_t partialCount) -> T {
        auto bodyWrapper = [&](std::size_t blockId, std::size_t lo,
                               std::size_t hi) {
          partials[blockId].value = map(lo, hi);
          partials[blockId].done = 1;
        };
        dispatchReduceJobTyped<HintsT>(first, last, bodyWrapper, std::move(tok),
                                       participants, chunk, nChunks);

        if constexpr (!kCancellationActive) {
          auto wrappedCombine = [&combine](T a, T b) {
            return combine(std::move(a), std::move(b));
          };
          std::size_t step = 1;
          while (step < partialCount) {
            const std::size_t stride = step * 2U;
            for (std::size_t i = 0; i + step < partialCount; i += stride) {
              partials[i].value =
                  wrappedCombine(std::move(partials[i].value),
                                 std::move(partials[i + step].value));
            }
            step = stride;
          }
          return combine(std::move(init), std::move(partials.front().value));
        }

        // Single pass over `partials[]`: collect completed chunks into `flat[]`
        // and remember whether any chunk was missed in one cache-line walk
        // instead of two.
        std::vector<T> flat;
        flat.reserve(partialCount);
        bool anyMissed = false;
        for (std::size_t i = 0; i < partialCount; ++i) {
          if (partials[i].done != 0U) {
            flat.push_back(std::move(partials[i].value));
          } else {
            anyMissed = true;
          }
        }
        // Empty partials (every chunk cancelled): return |init| unchanged. See
        // the Kahan branch above for why combining with `T{}` is unsafe for
        // arbitrary user combiners.
        if (flat.empty()) {
          throw cancelled_value_exception<T>(std::move(init));
        }
        auto wrappedCombine = [&combine](T a, T b) {
          return combine(std::move(a), std::move(b));
        };
        T treeResult = detail::pairwiseTreeCombine(flat, wrappedCombine);
        T combined = combine(std::move(init), std::move(treeResult));
        if (anyMissed) {
          throw cancelled_value_exception<T>(std::move(combined));
        }
        return combined;
      };

      if constexpr (sizeof(Slot) * kReduceMaxChunks <=
                    kReduceStackScratchBytes) {
        if (nChunks <= kReduceMaxChunks) {
          std::array<Slot, kReduceMaxChunks> partials;
          return runWithPartials(partials, nChunks);
        }
      }
      // Default-construct via `resize`: `Slot::value` is read only when
      // `done != 0`, so leaving `value` uninitialized for unused slots saves
      // one fill-copy per chunk and drops the copy-constructible requirement
      // on `T` for slot construction.
      std::vector<Slot> partials;
      partials.resize(nChunks);
      return runWithPartials(partials, nChunks);
    }
  }

  /// Runtime-hint parallel reducer; selects the determinism path via a runtime
  /// branch.
  template <class T, class Map, class Combine>
  T runReduceParallelRuntime(std::size_t first, std::size_t last, T init,
                             Map &&map, Combine &&combine, const Hints &hints,
                             CancellationToken tok, std::size_t participants) {
    const std::size_t n = last - first;
    const std::size_t chunk = reduceChunkSize(n, hints.chunk);
    const std::size_t nChunks = ceilDiv(n, chunk);

    if (hints.determinism == Determinism::KahanCompensated) {
      struct alignas(kCacheLine) KahanSlot {
        detail::KahanPair value{};
        std::uint8_t done = 0;
      };
      std::vector<KahanSlot> partials(nChunks);

      auto bodyWrapper = [&](std::size_t lo, std::size_t hi) {
        const std::size_t blockId = (lo - first) / chunk;
        const T mapped = map(lo, hi);
        partials[blockId].value =
            detail::kahanAdd(detail::KahanPair{}, static_cast<double>(mapped));
        partials[blockId].done = 1;
      };
      const FunctionRef<void(std::size_t, std::size_t)> body{bodyWrapper};
      dispatchReduceJob(first, last, body, std::move(tok), participants, chunk,
                        nChunks);

      std::vector<detail::KahanPair> flat;
      flat.reserve(nChunks);
      bool anyMissed = false;
      for (std::size_t i = 0; i < nChunks; ++i) {
        if (partials[i].done != 0U) {
          flat.push_back(partials[i].value);
        } else {
          anyMissed = true;
        }
      }
      if (flat.empty()) {
        throw cancelled_value_exception<T>(std::move(init));
      }
      const detail::KahanPair treeResult = detail::pairwiseTreeCombine(
          flat, [](detail::KahanPair a, detail::KahanPair b) {
            return detail::kahanCombine(a, b);
          });
      const auto treeT = static_cast<T>(treeResult.sum);
      T combined = combine(std::move(init), treeT);
      if (anyMissed) {
        throw cancelled_value_exception<T>(std::move(combined));
      }
      return combined;
    }

    struct alignas(kCacheLine) Slot {
      T value;
      std::uint8_t done = 0;
    };
    std::vector<Slot> partials;
    partials.resize(nChunks);

    auto bodyWrapper = [&](std::size_t lo, std::size_t hi) {
      const std::size_t blockId = (lo - first) / chunk;
      partials[blockId].value = map(lo, hi);
      partials[blockId].done = 1;
    };
    const FunctionRef<void(std::size_t, std::size_t)> body{bodyWrapper};
    dispatchReduceJob(first, last, body, std::move(tok), participants, chunk,
                      nChunks);

    std::vector<T> flat;
    flat.reserve(nChunks);
    bool anyMissed = false;
    for (std::size_t i = 0; i < nChunks; ++i) {
      if (partials[i].done != 0U) {
        flat.push_back(std::move(partials[i].value));
      } else {
        anyMissed = true;
      }
    }
    if (flat.empty()) {
      throw cancelled_value_exception<T>(std::move(init));
    }
    auto wrappedCombine = [&combine](T a, T b) {
      return combine(std::move(a), std::move(b));
    };
    T treeResult = detail::pairwiseTreeCombine(flat, wrappedCombine);
    T combined = combine(std::move(init), std::move(treeResult));
    if (anyMissed) {
      throw cancelled_value_exception<T>(std::move(combined));
    }
    return combined;
  }

  /// Single-thread fallback: run every phase of the plex on the producer
  /// thread. Triggered when the pool has at most one participant. Slot 0 owns
  /// the entire range `[0, n)`. `prePhaseFn(phaseIdx)` is invoked before each
  /// phase; cancellation is observed before each phase and the call returns
  /// cleanly when the token is stopped.
  template <class Phase, class PrePhase>
  void runPlexInline(std::size_t nPhases, std::size_t n, Phase &&phaseFn,
                     PrePhase &&prePhaseFn, const CancellationToken &tok) {
    for (std::size_t p = 0; p < nPhases; ++p) {
      if (tok.stop_requested()) {
        return;
      }
      prePhaseFn(p);
      phaseFn(p, std::uint32_t{0}, std::size_t{0}, n);
    }
  }

  /// Persistent-worker plex driver: publish one dispatch and drive `nPhases`
  /// phases.
  ///
  /// Allocates a stack-resident `PlexState` with per-worker `done[]` slots on
  /// dedicated lines. The wrapper body, invoked with `(slot, slot+1)`, runs
  /// the per-slot phase loop: spin until `currentPhase >= localPhase`, run
  /// `phaseFn`, signal `done[slot]`. The dispatch publishes a single job with
  /// `blockCount = participants` and `chunk = 1` so every worker receives
  /// exactly one body invocation; `runStaticPartition` matches block id to
  /// slot id. The producer participates as slot 0; its body drives the phase
  /// epoch (publish next phase, run slot 0's `phaseFn`, then spin-wait on
  /// every other slot's `done`). The descriptor's join after dispatch returns
  /// establishes happens-before for every worker's writes; any captured
  /// exception is rethrown by `dispatchOne`.
  ///
  /// Wait for every non-producer slot to reach |pStamp| on its `done`
  /// counter. Used by `runPlexParallel` to close a phase: the producer joins
  /// on every other slot's `done >= pStamp` before publishing the next phase
  /// (or before returning on a cancellation path).
  [[gnu::always_inline]] static void
  waitPlexSlotsReachStamp(detail::PlexState &state,
                          std::uint64_t pStamp) noexcept {
    for (std::uint32_t s = 1; s < state.participants; ++s) {
      while (true) {
        const std::uint64_t obs =
            state.doneSlot(s).done.load(std::memory_order_acquire);
        if (obs >= pStamp) {
          break;
        }
        detail::cpuRelax();
      }
    }
  }

  /// Pre-phase hook: the producer invokes `prePhaseFn(p - 1)` before each
  /// release-store on `currentPhase`. The hook synchronises with workers'
  /// acquire-spin via the same release-store, so any state the hook writes is
  /// visible to every worker that observes the new phase.
  template <class Phase, class PrePhase>
  void runPlexParallel(std::size_t nPhases, std::size_t n, Phase &&phaseFn,
                       PrePhase &&prePhaseFn, CancellationToken tok,
                       std::size_t participants) {
    // Acquire the dispatch gate BEFORE touching pool-owned scratch
    // (`m_plexDoneSlots`, `m_plexEpochBase`). Concurrent producers would
    // otherwise race on the epoch advance.
    const DispatchLease lease(*this, Priority::Throughput);

    detail::PlexState state;
    state.nPhases = nPhases;
    state.n = n;
    state.participants = static_cast<std::uint32_t>(participants);
    state.doneSlots = m_plexDoneSlots.get();
    // Reserve a fresh epoch interval in `m_plexEpochBase`. Workers stamp
    // absolute targets
    // (`epochBase + p`); the next dispatch starts at `epochBase + nPhases + 1`,
    // so prior-call stamps cannot satisfy this call's waits. The wrap guard
    // zero-resets and rebases when the counter approaches `UINT64_MAX`; with
    // typical advance rates the branch is unreachable.
    if (m_plexEpochBase > UINT64_MAX - 1024U) [[unlikely]] {
      auto *slotsBase = m_plexDoneSlots.get();
      for (std::size_t i = 0; i < participants; ++i) {
        slotsBase[i].done.store(0, std::memory_order_relaxed);
      }
      m_plexEpochBase = 0;
    }
    state.epochBase = m_plexEpochBase;
    m_plexEpochBase += static_cast<std::uint64_t>(nPhases) + 1U;

    // The plex's cancellation token lives on the producer's stack; we DO NOT
    // move it into the descriptor's token field because `runStaticPartition`
    // consults `desc.token.stop_requested` before admitting each block. With
    // `blockCount = participants` and `chunk = 1`, every slot has exactly one
    // block; a stopped token would cause workers to skip their slot body
    // entirely, deadlocking the producer's slot-0 join. The wrapper observes
    // the token directly at phase boundaries instead.
    CancellationToken plexTok = std::move(tok);

    // The wrapper body is invoked once per slot (chunkBlock == 1). Each
    // invocation runs the full phase loop for that slot. Slot 0 (the producer)
    // drives the phase epoch and joins on every other slot's `done` between
    // phases; non-zero slots spin-wait on `currentPhase` and signal `done`
    // after each phase's work.
    auto bodyWrapper = [&state, &phaseFn, &prePhaseFn,
                        &plexTok](std::size_t lo, std::size_t /*hi*/) {
      const auto slot = static_cast<std::uint32_t>(lo);
      const std::size_t totalPhases = state.nPhases;
      const std::uint64_t epochBase = state.epochBase;
      const std::uint64_t finalStamp =
          epochBase + static_cast<std::uint64_t>(totalPhases);
      const auto [slotLo, slotHi] = state.slotRange(slot);

      if (slot == 0U) {
        // Producer drives the phase epoch.
        for (std::size_t p = 1; p <= totalPhases; ++p) {
          const std::uint64_t pStamp =
              epochBase + static_cast<std::uint64_t>(p);
          // Cancellation check before publishing the next phase. Stop on either
          // an external stop request OR a peer-observed throw / cancellation,
          // so the producer does not run prePhaseFn or slot-0's phaseFn for
          // phases beyond the failure.
          if (plexTok.stop_requested() ||
              state.phaseCancelled.load(std::memory_order_acquire) != 0U) {
            state.phaseCancelled.store(1U, std::memory_order_release);
            // Publish the cancelled epoch so workers leave their spin loops.
            state.currentPhase.store(p, std::memory_order_release);
            // Wait for every worker to observe and exit the phase loop. They
            // will set `done[slot]` to the final epoch stamp so we know they
            // have left.
            waitPlexSlotsReachStamp(state, pStamp);
            return;
          }
          // Pre-phase hook: write any shared state the upcoming phase reads.
          // The release-store on `currentPhase` below publishes these writes to
          // every worker.
          try {
            prePhaseFn(p - 1);
          } catch (...) {
            captureFirstException(state);
            state.phaseCancelled.store(1U, std::memory_order_release);
            state.currentPhase.store(p, std::memory_order_release);
            state.doneSlot(0).done.store(pStamp, std::memory_order_release);
            waitPlexSlotsReachStamp(state, pStamp);
            return;
          }
          // Publish the next phase via release: workers' acquire-spin observes
          // the new value and proceeds to run their slice for phase index `p -
          // 1`.
          state.currentPhase.store(p, std::memory_order_release);
          // Producer runs slot 0's slice for this phase.
          try {
            phaseFn(p - 1, std::uint32_t{0}, slotLo, slotHi);
          } catch (...) {
            captureFirstException(state);
            // Mark cancelled so workers exit their loops, then close the phase.
            state.phaseCancelled.store(1U, std::memory_order_release);
            state.doneSlot(0).done.store(pStamp, std::memory_order_release);
            waitPlexSlotsReachStamp(state, pStamp);
            return;
          }
          state.doneSlot(0).done.store(pStamp, std::memory_order_release);
          // Producer joins on every other slot's `done >= pStamp` before
          // publishing the next phase.
          waitPlexSlotsReachStamp(state, pStamp);
        }
      } else {
        // Background worker: spin on `currentPhase` and run our slice once per
        // phase.
        for (std::size_t p = 1; p <= totalPhases; ++p) {
          const std::uint64_t pStamp =
              epochBase + static_cast<std::uint64_t>(p);
          // Acquire-spin on `currentPhase` until the producer publishes our
          // next phase OR cancellation is observed. Both `currentPhase` advance
          // and `phaseCancelled` flip are release-stores by the producer; the
          // worker's acquire-loads pair with them so any state the producer
          // wrote is visible here.
          while (true) {
            const std::uint64_t observed =
                state.currentPhase.load(std::memory_order_acquire);
            if (observed >= p) {
              break;
            }
            if (state.phaseCancelled.load(std::memory_order_acquire) != 0U) {
              // Producer threw or cancelled before publishing this phase. Stamp
              // the final phase epoch on `done` so any future producer-side
              // join (none expected here, but the contract is the descriptor's
              // join sees a deterministic epoch) advances.
              state.doneSlot(slot).done.store(finalStamp,
                                              std::memory_order_release);
              return;
            }
            detail::cpuRelax();
          }
          if (state.phaseCancelled.load(std::memory_order_acquire) != 0U) {
            // Producer signalled cancellation while publishing this phase.
            // Signal we have observed the current phase so the producer's exit
            // join advances, then drain the remaining epochs.
            state.doneSlot(slot).done.store(pStamp, std::memory_order_release);
            if (p < totalPhases) {
              state.doneSlot(slot).done.store(finalStamp,
                                              std::memory_order_release);
            }
            return;
          }
          try {
            phaseFn(p - 1, slot, slotLo, slotHi);
          } catch (...) {
            captureFirstException(state);
            state.phaseCancelled.store(1U, std::memory_order_release);
            // Stamp the final epoch so the producer's exit join completes.
            state.doneSlot(slot).done.store(finalStamp,
                                            std::memory_order_release);
            return;
          }
          // Release so the producer's acquire-load observes our writes.
          state.doneSlot(slot).done.store(pStamp, std::memory_order_release);
        }
      }
    };

    const FunctionRef<void(std::size_t, std::size_t)> body{bodyWrapper};

    detail::JobDescriptor desc;
    desc.first = 0;
    desc.last = participants;
    desc.participants = static_cast<std::uint32_t>(participants);
    desc.balance = Balance::StaticUniform;
    desc.priority = Priority::Throughput;
    desc.body = body;
    // Leave `desc.token` default-constructed: the descriptor's stop-check would
    // gate `runStaticPartition` from admitting per-slot blocks, deadlocking the
    // producer's join. The wrapper observes `plexTok` at phase boundaries
    // instead.
    desc.chunk = 1;
    desc.blockCount = participants;

    // The dispatch gate is already held via `lease` above (so the epoch
    // reservation is serialized against concurrent producers); use the locked
    // variant which skips the redundant gate acquisition.
    dispatchOneStaticLocked<Balance::StaticUniform>(desc);

    // `dispatchOneStaticLocked` rethrows exceptions captured by
    // `desc.firstException`; the body wrapper routes plex-thrown exceptions
    // through `state.firstException` instead so the descriptor's capture slot
    // stays clean. Rethrow here.
    rethrowIfCaptured(state);
  }

  /// First-exception capture shared by every primitive (plex, chain, scan,
  /// forkJoin, dispatch). Stored on the protocol's state so the descriptor's
  /// own slot remains untouched (workers may also throw from the wrapper body
  /// itself, which the dispatch engine captures separately). The first thread
  /// to win the CAS owns the exception pointer; subsequent throws delete their
  /// own copy. |State| must expose a `firstException` atomic of
  /// `exception_ptr*`.
  template <class State>
  [[gnu::always_inline]] static void
  captureFirstException(State &state) noexcept {
    auto *eptr =
        new (std::nothrow) std::exception_ptr(std::current_exception());
    if (eptr == nullptr) {
      std::terminate();
    }
    std::exception_ptr *expected = nullptr; // NOLINT(misc-const-correctness)
    if (!state.firstException.compare_exchange_strong(
            expected, eptr, std::memory_order_release,
            std::memory_order_acquire)) {
      delete eptr;
    }
  }

  /// `BarrierKind::Global` rendezvous: each slot scans every peer's `done`
  /// epoch until all have reached |target|. Used by every slot (producer and
  /// non-producers alike) for `BarrierKind::Global` and
  /// `BarrierKind::DeterministicReduce`. Each slot stamps its own `done` to
  /// |target| then scans every peer's `done` line until all have reached
  /// |target|.
  ///
  /// Coherence model: each slot's `done` line is on its own cache line (no
  /// false sharing). After a slot's release-store the line transitions
  /// Modified on the writing core; readers fetch it via Modified -> Shared
  /// transitions. Because every slot scans in parallel, the wall-clock cost
  /// of the rendezvous is bounded by `(slowest_writer_finish_time) + (one
  /// cross-core coherence pull per peer)`, not by a producer's serial
  /// scan-then-publish chain.
  ///
  /// Cancellation handshake: a slot bailing on cancellation stamps
  /// `done = nStages` instead of |target|. Because `nStages > target` for
  /// every active stage, the wait condition `done >= target` is satisfied
  /// unconditionally, so waiters exit cleanly without an explicit
  /// `chainCancelled` poll inside the spin loop. The slot's own `done` line
  /// is skipped via the `s != self` guard so the loop does not pay an extra
  /// L1-load for self-traffic.
  static void waitAllSlotsDone(detail::ChainState &state, std::uint32_t self,
                               std::uint64_t target) noexcept {
    const std::uint64_t absoluteTarget = state.epochBase + target;
    const std::uint32_t participants = state.participants;
    // Phase-shift the peer scan: each slot starts from `(self + 1) mod
    // participants` and walks forward, so the first lines pulled by every slot
    // are different. The naive `for (s = 0; s < participants; ++s)` order has
    // every slot probing slot 0 first, which spikes contention on slot 0's
    // cache line at every barrier; the shifted order distributes the initial
    // pulls across all peer lines.
    for (std::uint32_t step = 1; step < participants; ++step) {
      const std::uint32_t s = (self + step < participants)
                                  ? self + step
                                  : self + step - participants;
      while (state.doneSlot(s).done.load(std::memory_order_acquire) <
             absoluteTarget) {
        detail::cpuRelax();
      }
    }
  }

  /// Wait for slot 0's `done` epoch to reach |target| before returning. Used
  /// for `BarrierKind::ProducerSerial`: workers other than slot 0 spin on the
  /// producer's release-store before either skipping the next stage's body or
  /// proceeding. Cancellation handshake mirrors `waitAllSlotsDone`: a
  /// cancelled producer stamps `done[0] = nStages` so the wait condition is
  /// satisfied without an explicit `chainCancelled` poll inside the spin loop.
  static void waitProducerSerialBarrier(detail::ChainState &state,
                                        std::uint64_t target) noexcept {
    const std::uint64_t absoluteTarget = state.epochBase + target;
    while (state.doneSlot(0).done.load(std::memory_order_acquire) <
           absoluteTarget) {
      detail::cpuRelax();
    }
  }

  /// Recursive helper: run stage |I| (and every later stage) for one slot.
  /// Uses a compile-time `if constexpr` ladder over `BarrierKind` so each
  /// stage's barrier becomes a branchless inlined call. The recursion
  /// terminates when `I == sizeof...(Stages)`.
  ///
  /// Pre-stage entry (driven by stage `I-1`'s barrier):
  /// - `None`: no wait. Each slot pipelines forward independently.
  /// - `Global` / `DeterministicReduce`: every slot scans every peer's
  ///   `done >= I` independently; no participant acts as a serial gate.
  ///   Coherence traffic per stage is bounded by the slowest writer's
  ///   release-store plus one cross-core pull per peer per slot, executed
  ///   in parallel across slots.
  /// - `ProducerSerial`: non-producers wait on slot 0's `done >= I`;
  ///   producer skips the wait.
  ///
  /// Stage execution:
  /// - `ProducerSerial`: only slot 0 invokes the body; other slots stamp
  ///   their `done` and skip.
  /// - Otherwise: every slot invokes the body for its `(slotLo, slotHi)`.
  ///
  /// Cancellation: between every stage the slot checks `chainCancelled`;
  /// when set, the slot stamps `done` to the final stage epoch and returns.
  template <std::size_t I, class StagesTuple>
  void runChainStageForSlot(std::uint32_t slot, std::size_t slotLo,
                            std::size_t slotHi, detail::ChainState &state,
                            StagesTuple &stages,
                            const CancellationToken &tok) noexcept {
    if constexpr (I < std::tuple_size_v<StagesTuple>) {
      // Apply pre-stage barrier driven by stage `I-1`'s post-stage barrier
      // kind. `None` means "no cross-slot wait" -- the chain keeps each slot on
      // its own contiguous chunk across stages, so the slot's prior
      // release-store satisfies any implicit dependency on its own upstream
      // output.
      const bool slotRunsBody = [&]() noexcept {
        if constexpr (I > 0) {
          using PrevStage = std::tuple_element_t<I - 1, StagesTuple>;
          constexpr BarrierKind kPrevBarrier = PrevStage::barrier;
          if constexpr (kPrevBarrier == BarrierKind::Global ||
                        kPrevBarrier == BarrierKind::DeterministicReduce) {
            // Decentralized Global rendezvous: every slot scans every peer's
            // `done` line in parallel. Each slot pays one cross-core pull per
            // peer, but the pulls happen in parallel across slots so the wall
            // time is bounded by the slowest writer plus one peer-scan latency
            // rather than by a producer's serial scan-then-publish chain.
            waitAllSlotsDone(state, slot, static_cast<std::uint64_t>(I));
            return true;
          } else if constexpr (kPrevBarrier == BarrierKind::ProducerSerial) {
            if (slot != 0U) {
              // Wait for slot 0 to finish stage `I` itself (its post-stage
              // stamp is `I + 1`), not just the previous stage. Otherwise a
              // non-producer skipping stage `I` could race past slot 0's
              // still-running serial body and enter stage `I + 1`.
              waitProducerSerialBarrier(state,
                                        static_cast<std::uint64_t>(I + 1));
              return false;
            }
            return true;
          } else {
            return true;
          }
        } else {
          return true;
        }
      }();

      // Cancellation observed at the stage boundary: stamp the final epoch and
      // bail.
      if (state.chainCancelled.load(std::memory_order_acquire) != 0U ||
          tok.stop_requested()) {
        if (tok.stop_requested()) {
          state.chainCancelled.store(1U, std::memory_order_release);
        }
        state.doneSlot(slot).done.store(
            state.epochBase + static_cast<std::uint64_t>(state.nStages),
            std::memory_order_release);
        return;
      }

      if (slotRunsBody) {
        try {
          std::get<I>(stages).fn(I, slot, slotLo, slotHi);
        } catch (...) {
          captureFirstException(state);
          state.chainCancelled.store(1U, std::memory_order_release);
          state.doneSlot(slot).done.store(
              state.epochBase + static_cast<std::uint64_t>(state.nStages),
              std::memory_order_release);
          return;
        }
      }

      // Stamp this slot's `done` for stage `I` only when a downstream consumer
      // might wait on it. Two readers exist:
      //   - The next stage's pre-barrier scan when THIS stage's barrier is
      //   Global /
      //     DeterministicReduce (reads `done[*] >= I + 1`).
      //   - Slot 0 in particular: when the PREV stage's barrier was
      //   ProducerSerial, every
      //     non-producer slot spins on `done[0] >= I + 1` to know slot 0
      //     finished THIS (the producer-serial) stage. Only slot 0 needs to
      //     stamp in that case.
      //   - When THIS stage's barrier is ProducerSerial, the next stage's
      //   pre-barrier wait is
      //     the ProducerSerial wait above (handled by THE NEXT stage via
      //     prev-was-PS), so the stamp is still required for slot 0 here too.
      // Other combinations (None post-stage with non-PS pre-stage) carry no
      // observable consumer and the release-store is dead weight.
      using ThisStage = std::tuple_element_t<I, StagesTuple>;
      constexpr BarrierKind kThisBarrier = ThisStage::barrier;
      constexpr bool kThisStampNeeded =
          (kThisBarrier == BarrierKind::Global ||
           kThisBarrier == BarrierKind::DeterministicReduce ||
           kThisBarrier == BarrierKind::ProducerSerial);
      constexpr bool kPrevWasProducerSerial = []() noexcept {
        if constexpr (I > 0) {
          using PrevStage = std::tuple_element_t<I - 1, StagesTuple>;
          return PrevStage::barrier == BarrierKind::ProducerSerial;
        } else {
          return false;
        }
      }();

      if constexpr (kThisStampNeeded) {
        state.doneSlot(slot).done.store(state.epochBase +
                                            static_cast<std::uint64_t>(I + 1),
                                        std::memory_order_release);
      } else if constexpr (kPrevWasProducerSerial) {
        if (slot == 0U) {
          state.doneSlot(0).done.store(state.epochBase +
                                           static_cast<std::uint64_t>(I + 1),
                                       std::memory_order_release);
        }
      }

      runChainStageForSlot<I + 1>(slot, slotLo, slotHi, state, stages, tok);
    }
  }

  /// True when `StageT`'s barrier permits the chain to use the
  /// dynamic-rebalance scheduler (`Global` or `DeterministicReduce`).
  template <class StageT>
  static consteval bool chainStageAllowsDynamicRebalance() noexcept {
    return StageT::barrier == BarrierKind::Global ||
           StageT::barrier == BarrierKind::DeterministicReduce;
  }

  /// Index-pack helper for `allChainStagesAllowDynamicRebalance`. Folds
  /// `chainStageAllowsDynamicRebalance` across every stage in the tuple.
  template <class StagesTuple, std::size_t... Is>
  static consteval bool allChainStagesAllowDynamicRebalanceImpl(
      std::index_sequence<Is...> /*unused*/) noexcept {
    return (... && chainStageAllowsDynamicRebalance<
                       std::tuple_element_t<Is, StagesTuple>>());
  }

  /// True when every stage in the chain permits dynamic rebalance. Used
  /// at the top of the chain dispatch to pick the scheduler.
  template <class StagesTuple>
  static consteval bool allChainStagesAllowDynamicRebalance() noexcept {
    return allChainStagesAllowDynamicRebalanceImpl<StagesTuple>(
        std::make_index_sequence<std::tuple_size_v<StagesTuple>>{});
  }

  /// Returns the per-stage block size for the dynamic chain scheduler.
  /// Honours `|hintChunk|` when non-zero; otherwise picks
  /// `ceil(n / (participants * 2))` so the scheduler has 2x
  /// oversubscription per slot for steal-driven rebalance.
  static constexpr std::size_t
  chainDynamicChunkSize(std::size_t n, std::size_t participants,
                        std::size_t hintChunk) noexcept {
    if (hintChunk != 0U) {
      return hintChunk;
    }
    return ceilDiv(n, participants * 2U);
  }

  /// Slot's main loop for the dynamic chain scheduler. Per stage, waits
  /// for the previous stage's barrier, then drains a per-stage atomic
  /// counter for blocks until the chain completes or is cancelled.
  template <std::size_t I, class StagesTuple>
  void runDynamicChainStageForSlot(std::uint32_t slot,
                                   detail::ChainState &state,
                                   StagesTuple &stages,
                                   const CancellationToken &tok) noexcept {
    if constexpr (I < std::tuple_size_v<StagesTuple>) {
      if constexpr (I > 0) {
        waitAllSlotsDone(state, slot, static_cast<std::uint64_t>(I));
      }

      if (state.chainCancelled.load(std::memory_order_acquire) != 0U ||
          tok.stop_requested()) {
        if (tok.stop_requested()) {
          state.chainCancelled.store(1U, std::memory_order_release);
        }
        state.doneSlot(slot).done.store(
            state.epochBase + static_cast<std::uint64_t>(state.nStages),
            std::memory_order_release);
        return;
      }

      auto &stageCounter = state.dynamicStageCounter(I);
      while (state.chainCancelled.load(std::memory_order_acquire) == 0U) {
        const std::size_t block =
            stageCounter.next.fetch_add(1, std::memory_order_relaxed);
        if (block >= state.dynamicBlockCount) {
          break;
        }
        const auto [lo, hi] = state.dynamicBlockRange(block);
        try {
          std::get<I>(stages).fn(I, slot, lo, hi);
        } catch (...) {
          captureFirstException(state);
          state.chainCancelled.store(1U, std::memory_order_release);
          state.doneSlot(slot).done.store(
              state.epochBase + static_cast<std::uint64_t>(state.nStages),
              std::memory_order_release);
          return;
        }
      }

      state.doneSlot(slot).done.store(state.epochBase +
                                          static_cast<std::uint64_t>(I + 1),
                                      std::memory_order_release);
      runDynamicChainStageForSlot<I + 1>(slot, state, stages, tok);
    }
  }

  /// Single-thread fallback: run every stage on the producer thread.
  /// Triggered when the pool has at most one participant or the stage pack is
  /// empty. Each stage runs with `slot = 0`, `lo = 0`, `hi = n`. Cancellation
  /// aborts the loop cleanly between stages. Stage exceptions propagate to
  /// the caller unchanged because the inline path has no shared state.
  template <class StagesTuple>
  void runChainInline(std::size_t n, const CancellationToken &tok,
                      StagesTuple &stages) {
    runChainInlineRec<0>(n, tok, stages);
  }

  /// Recursive worker for `runChainInline`. Walks every stage in order
  /// on the producer thread, observing cancellation between stages.
  template <std::size_t I, class StagesTuple>
  void runChainInlineRec(std::size_t n, const CancellationToken &tok,
                         StagesTuple &stages) {
    if constexpr (I < std::tuple_size_v<StagesTuple>) {
      if (tok.stop_requested()) {
        return;
      }
      std::get<I>(stages).fn(I, std::uint32_t{0}, std::size_t{0}, n);
      runChainInlineRec<I + 1>(n, tok, stages);
    }
  }

  /// Variadic chain dispatcher: publish one job, run every stage, rethrow on
  /// first throw. An empty stage pack returns immediately. With
  /// `participants <= 1` every stage runs inline on the producer. Otherwise
  /// a stack-resident `ChainState` is allocated, a wrapper body invokes
  /// `runChainStageForSlot<0>` for the slot's id, the call dispatches a
  /// static-uniform job with `chunk == 1` and `blockCount == participants`,
  /// and the chain's first captured exception (if any) is rethrown after the
  /// join.
  template <class ChainHintsT, class... Stages>
  void parallelChainImpl(std::size_t n, const CancellationToken &tok,
                         Stages &&...stages) {
    parallelChainImplWithRuntimeHints<ChainHintsT>(
        n, /*runtimeHints=*/nullptr, tok, std::forward<Stages>(stages)...);
  }

  /// Internal entry shared by the compile-time-hinted and runtime-hinted
  /// `parallelChain` overloads. `|runtimeHints|` is non-null when the
  /// runtime overload is in use; the compile-time overload passes
  /// `nullptr` and the implementation reads `ChainHintsT` instead.
  template <class ChainHintsT, class... Stages>
  void parallelChainImplWithRuntimeHints(std::size_t n,
                                         const ChainHints *runtimeHints,
                                         const CancellationToken &tok,
                                         Stages &&...stages) {
    constexpr std::size_t kStageCount = sizeof...(Stages);
    if constexpr (kStageCount == 0) {
      return;
    } else {
      auto stagePack =
          std::tuple<std::decay_t<Stages>...>(std::forward<Stages>(stages)...);

      const std::size_t participants = m_control.participants;
      if (participants <= 1 || shouldFallThroughCrossArena()) {
        runChainInline(n, tok, stagePack);
        return;
      }

      // Acquire the dispatch gate BEFORE touching pool-owned scratch
      // (`m_chainDoneSlots`, `m_chainEpochBase`). Concurrent producers would
      // otherwise race on the epoch advance and on the residual stamps left by
      // a peer's previous dispatch. Runtime path threads
      // `runtimeHints->priority` through; compile-time path uses
      // `ChainHintsT::priority`.
      constexpr bool kIsRuntimeHints = std::is_same_v<ChainHintsT, ChainHints>;
      Priority leasePriority = Priority::Throughput;
      if constexpr (kIsRuntimeHints) {
        if (runtimeHints != nullptr) {
          leasePriority = runtimeHints->priority;
        }
      } else {
        leasePriority = ChainHintsT::priority;
      }
      const DispatchLease lease(*this, leasePriority);

      // Reserve a fresh epoch interval in the pool's monotonic counter. The
      // interval covers stamps `[epochBase + 1, epochBase + nStages]`; the next
      // dispatch starts at `epochBase + nStages + 1` so prior-call stamps are
      // strictly below this call's targets. The wrap guard zero-resets and
      // rebases when the counter approaches `UINT64_MAX`; with typical advance
      // rates that branch is unreachable in practice.
      detail::ChainDoneSlot *const slotsBase = m_chainDoneSlots.get();
      const std::uint64_t epochAdvance =
          static_cast<std::uint64_t>(kStageCount) + 1U;
      if (m_chainEpochBase > UINT64_MAX - 1024U) [[unlikely]] {
        for (std::size_t i = 0; i < participants; ++i) {
          slotsBase[i].done.store(0, std::memory_order_relaxed);
        }
        m_chainEpochBase = 0;
      }
      const std::uint64_t epochBase = m_chainEpochBase;
      m_chainEpochBase += epochAdvance;

      detail::ChainState state;
      state.nStages = kStageCount;
      state.n = n;
      state.participants = static_cast<std::uint32_t>(participants);
      state.doneSlots = slotsBase;
      state.epochBase = epochBase;

      using StagePackT = decltype(stagePack);
      constexpr bool kRuntimeHints = std::is_same_v<ChainHintsT, ChainHints>;
      constexpr bool kDynamicRequested = []() consteval {
        if constexpr (kRuntimeHints) {
          return false;
        } else {
          return ChainHintsT::balance == Balance::DynamicChunked &&
                 !ChainHintsT::pipelineSameChunk;
        }
      }();
      constexpr bool kEveryStageCanBeRebalanced =
          allChainStagesAllowDynamicRebalance<StagePackT>();
      constexpr bool kUseDynamicRebalance =
          kDynamicRequested && kEveryStageCanBeRebalanced;

      auto dispatchSlotBody = [&](auto &bodyWrapper) {
        const FunctionRef<void(std::size_t, std::size_t)> body{bodyWrapper};

        detail::JobDescriptor desc;
        desc.first = 0;
        desc.last = participants;
        desc.participants = static_cast<std::uint32_t>(participants);
        desc.balance = Balance::StaticUniform;
        if constexpr (kRuntimeHints) {
          desc.priority = Priority::Throughput;
        } else {
          desc.priority = ChainHintsT::priority;
        }
        desc.body = body;
        // Leave `desc.token` default-constructed: the descriptor's stop-check
        // would gate `runStaticPartition` from admitting per-slot blocks,
        // deadlocking the chain rendezvous. The wrapper observes the
        // user-supplied token at stage boundaries instead.
        desc.chunk = 1;
        desc.blockCount = participants;

        dispatchOneStaticLocked<Balance::StaticUniform>(desc);
      };

      if constexpr (kUseDynamicRebalance) {
        if (n > 0U) {
          std::array<detail::ChainDynamicStageCounter, kStageCount>
              dynamicStageCounters;
          state.dynamicStageCounters = dynamicStageCounters.data();
          std::size_t hintChunk = 0;
          if constexpr (!kRuntimeHints) {
            hintChunk = ChainHintsT::chunk;
          }
          state.dynamicChunk =
              chainDynamicChunkSize(n, participants, hintChunk);
          state.dynamicBlockCount = ceilDiv(n, state.dynamicChunk);
          for (auto &counter : dynamicStageCounters) {
            counter.next.store(0, std::memory_order_relaxed);
          }

          auto bodyWrapper = [&state, &stagePack, &tok,
                              this](std::size_t lo, std::size_t /*hi*/) {
            const auto slot = static_cast<std::uint32_t>(lo);
            runDynamicChainStageForSlot<0>(slot, state, stagePack, tok);
          };
          dispatchSlotBody(bodyWrapper);
        } else {
          auto bodyWrapper = [&state, &stagePack, &tok,
                              this](std::size_t lo, std::size_t /*hi*/) {
            const auto slot = static_cast<std::uint32_t>(lo);
            const auto [slotLo, slotHi] = state.slotRange(slot);
            runChainStageForSlot<0>(slot, slotLo, slotHi, state, stagePack,
                                    tok);
          };
          dispatchSlotBody(bodyWrapper);
        }
      } else {
        auto bodyWrapper = [&state, &stagePack, &tok,
                            this](std::size_t lo, std::size_t /*hi*/) {
          const auto slot = static_cast<std::uint32_t>(lo);
          const auto [slotLo, slotHi] = state.slotRange(slot);
          runChainStageForSlot<0>(slot, slotLo, slotHi, state, stagePack, tok);
        };
        dispatchSlotBody(bodyWrapper);
      }

      // `dispatchOneStatic` rethrows exceptions captured by
      // `desc.firstException`; the body wrapper routes chain-thrown exceptions
      // through `state.firstException` instead.
      rethrowIfCaptured(state);
    }
  }

  /// Rethrow any exception captured into |state|'s `firstException` slot.
  /// The slot is reset to `nullptr` after the rethrow so the caller's stack
  /// frame can unwind without leaking the heap-allocated exception_ptr.
  template <class State>
  [[gnu::always_inline]] static void rethrowIfCaptured(State &state) {
    auto *eptr = state.firstException.load(std::memory_order_acquire);
    if (eptr != nullptr) [[unlikely]] {
      const std::exception_ptr captured = *eptr;
      delete eptr;
      state.firstException.store(nullptr, std::memory_order_release);
      std::rethrow_exception(captured);
    }
  }

  /// Parallel Blelloch scan engine: publish one job, run two passes, return
  /// the inclusive total. The pool-owned `done` slot block is reused (each
  /// dispatch reserves a disjoint epoch interval), and a per-chunk `partials`
  /// vector with cache-line padding keeps adjacent chunks' writes off the
  /// same line. The wrapper body invoked with `(slot, slot+1)` runs the
  /// per-slot two-pass protocol: slot 0 (the producer) drives the sequential
  /// reduce between passes; non-zero slots acquire-spin on
  /// `prefixesPublished` between Pass 1 and Pass 2. The dispatch publishes a
  /// static-uniform job with `blockCount = participants` and `chunk = 1` so
  /// every slot receives exactly one body invocation. Slot 0's body runs
  /// Pass 1, performs the sequential reduce, publishes prefixes, runs
  /// Pass 2, then joins on every other slot's `done >= 2`. Any captured
  /// exception is rethrown after dispatch returns.
  ///
  /// The per-chunk slot type is cache-line aligned so adjacent partials live
  /// on separate lines (false-sharing avoidance over byte-tight packing).
  template <class HintsT, class T, class BodyFn, class PrefixFn>
  T runScanParallel(std::size_t n, T identity, BodyFn &&body, PrefixFn &&prefix,
                    CancellationToken tok, std::size_t participants) {
    // NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
    struct alignas(kCacheLine) ScanSlot {
      T value;
    };
    std::vector<ScanSlot> partials;
    partials.resize(participants);

    // Hierarchical-scan setup. The slot-to-cluster mapping, contiguity
    // check, and `useHierarchical` flag were precomputed once at pool ctor
    // by `initScanScratch` from pool-immutable inputs. Reading the cached
    // fields takes the per-call topology resolution off the hot path.
    const bool useHierarchical = m_scanUseHierarchical;
    const std::uint32_t numClusters = m_scanNumClusters;
    // Cluster scratch. Numbers are tiny (`numClusters <= participants` and
    // typically <= 8) so false sharing across cluster entries is not worth
    // padding away: the cluster-total line bounce IS the cross-cluster
    // reduce traffic, so a few line transfers per call are intentional.
    std::vector<T> clusterTotals;
    std::vector<T> clusterPrefixes;
    // `std::atomic` is non-copyable and non-movable, so
    // `std::vector<std::atomic<T>>` will not compile and `std::array` needs a
    // compile-time size. Hence the unique_ptr-owned C array.
    // NOLINTNEXTLINE(modernize-avoid-c-arrays)
    std::unique_ptr<std::atomic<std::uint64_t>[]> clusterDoneStamps;
    if (useHierarchical) {
      clusterTotals.assign(numClusters, identity);
      clusterPrefixes.assign(numClusters, identity);
      clusterDoneStamps =
          // NOLINTNEXTLINE(modernize-avoid-c-arrays)
          std::make_unique<std::atomic<std::uint64_t>[]>(numClusters);
      for (std::uint32_t k = 0; k < numClusters; ++k) {
        clusterDoneStamps[k].store(0U, std::memory_order_relaxed);
      }
    }

    // Acquire the dispatch gate BEFORE touching pool-owned scratch
    // (`m_chainDoneSlots`, `m_chainEpochBase`). Concurrent producers would
    // otherwise race on the epoch advance and on the residual stamps left by a
    // peer's previous dispatch.
    const DispatchLease lease(*this, Priority::Throughput);

    // Reserve a fresh epoch interval: scan stamps `done = epochBase + 1` after
    // Pass 1 and `done = epochBase + 2` after Pass 2, so the next dispatch
    // starts at `epochBase + 3`. The wrap guard zero-resets and rebases when
    // the counter approaches `UINT64_MAX`.
    detail::ChainDoneSlot *const slotsBase = m_chainDoneSlots.get();
    if (m_chainEpochBase > UINT64_MAX - 1024U) [[unlikely]] {
      for (std::size_t i = 0; i < participants; ++i) {
        slotsBase[i].done.store(0, std::memory_order_relaxed);
      }
      m_chainEpochBase = 0;
    }
    const std::uint64_t epochBase = m_chainEpochBase;
    m_chainEpochBase += 3U;

    detail::ScanState<T> state;
    state.participants = static_cast<std::uint32_t>(participants);
    state.n = n;
    state.doneSlots = slotsBase;
    state.epochBase = epochBase;
    if (useHierarchical) {
      state.clusterIdOfSlot = m_scanClusterIdOfSlot.data();
      state.numClusters = numClusters;
      state.clusterFirstSlot = m_scanClusterFirstSlot.data();
      state.clusterSlotCount = m_scanClusterSlotCount.data();
      state.clusterTotals = clusterTotals.data();
      state.clusterPrefixes = clusterPrefixes.data();
    }

    // CCD-aware asymmetric chunk partitioning. The slot-to-CCD layout, the
    // producer-CCD slot count, and the asymmetric numerator (out of 16)
    // were precomputed once at pool ctor by `initScanScratch` from the
    // pool-immutable inputs (`m_ccdOfSlot`, the coherence probe's
    // `maxCrossOverIntraRatio`). The hot path reads the cached fields
    // directly; this saves the per-call O(participants) sweep over
    // `m_ccdOfSlot` plus the floating-point bias derivation.
    if (m_scanSlotsOnProducerCcd > 0U &&
        m_scanSlotsOnProducerCcd < participants) {
      state.ccdOfSlot = m_ccdOfSlot.data();
      state.producerCcd = m_scanProducerCcd;
      state.slotsOnProducerCcd = m_scanSlotsOnProducerCcd;
      state.asymmetricNum = m_scanAsymmetricNum;
    }

    CancellationToken scanTok = std::move(tok);

    // The inclusive accumulator at the right edge of the scan is computed by
    // slot 0's sequential reduce. Slot 0 IS the producer thread, so writing to
    // a stack-local captured by reference is race-free: no other thread reads
    // `inclusiveTotal` until `dispatchOne` joins.
    T inclusiveTotal{};

    std::atomic<std::uint64_t> *const clusterStampsPtr =
        clusterDoneStamps.get();
    auto bodyWrapper = [&state, &body, &prefix, &identity, &scanTok, &partials,
                        &inclusiveTotal, useHierarchical,
                        clusterStampsPtr](std::size_t lo, std::size_t /*hi*/) {
      const auto slot = static_cast<std::uint32_t>(lo);
      const auto [slotLo, slotHi] = state.slotRange(slot);
      const std::uint64_t pass1Stamp = state.epochBase + 1U;
      const std::uint64_t pass2Stamp = state.epochBase + 2U;
      // Worker-side failure capture: stamp the first exception slot,
      // then arm the cancellation flag so peers short-circuit at the
      // next pass-2 gate. Both stores are release; readers acquire-load
      // matching atomics elsewhere in the body wrapper.
      auto recordFailure = [&state] {
        captureFirstException(state);
        state.scanCancelled.store(1U, std::memory_order_release);
      };

      // ----- Pass 1: compute this chunk's partial sum with `initial =
      // identity`. Output buffer is `nullptr` to signal "Pass 1,
      // do not write" so the body can branch on it when it makes the
      // partial-sum computation cheaper than a full scan write.
      try {
        T partial = body(static_cast<std::size_t>(slot), slotLo, slotHi,
                         identity, static_cast<T *>(nullptr));
        partials[slot].value = std::move(partial);
      } catch (...) {
        recordFailure();
        // Stamp the final epoch so the producer's exit join completes
        // regardless of pass.
        state.doneSlot(slot).done.store(pass2Stamp, std::memory_order_release);
        if (slot == 0U) {
          // Producer must still wait for every other slot before returning to
          // keep dispatchOne's join contract intact. Publish prefixes so any
          // peer spinning on `prefixesPublished` exits its loop.
          state.prefixesPublished.store(1U, std::memory_order_release);
          for (std::uint32_t s = 1; s < state.participants; ++s) {
            while (state.doneSlot(s).done.load(std::memory_order_acquire) <
                   pass2Stamp) {
              detail::cpuRelax();
            }
          }
        }
        return;
      }
      // Release-store: pairs with the producer's acquire-load before computing
      // prefixes.
      state.doneSlot(slot).done.store(pass1Stamp, std::memory_order_release);

      // Hierarchical per-cluster reduce path. Each cluster's leader does
      // its own local seq-reduce over its cluster's slots; cluster
      // leaders then exchange one cluster-total cache line each across
      // the fabric, bounding inter-cluster line transit to one transfer
      // per cluster pair. The producer combines the per-cluster totals
      // into per-cluster exclusive prefixes and publishes them; each
      // cluster leader folds its prefix into its slots' partials before
      // Pass 2 reads them. Triggered when the pool's coherence probe
      // found two or more contiguous clusters and the producer is in
      // cluster 0.
      if (useHierarchical) {
        const std::uint32_t cluster = state.clusterIdOfSlot[slot];
        const std::uint32_t firstSlot = state.clusterFirstSlot[cluster];
        const std::uint32_t slotCount = state.clusterSlotCount[cluster];
        const bool isLeader = (slot == firstSlot);

        if (isLeader) {
          // Wait for cluster's slots' Pass 1.
          for (std::uint32_t s = firstSlot; s < firstSlot + slotCount; ++s) {
            if (s == slot) {
              continue;
            }
            while (state.doneSlot(s).done.load(std::memory_order_acquire) <
                   pass1Stamp) {
              detail::cpuRelax();
            }
          }
          // Local seq-reduce: write per-slot intra-cluster exclusive
          // prefixes back into `partials`, accumulate into `localAcc`.
          T localAcc = identity;
          try {
            for (std::uint32_t s = firstSlot; s < firstSlot + slotCount; ++s) {
              T t = std::move(partials[s].value);
              partials[s].value = localAcc;
              localAcc = prefix(std::move(localAcc), std::move(t));
            }
          } catch (...) {
            recordFailure();
          }
          // Publish cluster total. The store on `clusterTotals[cluster]`
          // happens-before the release on `clusterStampsPtr[cluster]`, so
          // the producer's acquire on the same atomic synchronises the
          // cluster total read.
          state.clusterTotals[cluster] = std::move(localAcc);
          clusterStampsPtr[cluster].store(1U, std::memory_order_release);

          if (slot == 0U) {
            // Producer (cluster 0 leader) waits for every other cluster
            // total, then computes the cross-cluster exclusive prefixes.
            for (std::uint32_t k = 1; k < state.numClusters; ++k) {
              while (clusterStampsPtr[k].load(std::memory_order_acquire) < 1U) {
                detail::cpuRelax();
              }
            }
            T globalAcc = identity;
            try {
              for (std::uint32_t k = 0; k < state.numClusters; ++k) {
                state.clusterPrefixes[k] = globalAcc;
                globalAcc =
                    prefix(std::move(globalAcc), state.clusterTotals[k]);
              }
            } catch (...) {
              recordFailure();
            }
            inclusiveTotal = std::move(globalAcc);
            // Publish prefixes globally so non-producer leaders proceed.
            state.prefixesPublished.store(1U, std::memory_order_release);
          } else {
            // Non-producer cluster leader: wait for global prefix
            // publication (the producer's release-store).
            while (state.prefixesPublished.load(std::memory_order_acquire) ==
                   0U) {
              detail::cpuRelax();
            }
          }

          // Cluster leader (every leader, including producer) combines
          // its cluster's exclusive prefix with each slot's intra-cluster
          // exclusive prefix to produce the per-slot global exclusive
          // prefix that Pass 2's body will read.
          const T cPrefix = state.clusterPrefixes[cluster];
          try {
            for (std::uint32_t s = firstSlot; s < firstSlot + slotCount; ++s) {
              partials[s].value = prefix(cPrefix, std::move(partials[s].value));
            }
          } catch (...) {
            recordFailure();
          }
          // Final per-cluster signal: cluster's slots can read their
          // partials and run Pass 2.
          clusterStampsPtr[cluster].store(2U, std::memory_order_release);
        } else {
          // Non-leader slot in this cluster: wait for the cluster's
          // leader to combine the global prefix into our `partials[slot]`.
          while (clusterStampsPtr[cluster].load(std::memory_order_acquire) <
                 2U) {
            detail::cpuRelax();
          }
        }

        // ----- Pass 2: every slot reads `partials[slot]` (now the
        // global exclusive prefix for this slot's chunk) and runs the
        // body with that prefix as `initial`.
        const bool cancelObserved =
            state.scanCancelled.load(std::memory_order_acquire) != 0U ||
            scanTok.stop_requested();
        if (!cancelObserved) {
          try {
            T myPrefix = partials[slot].value;
            (void)body(static_cast<std::size_t>(slot), slotLo, slotHi,
                       std::move(myPrefix), static_cast<T *>(nullptr));
          } catch (...) {
            recordFailure();
          }
        } else if (scanTok.stop_requested()) {
          state.scanCancelled.store(1U, std::memory_order_release);
        }
        state.doneSlot(slot).done.store(pass2Stamp, std::memory_order_release);
        return;
      }

      if (slot == 0U) {
        // Producer waits for every chunk's Pass-1 partial. A peer that threw
        // stamps the Pass-2 epoch as part of its catch; that satisfies `done >=
        // pass1Stamp` too, so the reduce proceeds against the partial slot's
        // last value (which the post-join rethrow then suppresses).
        for (std::uint32_t s = 1; s < state.participants; ++s) {
          while (state.doneSlot(s).done.load(std::memory_order_acquire) <
                 pass1Stamp) {
            detail::cpuRelax();
          }
        }

        // Skip the seq-reduce when a peer's Pass-1 captured an EXCEPTION: the
        // partials of the failing slot were never written, so feeding them to
        // the user's `prefix` would invoke it on a default-constructed (or
        // moved-from) T. Cancellation alone is NOT sufficient to skip -- Pass 1
        // ran on every slot before any cancellation gate fires (the gates are
        // in Pass 2), so the partials are valid and the producer's
        // `inclusiveTotal` contract still holds. Mark the prefix flag published
        // either way so any worker still spinning on it can short-circuit.
        const bool pass1Failed =
            state.firstException.load(std::memory_order_acquire) != nullptr;
        T acc = identity;
        if (!pass1Failed) {
          // ----- Sequential reduce on the producer: build exclusive prefixes
          // in chunk-id order, stash them back into `partials[c]` so each
          // slot's Pass-2 invocation reads its own seed. A throwing user
          // `prefix` MUST publish the prefixes flag and stamp Pass-2 done
          // before re-raising, otherwise background workers stuck on
          // `prefixesPublished` deadlock the producer's outer join.
          try {
            for (std::uint32_t c = 0; c < state.participants; ++c) {
              T t = std::move(partials[c].value);
              partials[c].value = acc;
              acc = prefix(std::move(acc), std::move(t));
            }
          } catch (...) {
            recordFailure();
          }
        }
        // Publish the prefixes via release: workers' acquire-load on
        // `prefixesPublished` synchronizes with the writes we just performed to
        // `partials[*]` (when the loop ran) or releases peers stuck on the flag
        // (when we short-circuited).
        state.prefixesPublished.store(1U, std::memory_order_release);

        // ----- Pass 2: run slot 0's body with `initial = partials[0]`. Skip
        // the body entirely if we already captured an exception or saw a stop
        // request, but still stamp our `done` and join on the others.
        const bool cancelObserved =
            state.scanCancelled.load(std::memory_order_acquire) != 0U ||
            scanTok.stop_requested();
        if (!cancelObserved) {
          try {
            T myPrefix = partials[0U].value;
            (void)body(std::size_t{0}, slotLo, slotHi, std::move(myPrefix),
                       static_cast<T *>(nullptr));
          } catch (...) {
            recordFailure();
          }
        } else if (scanTok.stop_requested()) {
          state.scanCancelled.store(1U, std::memory_order_release);
        }
        state.doneSlot(0).done.store(pass2Stamp, std::memory_order_release);

        // Stash the inclusive accumulator now, before returning. It is
        // captured by reference in this lambda; the caller reads it after
        // `dispatchOneStaticLocked` returns, which already waits for every
        // worker's mailbox to stamp the dispatch's done sentinel. That
        // mailbox-join provides the per-slot Pass-2 rendezvous, so the
        // producer does not need a second scan over `done` slots here.
        inclusiveTotal = std::move(acc);
      } else {
        // Background worker: spin on `prefixesPublished` until the producer
        // finishes the reduce.
        while (true) {
          if (state.prefixesPublished.load(std::memory_order_acquire) != 0U) {
            break;
          }
          detail::cpuRelax();
        }
        if (state.scanCancelled.load(std::memory_order_acquire) != 0U ||
            scanTok.stop_requested()) {
          if (scanTok.stop_requested()) {
            state.scanCancelled.store(1U, std::memory_order_release);
          }
          state.doneSlot(slot).done.store(pass2Stamp,
                                          std::memory_order_release);
          return;
        }
        // Pass 2: invoke the body with `initial = partials[slot]`. Output
        // buffer is `nullptr` matching Pass 1; the body branches on `initial !=
        // identity` to know it should write.
        try {
          T myPrefix = partials[slot].value;
          (void)body(static_cast<std::size_t>(slot), slotLo, slotHi,
                     std::move(myPrefix), static_cast<T *>(nullptr));
        } catch (...) {
          recordFailure();
          state.doneSlot(slot).done.store(pass2Stamp,
                                          std::memory_order_release);
          return;
        }
        state.doneSlot(slot).done.store(pass2Stamp, std::memory_order_release);
      }
    };

    const FunctionRef<void(std::size_t, std::size_t)> bodyRef{bodyWrapper};

    detail::JobDescriptor desc;
    desc.first = 0;
    desc.last = participants;
    desc.participants = static_cast<std::uint32_t>(participants);
    desc.balance = Balance::StaticUniform;
    desc.priority = Priority::Throughput;
    desc.body = bodyRef;
    // Leave `desc.token` default-constructed: the descriptor's stop-check would
    // gate the static-partition admit from running every slot's body, and slot
    // 0 IS the participant that joins on the others, so a stopped token would
    // deadlock the producer's join. The wrapper observes `scanTok` at pass
    // boundaries instead.
    desc.chunk = 1;
    desc.blockCount = participants;

    dispatchOneStaticLocked<Balance::StaticUniform>(desc);

    // `dispatchOneStaticLocked` rethrows exceptions captured by
    // `desc.firstException`; the wrapper routes scan-thrown exceptions through
    // `state.firstException` instead.
    rethrowIfCaptured(state);

    return inclusiveTotal;
  }

  /// Decoupled-lookback inclusive prefix-sum engine. Single-pass: each
  /// tile reads its slice of `in` once and writes its slice of `out`
  /// once, so the bandwidth floor is 2n bytes (read input + write
  /// output) instead of two-pass Blelloch's 3n.
  ///
  /// Per-tile state machine (Merrill-Garland 2016): every tile cycles
  /// through `Initialized -> AggregateAvailable -> PrefixAvailable`.
  /// Tile T's worker computes its local total, publishes it as
  /// `aggregate`, walks predecessors backward summing aggregates until
  /// it finds a predecessor in `PrefixAvailable`, computes its own
  /// prefix, publishes `prefix`, then runs the inclusive scan into
  /// `out[T_lo..T_hi]` with `prefix` as the seed.
  ///
  /// Cross-CCD adaptation: each tile is owned by a single worker (slot)
  /// and per-tile state lines live with the owner; the lookback chain
  /// sweeps backward across tiles, so workers on cluster N reading a
  /// predecessor tile owned by cluster M pay the cross-cluster
  /// coherence cost. With tiles sized to the runtime-probed L2/2 the
  /// chain typically terminates within a couple of hops because
  /// immediate predecessors finish their aggregate before the
  /// successor's body returns.
  ///
  /// Output prefetch: each tile issues `PREFETCHW` over its own
  /// `out[T_lo..T_hi]` slice immediately after publishing its
  /// aggregate, so the cross-cluster RFO traffic for the writes runs
  /// concurrently with the lookback walk and the local scan, hiding
  /// the inter-die fabric round-trip behind per-tile compute.
  ///
  /// Tile size: `tileBytes = max(64 KiB, l2KibPerCore * 1024 / 2)` --
  /// half the runtime-probed L2 leaves room for both the input read
  /// and the output write of a tile to be L2-resident. Falls back to
  /// 256 KiB when sysfs is absent.
  ///
  /// Returns the inclusive total at the right edge.
  template <class HintsT, class T, class PrefixFn>
  T runInclusiveScanLookback(std::span<const T> in, std::span<T> out,
                             T identity, PrefixFn &&prefix,
                             const CancellationToken &tok) {
    if (in.size() != out.size()) {
      throw std::invalid_argument(
          "inclusiveScan: in.size() must equal out.size()");
    }
    const std::size_t n = in.size();
    if (n == 0U) {
      return identity;
    }
    const std::size_t participants = m_control.participants;

    // Choose tile bytes from the runtime-probed L2-per-core. The tile
    // size balances: (a) tile-local working set should fit in L2 so
    // Pass-1's chunk-local scan stays cache-resident through the
    // lookback wait, (b) tile count >= participants so every worker
    // has work and the lookback chain pipelines (more tiles than
    // workers means a slow tile doesn't stall the whole chain), (c)
    // tile compute time should be a few microseconds so the lookback
    // chain hops overlap with adjacent tiles' Pass-1 work.
    //
    // Heuristic: tile_bytes = min(l2_per_core / 4, n / participants).
    // The L2/4 cap leaves headroom for d.in + d.out + a small cushion
    // of locked stack frames in cache; the n/participants cap ensures
    // there are at least `participants` tiles. Hardware-agnostic:
    // works on any CPU that exposes index2/size in sysfs; falls back
    // to 64 KiB when sysfs is absent.
    // Tile sizing -- balance two competing constraints:
    //   (a) Chain parallelism: in a single coherence cluster, the
    //       lookback chain serializes the workers (tile T waits on
    //       T-1's prefix). If `numTiles > participants`, some workers
    //       grab a second tile and must then wait for the chain to
    //       progress past the first wave, defeating parallelism. So
    //       prefer `numTiles ~= participants_per_cluster` so each
    //       cluster's chain length matches its worker count.
    //   (b) L2 residency: Pass-1 writes d.out chunk-local-scan (one
    //       cache pass) and Pass-2 in-place adds the global prefix
    //       (second pass over the same lines). Each worker's per-tile
    //       working set is `2 * tileBytes` (the tile's d.out lines
    //       are read+written twice). If this exceeds L2, Pass-2's
    //       reads spill to L3, costing bandwidth.
    //
    // Resolve: pick `tileBytes = clamp(perParticipantBytes, kMinTileBytes,
    // l2KibPerCore*1024)`. The L2 cap ensures the tile's working set is
    // L2-resident; the per-participant ratio ensures the chain length
    // equals participant count when `n` is small enough; the
    // `kMinTileBytes` floor avoids pathological 4-tile dispatches with
    // huge per-tile overhead.
    constexpr std::size_t kFallbackL2Bytes = std::size_t{512U} * 1024U;
    constexpr std::size_t kMinTileBytes = std::size_t{64U} * 1024U;
    const std::size_t l2Bytes =
        m_topology.l2KibPerCore == 0U
            ? kFallbackL2Bytes
            : static_cast<std::size_t>(m_topology.l2KibPerCore) * 1024U;
    const std::size_t nBytes = n * sizeof(T);
    const std::size_t perParticipantBytes =
        (nBytes + participants - 1U) / participants;
    std::size_t tileBytes = std::min(perParticipantBytes, l2Bytes);
    tileBytes = std::max(tileBytes, kMinTileBytes);
    const std::size_t tileElems = tileBytes / sizeof(T);
    if (tileElems == 0U) {
      // T larger than the tile budget; fall through to a serial scan.
      T running = identity;
      for (std::size_t i = 0; i < n; ++i) {
        running = prefix(std::move(running), in[i]);
        out[i] = running;
      }
      return running;
    }
    const std::size_t numTiles = (n + tileElems - 1U) / tileElems;
    if (participants <= 1U || numTiles == 1U || shouldFallThroughCrossArena()) {
      // Serial path: a single thread does the whole scan.
      if (tok.stop_requested()) {
        return identity;
      }
      T running = identity;
      for (std::size_t i = 0; i < n; ++i) {
        running = prefix(std::move(running), in[i]);
        out[i] = running;
      }
      return running;
    }

    using Tile = detail::LookbackTile<T>;
    std::vector<Tile> tiles(numTiles);
    std::atomic<std::exception_ptr *> firstException{nullptr};

    // Per-cluster tile claim counters. Each cluster's workers atomically
    // claim tiles from their cluster's contiguous tile range. The
    // partition is `tilesPerCluster_k = round(numTiles * slotsInCluster_k
    // / participants)`, with the residual tile (if `numTiles %
    // participants != 0`) assigned to the last cluster. The lookback
    // chain runs intra-cluster except for the single hop at the
    // cluster-0 / cluster-1 boundary, so cross-cluster cache-line
    // transit on the chain is bounded to one line per scan.
    const bool multiCluster = m_scanUseHierarchical && m_scanNumClusters >= 2U;
    const std::uint32_t numClustersUsed = multiCluster ? m_scanNumClusters : 1U;
    std::vector<std::uint64_t> clusterFirstTile(numClustersUsed, 0U);
    std::vector<std::uint64_t> clusterLastTile(numClustersUsed, numTiles);
    if (multiCluster) {
      std::uint64_t cursor = 0;
      for (std::uint32_t k = 0; k < numClustersUsed; ++k) {
        clusterFirstTile[k] = cursor;
        const std::uint64_t kSlots = m_scanClusterSlotCount[k];
        std::uint64_t kTiles =
            (numTiles * kSlots + participants - 1ULL) / participants;
        if (k + 1U == numClustersUsed || cursor + kTiles > numTiles) {
          kTiles = numTiles - cursor;
        }
        cursor += kTiles;
        clusterLastTile[k] = cursor;
      }
    }
    std::vector<std::atomic<std::uint64_t>> clusterNextTile(numClustersUsed);
    for (std::uint32_t k = 0; k < numClustersUsed; ++k) {
      clusterNextTile[k].store(clusterFirstTile[k], std::memory_order_relaxed);
    }

    auto claimAndRunTile = [&](std::uint32_t slot) noexcept(noexcept(
                               prefix(std::declval<T>(), std::declval<T>()))) {
      const std::uint32_t cluster =
          multiCluster ? m_scanClusterIdOfSlot[slot] : 0U;
      const std::uint64_t myLast = clusterLastTile[cluster];
      while (true) {
        const std::uint64_t tIdx =
            clusterNextTile[cluster].fetch_add(1U, std::memory_order_acq_rel);
        if (tIdx >= myLast) {
          return;
        }
        if (firstException.load(std::memory_order_acquire) != nullptr) {
          // A peer threw; mark our tile's prefix as identity and
          // publish so successors don't deadlock on lookback.
          tiles[tIdx].prefix = identity;
          tiles[tIdx].flag.store(
              static_cast<std::uint64_t>(Tile::Flag::PrefixAvailable),
              std::memory_order_release);
          continue;
        }
        try {
          const std::size_t lo = tIdx * tileElems;
          const std::size_t hi = (lo + tileElems > n) ? n : (lo + tileElems);

          // Pass 1: read `in[lo..hi]` once, compute the chunk-local
          // inclusive scan with prefix=identity, write directly to
          // `out[lo..hi]`. The chunk total (= scan's last element)
          // is also published as the tile's `aggregate`. By
          // reusing `out` as the scratch buffer for the
          // local-scan output we avoid a separate temp buffer's
          // allocation and keep the data path L2-resident: Pass 2
          // immediately re-reads the same lines from L2 and
          // overwrites them with the global-prefix-corrected
          // values, so total memory traffic through the cache
          // hierarchy is `n bytes read in + n bytes written out`,
          // matching the 2n bandwidth floor.
          T localTotal = identity;
          for (std::size_t i = lo; i < hi; ++i) {
            localTotal = prefix(std::move(localTotal), in[i]);
            out[i] = localTotal;
          }

          // Publish aggregate. Release-store on the flag pairs with
          // a successor tile's acquire-load in `lookbackWalk`.
          tiles[tIdx].aggregate = localTotal;
          tiles[tIdx].flag.store(
              static_cast<std::uint64_t>(Tile::Flag::AggregateAvailable),
              std::memory_order_release);

          // Compute exclusive prefix for this tile.
          T myPrefix;
          if (tIdx == 0U) {
            myPrefix = identity;
          } else {
            myPrefix = detail::lookbackWalk<T>(tiles.data(),
                                               static_cast<std::uint32_t>(tIdx),
                                               identity, prefix);
          }
          tiles[tIdx].prefix = myPrefix;
          tiles[tIdx].flag.store(
              static_cast<std::uint64_t>(Tile::Flag::PrefixAvailable),
              std::memory_order_release);

          // Pass 2: in-place adjust the chunk-local inclusive scan
          // we wrote in Pass 1 by adding the global prefix. The
          // lines are warm in L1/L2 from Pass 1's write so this is
          // an L2-bandwidth-bound read+write pass with no
          // additional input read from `in`.
          if (tIdx > 0U) {
            for (std::size_t i = lo; i < hi; ++i) {
              out[i] = prefix(myPrefix, std::move(out[i]));
            }
          }
        } catch (...) {
          auto *eptr = new std::exception_ptr(std::current_exception());
          // NOLINTNEXTLINE(misc-const-correctness)
          std::exception_ptr *expected = nullptr;
          if (!firstException.compare_exchange_strong(
                  expected, eptr, std::memory_order_release,
                  std::memory_order_acquire)) {
            delete eptr;
          }
          tiles[tIdx].prefix = identity;
          tiles[tIdx].flag.store(
              static_cast<std::uint64_t>(Tile::Flag::PrefixAvailable),
              std::memory_order_release);
        }
      }
    };

    // Dispatch: every slot pulls tiles from the shared atomic counter.
    // Producer participates as slot 0. The dispatch's existing
    // mailbox-join handles the rendezvous after every tile is
    // consumed.
    auto bodyWrapper = [&claimAndRunTile](std::size_t slot,
                                          std::size_t /*hi*/) {
      claimAndRunTile(static_cast<std::uint32_t>(slot));
    };
    const FunctionRef<void(std::size_t, std::size_t)> bodyRef{bodyWrapper};

    const DispatchLease lease(*this, Priority::Throughput);
    detail::JobDescriptor desc;
    desc.first = 0;
    desc.last = participants;
    desc.participants = static_cast<std::uint32_t>(participants);
    desc.balance = Balance::StaticUniform;
    desc.priority = Priority::Throughput;
    desc.body = bodyRef;
    desc.chunk = 1;
    desc.blockCount = participants;
    dispatchOneStaticLocked<Balance::StaticUniform>(desc);

    auto *eptr = firstException.load(std::memory_order_acquire);
    if (eptr != nullptr) [[unlikely]] {
      const std::exception_ptr captured = *eptr;
      delete eptr;
      firstException.store(nullptr, std::memory_order_release);
      std::rethrow_exception(captured);
    }
    if (tok.stop_requested()) {
      return identity;
    }
    // Inclusive total at the right edge: combine the last tile's
    // exclusive prefix with its own aggregate.
    return prefix(tiles[numTiles - 1U].prefix, tiles[numTiles - 1U].aggregate);
  }

  /// Resolve the steal-policy field of |HintsT|, defaulting to
  /// `StealPolicy::ClusterLocal` when the hint type does not declare one.
  /// The detection idiom keeps the `forkJoin` entry compatible with hint
  /// presets that omit the field. The fallback matches `HintsDefaults` so
  /// an absent field behaves identically to the default-constructed hint.
  template <class HintsT>
  static constexpr StealPolicy stealPolicyFromHints() noexcept {
    if constexpr (requires { HintsT::stealPolicy; }) {
      return HintsT::stealPolicy;
    } else {
      return StealPolicy::ClusterLocal;
    }
  }

  /// Wrap each task closure in a `FunctionRef<void()>` and store it in a
  /// stack-resident `detail::Task` descriptor. Used by both `forkJoin` entry
  /// overloads.
  template <class ClosureTuple, std::size_t N, std::size_t... Is>
  static void fillTaskBodies(ClosureTuple &closures,
                             std::array<detail::Task, N> &tasks,
                             std::index_sequence<Is...> /*indices*/) noexcept {
    ((tasks[Is].body = FunctionRef<void()>(std::get<Is>(closures))), ...);
  }

  /// Run |nTasks| fork-join tasks from the producer thread, dispatching to
  /// background workers via the standard generation-publish protocol. Each
  /// task is pushed onto slot 0's deque, the descriptor is published so
  /// background workers wake and enter the drain loop, the producer runs its
  /// own drain loop as slot 0, and the call joins when every worker's
  /// `doneEpoch` reaches the new generation. The first captured exception
  /// (if any) is rethrown after the join.
  template <bool HasToken = true>
  void runForkJoinOuter(detail::Task *tasks, std::size_t nTasks,
                        CancellationToken tok, StealPolicy stealPolicy) {
    const std::size_t participants = m_control.participants;

    // Construct shared state on the producer's stack.
    detail::ForkJoinState state;
    state.participants = static_cast<std::uint32_t>(participants);
    state.ccdOfSlot = m_ccdOfSlot.empty() ? nullptr : m_ccdOfSlot.data();
    state.preferSameCcd = (stealPolicy == StealPolicy::ClusterLocal);
    state.token = std::move(tok);
    state.pendingTasks.store(static_cast<std::int64_t>(nTasks),
                             std::memory_order_relaxed);

    // Single-participant fast path: run every task inline on the producer; no
    // fan-out, no deque traffic, no generation publish. Cheaper than the
    // dispatch round-trip for the canonical n_init = 1 case. The cross-arena
    // guard takes the same path: a worker on a different arena must not block
    // on this arena's queue, so it runs the tasks itself.
    if (participants <= 1 || shouldFallThroughCrossArena()) {
      runForkJoinInline(tasks, nTasks, state);
      rethrowIfCaptured(state);
      return;
    }

    // Acquire the dispatch gate BEFORE writing per-task state and pushing onto
    // the worker deques: Chase-Lev push is single-owner and concurrent producer
    // pushes corrupt the deque, and the descriptor pointer races with workers
    // servicing a peer producer's forkJoin. The lease serializes producers
    // around `m_workerDeques` mutation.
    const DispatchLease lease(*this, Priority::Throughput);

    // Route every root task onto slot 0's (the producer's) deque. The earlier
    // round-robin fan-out across all `participants` deques pessimized fib-class
    // workloads: with only two root tasks, round-robin splits work across a
    // small subset of deques while the first recursive levels are still serial
    // on their owners. Pushing onto slot 0 instead lets stealers converge on a
    // single hot deque on the first probe; recursive forkJoin from inside a
    // worker still goes onto the calling worker's own deque (see
    // runForkJoinNested), so the steal radius shrinks as recursion
    // deepens. This matches TBB's "submit to caller's task pool" shape on the
    // outer call.
    //
    // Pre-grow once when |nTasks| would force multiple geometric grows on
    // the slot-0 deque (initial capacity is 64). Folds up to log2(nTasks)
    // allocations into one for large fan-outs.
    if (nTasks > 64U) {
      m_workerDeques[0]->reserveOwner(nTasks);
    }
    for (std::size_t i = 0; i < nTasks; ++i) {
      tasks[i].state = &state;
      m_workerDeques[0]->push(&tasks[i]);
    }
    (void)participants;

    // Body wrapper that workers run from `runActiveJob`. The job descriptor's
    // `lo` is the worker slot; we ignore `hi` (= lo+1, by construction). The
    // wrapper drains the worker's own deque, then steals from victims, until
    // every task in this state has retired.
    detail::ForkJoinState *statePtr = &state;
    auto bodyWrapper = [this, statePtr](std::size_t lo, std::size_t /*hi*/) {
      this->template runForkJoinDrain<HasToken>(static_cast<std::uint32_t>(lo),
                                                *statePtr);
    };
    const FunctionRef<void(std::size_t, std::size_t)> bodyRef{bodyWrapper};

    detail::JobDescriptor desc;
    desc.first = 0;
    desc.last = participants;
    desc.participants = static_cast<std::uint32_t>(participants);
    desc.balance = Balance::StaticUniform;
    desc.priority = Priority::Throughput;
    desc.body = bodyRef;
    // The descriptor's own `token` is left default-constructed: the
    // cancellation logic flows through `state.token`, observed by
    // `runForkJoinDrain` at task boundaries. A stopped descriptor token would
    // gate the static-partition admit and prevent slot 0's drain from running,
    // which would deadlock the join.
    desc.chunk = 1;
    desc.blockCount = participants;

    // The lease covers both the deque pushes above and the
    // publish/participate/join below. `dispatchOneStaticLocked` installs slot-0
    // TLS context across the producer's inline body so a recursive `forkJoin`
    // call from inside a task body can take the in-place nested-recursion path;
    // nothing additional is needed here.
    dispatchOneStaticLocked<Balance::StaticUniform>(desc);

    rethrowIfCaptured(state);
  }

  /// Recursive `forkJoin` fast path that calls the last child inline by
  /// static type. Pushes the first |deferredCount| `Task` descriptors onto
  /// the calling worker's deque (so peers can steal via the standard victim
  /// probe) and invokes |inlineLast| directly, bypassing the `FunctionRef`
  /// thunk and the per-task `runOneTask` wrapper. Cancellation gating,
  /// exception capture, and the `pendingTasks` rendezvous match
  /// `runOneTask`'s contract; only the indirect call disappears.
  ///
  /// Invariant: `pendingTasks` is initialized to |deferredCount| (the
  /// deque-pushed children only). The inline child runs on the producer
  /// thread and does not contribute to the join counter, since it always
  /// completes before drain begins. For `participants <= 1` the path falls
  /// back to the single-participant inline executor after rebuilding a full
  /// Task array.
  template <bool HasToken = true, class InlineFn>
  void runForkJoinTypedTailNested(
      detail::Task *deferred, std::size_t deferredCount, InlineFn &&inlineLast,
      CancellationToken tok, // NOLINT(performance-unnecessary-value-param)
      StealPolicy stealPolicy, std::uint32_t callerSlot) {
    const std::size_t participants = m_control.participants;

    detail::ForkJoinState state;
    state.participants = static_cast<std::uint32_t>(participants);
    state.ccdOfSlot = m_ccdOfSlot.empty() ? nullptr : m_ccdOfSlot.data();
    state.preferSameCcd = (stealPolicy == StealPolicy::ClusterLocal);
    if constexpr (HasToken) {
      if (tok != CancellationToken{}) {
        state.token = std::move(tok);
      }
    } else {
      (void)tok;
    }
    // Only the deque-pushed children are tracked by the join counter; the
    // inline tail runs synchronously on this thread before drain begins.
    state.pendingTasks.store(static_cast<std::int64_t>(deferredCount),
                             std::memory_order_relaxed);

    if (participants <= 1) {
      // Single-participant fallback: run the deferred tasks serially via the
      // standard wrapper, then call the typed tail directly, with the same
      // execution order as `runForkJoinInline`, no array rebuild required. The
      // tail's cancel gate / exception capture / fetch_sub equivalent is the
      // same as the post-loop tail below.
      state.pendingTasks.store(static_cast<std::int64_t>(deferredCount + 1),
                               std::memory_order_relaxed);
      for (std::size_t i = 0; i < deferredCount; ++i) {
        deferred[i].state = &state;
        runOneTaskImpl<HasToken>(deferred[i], 0);
      }
      if (state.forkJoinCancelled.load(std::memory_order_acquire) == 0U &&
          (!HasToken || !state.token.stop_requested())) {
        try {
          std::forward<InlineFn>(inlineLast)();
        } catch (...) {
          captureFirstException(state);
          state.forkJoinCancelled.store(1U, std::memory_order_release);
        }
      }
      state.pendingTasks.fetch_sub(1, std::memory_order_release);
      rethrowIfCaptured(state);
      return;
    }

    for (std::size_t i = 0; i < deferredCount; ++i) {
      deferred[i].state = &state;
      m_workerDeques[callerSlot]->push(&deferred[i]);
    }

    // Inline the typed tail directly. Replicates `runOneTask`'s cancellation
    // gate and exception capture verbatim, but skips the FunctionRef indirect
    // call and the `pendingTasks.fetch_sub` (the tail is not in the join
    // counter).
    if (state.forkJoinCancelled.load(std::memory_order_acquire) == 0U &&
        (!HasToken || !state.token.stop_requested())) {
      try {
        std::forward<InlineFn>(inlineLast)();
      } catch (...) {
        captureFirstException(state);
        state.forkJoinCancelled.store(1U, std::memory_order_release);
      }
    }

    // Producer-local own-pop counter. Every task popped off our own deque whose
    // `state` matches this scope retires here (single-threaded against this
    // counter), so we elide the atomic `pendingTasks.fetch_sub` per own-pop.
    // The drain combines the atomic (peer-stolen decrements) with this local
    // counter to detect the join condition.
    std::int64_t ownPending = 0;
    this->template runForkJoinDrain<HasToken>(callerSlot, state, &ownPending);

    rethrowIfCaptured(state);
  }

  /// Run |nTasks| fork-join tasks recursively from inside a worker's drain
  /// loop. Called when `forkJoin` is invoked re-entrantly from a thread that
  /// is already a worker of this pool. Pushes the children onto the calling
  /// worker's own deque so they are visible to the standard victim-selection
  /// probe and runs the calling worker's drain loop with a fresh
  /// `detail::ForkJoinState` until the inner pending-task counter reaches
  /// zero. The outer state's drain loop is unaffected; the inner call
  /// borrows the worker for the duration of the recursive frame.
  void runForkJoinNested(detail::Task *tasks, std::size_t nTasks,
                         CancellationToken tok, StealPolicy stealPolicy,
                         std::uint32_t callerSlot) {
    const std::size_t participants = m_control.participants;

    // Per-recursion ForkJoinState init: skip the token move-assign when
    // |tok| is the default-constructed sentinel (m_state.get() == nullptr).
    // The default ForkJoinState::token is also a sentinel by construction,
    // so skipping the assign produces identical observable behavior
    // (stop_requested() returns false on both). Saves one shared_ptr copy +
    // potential refcount RMW per nested call.
    detail::ForkJoinState state;
    state.participants = static_cast<std::uint32_t>(participants);
    state.ccdOfSlot = m_ccdOfSlot.empty() ? nullptr : m_ccdOfSlot.data();
    state.preferSameCcd = (stealPolicy == StealPolicy::ClusterLocal);
    if (tok != CancellationToken{}) {
      state.token = std::move(tok);
    }
    state.pendingTasks.store(static_cast<std::int64_t>(nTasks),
                             std::memory_order_relaxed);

    if (participants <= 1) {
      runForkJoinInline(tasks, nTasks, state);
      rethrowIfCaptured(state);
      return;
    }

    // Cilk-5 "spawn parent" trick: push every task EXCEPT the last so peers can
    // steal them, then run the last task inline before entering the drain. The
    // drain subsequently pops the un-stolen pushed tasks (LIFO order), or
    // steals replacements from peers.
    //
    // Why this is correct: the pushed tasks are visible to peers via
    // Chase-Lev's release-store on bottom; the inline last task runs on this
    // worker concurrently with peer execution of the stolen tasks. The join
    // condition (pendingTasks == 0) is invariant to which slot executes which
    // task, so identity-permuting which one runs inline does not change
    // observed behaviour. The captured-exception path inside `runOneTask` is
    // shared across pushed and inline executions.
    //
    // Why this is faster: a push + LIFO pop pair on the same deque costs two
    // atomic ops on `bottom` (release on push, acquire on pop) plus a
    // memory_order_seq_cst CAS in the last-item race resolution. Inlining the
    // body skips both. Recursive workloads (fib, UTS, cilksort) call this path
    // on every level of recursion; saving the per-call deque traffic compounds
    // with depth. Eliminating one push+pop per recursive call moves the
    // producer's hot loop measurably.
    for (std::size_t i = 0; i + 1 < nTasks; ++i) {
      tasks[i].state = &state;
      m_workerDeques[callerSlot]->push(&tasks[i]);
    }
    tasks[nTasks - 1].state = &state;
    runOneTask(tasks[nTasks - 1], callerSlot);

    runForkJoinDrain(callerSlot, state);

    rethrowIfCaptured(state);
  }

  /// Inline fallback: run every task serially on the calling thread. Used for
  /// the single-participant case (no fan-out is possible) and also as the
  /// slow-path tail of `runForkJoinDrain` when nothing is left to steal but
  /// tasks are still pending. Each body's exception is caught and recorded
  /// into the call's `firstException` slot.
  static void runForkJoinInline(detail::Task *tasks, std::size_t nTasks,
                                detail::ForkJoinState &state) noexcept {
    for (std::size_t i = 0; i < nTasks; ++i) {
      tasks[i].state = &state;
      runOneTask(tasks[i], 0);
    }
  }

  /// Worker-side drain loop used by the producer (slot 0) and every
  /// background participant. Repeatedly pop a task from the worker's own
  /// deque; if empty, steal from another worker; run the popped/stolen task;
  /// loop until |state|'s `pendingTasks` counter reaches zero. The
  /// cancellation flag short-circuits the body and decrements the counter
  /// without running the payload so the join finishes promptly under
  /// cancellation or after a captured exception.
  template <bool HasToken = true>
  void runForkJoinDrain(std::uint32_t slot, detail::ForkJoinState &state,
                        std::int64_t *ownPending = nullptr) noexcept {
    const std::size_t participants = state.participants;
    auto &ownDeque = *m_workerDeques[slot];
    std::uint64_t rng = xorshiftSeed(slot);
    std::uint32_t spinCount = 0;
    // Hot-victim hint: remember the slot we last stole from. When a worker is
    // generating a deep recursion (UTS / cilksort interior nodes), the same
    // victim is likely to publish more children before its sub-tree is fully
    // consumed. Probing the last-good victim first turns a 1/(participants-1)
    // random hit into a near-100 % cache-warm hit; failed probes fall through
    // to the random path so a stale hint costs at most one extra cache-line
    // read.
    auto lastVictim =
        static_cast<std::uint32_t>(participants); // sentinel: invalid

    while (true) {
      const std::int64_t pending =
          state.pendingTasks.load(std::memory_order_acquire);
      // When `ownPending` is provided, the caller (producer) is tracking
      // own-pop retirements in a non-atomic stack-local counter; the join
      // condition combines the atomic (peer decrements) with the local (own
      // decrements). This eliminates the LOCK XADD on every own-pop in the hot
      // recursive fib-class path, where same-thread retirements dominate.
      const std::int64_t effective =
          (ownPending != nullptr) ? (pending - *ownPending) : pending;
      if (effective <= 0) {
        return;
      }

      // 1. Try to pop from the owner's own deque first. The Le 2013 last-item
      // race is settled
      //    by the deque's internal seq_cst CAS.
      if (auto popped = ownDeque.pop()) {
        // Own-state task on the producer's drain: skip the atomic decrement;
        // bump the stack-local `ownPending` counter instead. Foreign-state
        // tasks (nested recursion surfacing in the same deque) still use the
        // atomic path so their parent drain's join condition observes the
        // decrement.
        if (ownPending != nullptr && (*popped)->state == &state) {
          runOneTaskOwn<HasToken>(**popped);
          ++(*ownPending);
        } else {
          runOneTaskImpl<HasToken>(**popped, slot);
        }
        spinCount = 0;
        continue;
      }

      // 2. Hot-victim hint: probe the last successful victim before going
      // random. Cheap when it
      //    fits the recursion pattern; one extra atomic load when stale.
      if (lastVictim < participants && lastVictim != slot) {
        if (auto stolen = m_workerDeques[lastVictim]->steal()) {
          runOneTaskImpl<HasToken>(**stolen, slot);
          spinCount = 0;
          continue;
        }
        // Invalidate the stale hint so we don't keep probing the same
        // dry victim. Random probing below picks a fresh target; on the
        // next successful steal we re-arm `lastVictim`.
        lastVictim = static_cast<std::uint32_t>(participants);
      }

      // 3. Random victim probe. Same-CCD bias when CCD-local affinity is
      // requested.
      auto victimOut =
          static_cast<std::uint32_t>(participants); // unused if nullptr
      detail::Task *stolen = trySteal(slot, state, rng, victimOut);
      if (stolen != nullptr) {
        lastVictim = victimOut;
        runOneTaskImpl<HasToken>(*stolen, slot);
        spinCount = 0;
        continue;
      }

      // 3. Nothing to do right now. Yield in user space for a bounded time
      // before re-checking
      //    the pending counter; the rest of the drain stays off the kernel
      //    scheduler. We do not park here because the join condition is the
      //    pending counter, not a new generation.
      detail::cpuRelax();
      ++spinCount;
      // Cap the busy-spin so the worker eventually backs off the steal probes.
      // The cap is the smallest budget that keeps the steal-success-rate test
      // happy.
      if (spinCount >= 4096U) {
        spinCount = 0;
        // Yield once per cycle to let other ready threads make progress on
        // co-tenant cores.
#if defined(__x86_64__) || defined(_M_X64)
        for (std::uint32_t k = 0; k < 64U; ++k) {
          detail::cpuRelax();
        }
#else
        (void)participants;
#endif
      }
      (void)participants;
    }
  }

  /// Pick a victim slot and call its deque's `steal` entry. Uses an xorshift
  /// RNG seeded from the caller's slot id; when |state| requests CCD-local
  /// affinity, the candidate set is restricted to same-CCD victims first,
  /// falling back to any victim if no same-CCD steal succeeded after a full
  /// rotation. Returns the stolen `Task` pointer or `nullptr` on
  /// contention / empty.
  detail::Task *trySteal(std::uint32_t self, detail::ForkJoinState &state,
                         std::uint64_t &rng,
                         std::uint32_t &victimOut) noexcept {
    const std::uint32_t participants = state.participants;
    if (participants <= 1) {
      return nullptr;
    }

    // Probe ONE random victim per call regardless of CCD preference. The drain
    // loop's outer spinCount budget provides the inter-attempt backoff. Probing
    // all same-CCD victims in a tight inner loop here burned most CPU on the
    // steal path for fib-class workloads: idle workers chasing a small root set
    // repeatedly loaded many empty deques before yielding.
    //
    // Same-CCD bias is preserved as a probability bias rather than an
    // unconditional sweep: 7/8 of probes target a same-CCD victim, 1/8 reach
    // for a cross-CCD victim. The bias keeps CCD-local locality without burning
    // cross-CCD coherence traffic on empty-deque sweeps, and the cross-CCD
    // escape valve still finds work when same-CCD is starved (otherwise workers
    // on a CCD with no live tasks would never steal at all).
    //
    // `victimOut` is filled with the probed slot so the caller can use it as a
    // hot-victim hint on subsequent calls. Written even on failure so the
    // caller can avoid re-probing the same empty deque on the very next
    // iteration.
    if (state.preferSameCcd) {
      const auto roll = static_cast<std::uint32_t>(xorshiftNext(rng) & 0x7U);
      const bool tryCross = (roll == 0U);
      if (!tryCross && !m_sameCcdVictims[self].empty()) {
        const auto &same = m_sameCcdVictims[self];
        const auto sameSize = static_cast<std::uint32_t>(same.size());
        const auto cursor = fastRange32(xorshiftNext(rng), sameSize);
        const auto victim = same[cursor];
        victimOut = victim;
        if (auto stolen = m_workerDeques[victim]->steal()) {
          return *stolen;
        }
        return nullptr;
      }
      const auto &cross = m_crossCcdVictims[self];
      const auto crossSize = static_cast<std::uint32_t>(cross.size());
      if (crossSize > 0U) {
        const auto cursor = fastRange32(xorshiftNext(rng), crossSize);
        const auto victim = cross[cursor];
        victimOut = victim;
        if (auto stolen = m_workerDeques[victim]->steal()) {
          return *stolen;
        }
      }
      return nullptr;
    }

    // No CCD preference: probe one random victim per call.
    {
      const auto &row = m_allVictims[self];
      const auto rowSize = static_cast<std::uint32_t>(row.size());
      const auto cursor = fastRange32(xorshiftNext(rng), rowSize);
      const auto victim = row[cursor];
      victimOut = victim;
      if (auto stolen = m_workerDeques[victim]->steal()) {
        return *stolen;
      }
    }
    return nullptr;
  }

  /// Execute one popped/stolen `detail::Task`. Runs the body in a try-block;
  /// on success decrements the call's `pendingTasks` counter via release so
  /// the producer's spin-loop establishes happens-before with whatever the
  /// body wrote. On exception, records the first throw into the call's
  /// `firstException` slot, sets the cancellation flag, and decrements
  /// unconditionally so the join finishes.
  ///
  /// Producer-local variant: skips `state.pendingTasks.fetch_sub` because
  /// the caller is tracking the decrement in a non-atomic stack-local
  /// counter. Used by the producer's own drain when the popped task belongs
  /// to the producer's own scope; peer-stolen tasks still use
  /// `runOneTaskImpl<HasToken>` to keep the cross-thread atomic.
  template <bool HasToken>
  [[gnu::always_inline]] static void
  runOneTaskOwn(detail::Task &task) noexcept {
    detail::ForkJoinState &state = *task.state;
    if (state.forkJoinCancelled.load(std::memory_order_acquire) != 0U ||
        (HasToken && state.token.stop_requested())) [[unlikely]] {
      return;
    }
    try {
      task.body();
    } catch (...) {
      captureFirstException(state);
      state.forkJoinCancelled.store(1U, std::memory_order_release);
    }
  }

  /// Cross-thread variant of the per-task runner. Decrements
  /// `state.pendingTasks` after the body returns or after a cancellation
  /// short-circuit so the producer's join condition can observe
  /// completion. `HasToken` elides the token poll when the caller routed
  /// through the no-token entry.
  template <bool HasToken>
  static void runOneTaskImpl(detail::Task &task,
                             std::uint32_t /*slot*/) noexcept {
    detail::ForkJoinState &state = *task.state;

    // Honour cancellation by decrementing pending so the join can
    // observe `pendingTasks == 0`. Always poll `state.token`: the
    // caller's `HasToken` reflects the OUTER drain's instantiation,
    // but a peer-stolen task may belong to a nested call whose state
    // carries an owned token. The default sentinel resolves to a single
    // null-pointer test, so the no-token hot path pays one branch.
    if (state.forkJoinCancelled.load(std::memory_order_acquire) != 0U ||
        state.token.stop_requested()) [[unlikely]] {
      state.pendingTasks.fetch_sub(1, std::memory_order_release);
      return;
    }
    (void)HasToken;

    try {
      task.body();
    } catch (...) {
      captureFirstException(state);
      state.forkJoinCancelled.store(1U, std::memory_order_release);
    }
    state.pendingTasks.fetch_sub(1, std::memory_order_release);
  }

  /// Default-token specialisation of `runOneTaskImpl<true>`. Used by the
  /// peer-stolen task path that does not statically know whether the
  /// caller passed a token.
  static void runOneTask(detail::Task &task, std::uint32_t slot) noexcept {
    runOneTaskImpl<true>(task, slot);
  }

  /// Seed an xorshift64 RNG from |slot|. The RNG drives `forkJoin`'s
  /// victim-selection probe order. Seeding from the slot id keeps each
  /// worker's first probe distinct so workers do not pile on the same
  /// victim.
  std::uint64_t xorshiftSeed(std::uint32_t slot) const noexcept {
    // Mix in `this` so two arenas with the same slot id produce different
    // RNG sequences. Without the `this` mix, slot 1 in arena A and slot 1
    // in arena B started their victim probes in lock-step, doubling
    // cache-line contention on the same victims.
    std::uint64_t s =
        0x9E3779B97F4A7C15ULL ^
        (static_cast<std::uint64_t>(slot) * 0xBF58476D1CE4E5B9ULL) ^
        reinterpret_cast<std::uintptr_t>(this);
    if (s == 0) {
      s = 1;
    }
    return s;
  }

  /// Advance an xorshift64 RNG. Marsaglia's xorshift64 sequence: one 64-bit
  /// register, three shifts, no branches.
  static std::uint64_t xorshiftNext(std::uint64_t &state) noexcept {
    std::uint64_t s = state;
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    state = s;
    return s;
  }

  /// Reduce a 64-bit RNG draw to `[0, n)` via multiply-high (Lemire 2018),
  /// replacing `xorshiftNext(rng) % n` in the steal hot path so the
  /// non-power-of-two case avoids `idiv`. Bias is bounded by `1 / 2^32`
  /// per bucket, irrelevant for victim selection. |n| must be non-zero.
  static std::uint32_t fastRange32(std::uint64_t x, std::uint32_t n) noexcept {
    return static_cast<std::uint32_t>(((x >> 32) * n) >> 32);
  }

  /// Common job-publish/join helper used by the reduce paths. Builds a
  /// `JobDescriptor` with the supplied static chunk shape, dispatches via
  /// `dispatchOne`, and returns once every worker has stamped its
  /// `doneEpoch`. The caller reads the partials array after this returns;
  /// the join's acquire-load on `doneEpoch` establishes happens-before for
  /// the partial writes.
  void dispatchReduceJob(std::size_t first, std::size_t last,
                         FunctionRef<void(std::size_t, std::size_t)> body,
                         CancellationToken tok, std::size_t participants,
                         std::size_t chunk, std::size_t nChunks) {
    detail::JobDescriptor desc;
    desc.first = first;
    desc.last = last;
    desc.participants = static_cast<std::uint32_t>(participants);
    desc.balance = Balance::StaticUniform;
    desc.priority = Priority::Throughput;
    desc.preWakeCompletionProbe = true;
    desc.body = body;
    desc.token = std::move(tok);
    desc.chunk = chunk;
    desc.blockCount = nChunks;

    dispatchOneStatic<Balance::StaticUniform>(desc);
  }

  /// Typed reduce dispatcher used by the parallel reducer engine. Reuses
  /// the per-thread shared `JobDescriptor` cache and tracks
  /// field-by-field whether the descriptor matches the previous
  /// dispatch; on a match the worker entry's reuse path skips the
  /// per-rank descriptor read.
  template <class HintsT, class Body>
  void dispatchReduceJobTyped(std::size_t first, std::size_t last, Body &body,
                              const CancellationToken &tok,
                              std::size_t participants, std::size_t chunk,
                              std::size_t nChunks) {
    constexpr bool kCancellationActive = detail::kCancellationActive<HintsT>;

    detail::JobDescriptor &desc = sharedReduceDesc();
    bool keyMatches = true;
    if (desc.first != first) {
      desc.first = first;
      keyMatches = false;
    }
    if (desc.last != last) {
      desc.last = last;
      keyMatches = false;
    }
    if (desc.participants != participants) {
      desc.participants = static_cast<std::uint32_t>(participants);
      keyMatches = false;
    }
    if (desc.balance != Balance::StaticUniform) {
      desc.balance = Balance::StaticUniform;
      keyMatches = false;
    }
    if (desc.priority != HintsT::priority) {
      desc.priority = HintsT::priority;
      keyMatches = false;
    }
    if (!desc.preWakeCompletionProbe) {
      desc.preWakeCompletionProbe = true;
      keyMatches = false;
    }
    if (desc.chunk != chunk) {
      desc.chunk = chunk;
      keyMatches = false;
    }
    if (desc.blockCount != nChunks) {
      desc.blockCount = nChunks;
      keyMatches = false;
    }
    auto *fnAddr = static_cast<void *>(std::addressof(body));
    if (desc.fnPtr != fnAddr) {
      desc.fnPtr = fnAddr;
      keyMatches = false;
    }
    auto *expectedEntry =
        &detail::typedStaticContiguousWorkerEntry<HintsT, Body>;
    if (desc.workerEntry != expectedEntry) {
      desc.workerEntry = expectedEntry;
      keyMatches = false;
    }
    if (desc.workerStateBase != nullptr) {
      desc.workerStateBase = nullptr;
      keyMatches = false;
    }
    if constexpr (kCancellationActive) {
      if (desc.token != tok) {
        desc.token = tok;
        keyMatches = false;
      }
    } else {
      (void)tok;
    }

    dispatchOneStaticContiguousTypedSlot0Hinted<HintsT>(
        desc, body, !kCancellationActive && keyMatches);
  }

  /// Acquire the priority gate that serializes concurrent `dispatchOne`
  /// callers. Concurrent producers contending on the same pool pass through
  /// this gate before publishing the next generation. The gate is a small
  /// two-bucket priority lane:
  ///
  /// - `Priority::Latency` callers register their waiting count via
  ///   `m_latencyWaiting` and then acquire `m_dispatchMutex` directly. The
  ///   increment is observed by throughput / background callers, which back
  ///   off until it drops to zero before locking.
  /// - `Priority::Throughput` callers register via `m_throughputWaiting` and
  ///   acquire the mutex after spinning until the latency waiter count is
  ///   zero. This is the default and the hot path when only one producer
  ///   dispatches at a time.
  /// - `Priority::Background` callers wait for both `m_latencyWaiting` and
  ///   `m_throughputWaiting` to drop to zero before locking, AND
  ///   release-and-retry if either counter ticks up while they hold the lock
  ///   but have not yet started the dispatch body. Background is
  ///   best-effort and may starve under sustained higher-priority traffic;
  ///   the contract is "yield, do not preempt".
  ///
  /// The single-producer fast path bypasses the back-off entirely so the hot
  /// dispatch path pays one un-contended atomic increment, one atomic
  /// decrement, and one mutex lock/unlock pair.
  void acquireDispatchGate(Priority priority) noexcept {
    if (priority == Priority::Latency) {
      m_latencyWaiting.fetch_add(1, std::memory_order_acq_rel);
      dispatchGateLock();
      m_latencyWaiting.fetch_sub(1, std::memory_order_acq_rel);
      return;
    }
    if (priority == Priority::Throughput) {
      // Single-producer fast path: try the lock without registering. If it
      // succeeds and no latency caller has registered, the gate is held with
      // zero atomic increments. Re-checking `m_latencyWaiting` after `try_lock`
      // preserves priority ordering: a latency caller that registered between
      // our load and our `try_lock` wins because `lock()` blocks behind us
      // until we release; the recheck here covers the symmetric race.
      if (m_latencyWaiting.load(std::memory_order_acquire) == 0U &&
          dispatchGateTryLock()) {
        if (m_latencyWaiting.load(std::memory_order_acquire) == 0U) {
          return;
        }
        dispatchGateUnlock();
      }
      m_throughputWaiting.fetch_add(1, std::memory_order_acq_rel);
      while (true) {
        // Defer to any latency caller that registered itself before us. The
        // release/acquire on the counter pairs with the latency caller's
        // increment; missing a transient bump merely costs one extra spin pass
        // (correctness-neutral).
        while (m_latencyWaiting.load(std::memory_order_acquire) != 0U) {
          std::this_thread::yield();
        }
        dispatchGateLock();
        if (m_latencyWaiting.load(std::memory_order_acquire) != 0U) {
          // Latency caller registered while we were waiting on the lock;
          // release so its own `lock()` can succeed and retry from the wait
          // loop.
          dispatchGateUnlock();
          std::this_thread::yield();
          continue;
        }
        m_throughputWaiting.fetch_sub(1, std::memory_order_acq_rel);
        return;
      }
    }
    // Background: wait for both higher-priority lanes to drain before
    // contending.
    while (true) {
      while (m_latencyWaiting.load(std::memory_order_acquire) != 0U ||
             m_throughputWaiting.load(std::memory_order_acquire) != 0U) {
        std::this_thread::yield();
      }
      dispatchGateLock();
      if (m_latencyWaiting.load(std::memory_order_acquire) != 0U ||
          m_throughputWaiting.load(std::memory_order_acquire) != 0U) {
        // A higher-priority caller registered while we were waiting on the
        // lock; release so their own `lock()` can succeed and retry from the
        // wait loop.
        dispatchGateUnlock();
        std::this_thread::yield();
        continue;
      }
      return;
    }
  }

  /// Release the priority gate previously acquired via
  /// `acquireDispatchGate`. Pair with the matching acquire in `dispatchOne`'s
  /// scope; idempotent only when called once per acquire (no double-release
  /// defense; the call site guarantees the pairing).
  void releaseDispatchGate() noexcept { dispatchGateUnlock(); }

  /// Dispatch-gate primitives. Linux wraps `std::mutex` (glibc's
  /// `pthread_mutex` is already a futex pair). Windows uses a CAS fast
  /// path with a bounded `PAUSE` spin and `WaitOnAddress` park because
  /// `SRWLock` carries measurable overhead on the cold-dispatch budget.
#ifdef _WIN32
  /// Lock-word `held` bit. Set while a thread owns the gate.
  static constexpr std::uint32_t kHeldBit = 1U;
  /// Lock-word `has-waiter-parked` bit. Set when at least one thread is
  /// parked on the lock word via `WaitOnAddress`; lets the uncontended
  /// unlock elide the `WakeByAddressSingle` syscall.
  static constexpr std::uint32_t kHasWaiterBit = 2U;

  /// CAS the lock word from free (0) to held (kHeldBit). Returns true on
  /// success.
  [[nodiscard, gnu::always_inline]] bool dispatchGateTryLock() noexcept {
    std::uint32_t expected = 0U;
    return m_dispatchMutex.compare_exchange_strong(expected, kHeldBit,
                                                   std::memory_order_acquire,
                                                   std::memory_order_relaxed);
  }
  /// Acquire the gate. CAS fast path then bounded PAUSE spin; on
  /// sustained contention, set the has-waiter bit and park via
  /// `WaitOnAddress`. Returns only when the gate is held.
  void dispatchGateLock() noexcept {
    if (dispatchGateTryLock()) {
      return;
    }
    // Contended: brief PAUSE spin absorbs short hand-offs without a
    // syscall, then park via WaitOnAddress so sustained contention
    // does not burn CPU.
    constexpr unsigned kSpinIters = 64U;
    for (unsigned i = 0; i < kSpinIters; ++i) {
      if (m_dispatchMutex.load(std::memory_order_relaxed) == 0U &&
          dispatchGateTryLock()) {
        return;
      }
      detail::cpuRelax();
    }
    // Post-park acquire CAS sets `kHeldBit | kHasWaiterBit`, not just
    // `kHeldBit`: a sibling may have parked concurrently, and acquiring
    // without preserving the bit lets the matching unlock observe
    // `prev == kHeldBit` and skip the wake, stranding the sibling. One
    // spurious wake is cheap; missing a wake is a hang.
    for (;;) {
      std::uint32_t cur = m_dispatchMutex.load(std::memory_order_relaxed);
      if (cur == 0U) {
        if (m_dispatchMutex.compare_exchange_weak(cur, kHeldBit | kHasWaiterBit,
                                                  std::memory_order_acquire,
                                                  std::memory_order_relaxed)) {
          return;
        }
        continue;
      }
      if ((cur & kHasWaiterBit) == 0U) {
        if (!m_dispatchMutex.compare_exchange_weak(cur, cur | kHasWaiterBit,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_relaxed)) {
          continue;
        }
        cur |= kHasWaiterBit;
      }
      // Park on the lock word. If the holder unlocks and the word
      // transitions away from `cur`, the kernel returns immediately.
      (void)::WaitOnAddress(static_cast<volatile VOID *>(&m_dispatchMutex),
                            &cur, sizeof(std::uint32_t), INFINITE);
    }
  }
  /// Release the gate. Wakes one parked thread via `WakeByAddressSingle`
  /// when the has-waiter bit was observed on the unlock; uncontended
  /// release is one relaxed exchange and no syscall.
  void dispatchGateUnlock() noexcept {
    const std::uint32_t prev =
        m_dispatchMutex.exchange(0U, std::memory_order_release);
    if ((prev & kHasWaiterBit) != 0U) {
      ::WakeByAddressSingle(static_cast<PVOID>(&m_dispatchMutex));
    }
  }
#else
  /// Try-lock peer on Linux: forwards to `std::mutex::try_lock`.
  [[nodiscard, gnu::always_inline]] bool dispatchGateTryLock() noexcept {
    return m_dispatchMutex.try_lock();
  }
  /// Lock peer on Linux: forwards to `std::mutex::lock`.
  [[gnu::always_inline]] void dispatchGateLock() noexcept {
    m_dispatchMutex.lock();
  }
  /// Unlock peer on Linux: forwards to `std::mutex::unlock`.
  [[gnu::always_inline]] void dispatchGateUnlock() noexcept {
    m_dispatchMutex.unlock();
  }
#endif

  /// Common publish / participate / join helper shared by every primitive.
  ///
  /// 1. Acquire the priority gate so concurrent producers serialize through
  ///    a 2-bucket lane.
  /// 2. Compute the next generation by adding `kPhaseStep` to the current
  ///    value (preserving shutdown / cancel flag bits).
  /// 3. Stamp the generation onto |desc| and publish via release-store on
  ///    `activeJob`.
  /// 4. Release-store the new `generation`; this is the publish edge
  ///    workers acquire.
  /// 5. Bump `futexWord` (relaxed) and broadcast `FUTEX_WAKE_PRIVATE` to
  ///    every parked worker.
  /// 6. Producer participates as slot 0 by running its share of the job
  ///    inline.
  /// 7. Acquire-load each background worker's `doneEpoch` until all reach
  ///    the new generation.
  /// 8. Release-store `nullptr` into `activeJob` so the slot is empty for
  ///    the next dispatch.
  /// 9. Release the priority gate.
  /// 10. Rethrow the captured first-exception if any.
  void dispatchOne(detail::JobDescriptor &desc) {
    if (desc.balance == Balance::StaticUniform) {
      dispatchOneStatic<Balance::StaticUniform>(desc);
      return;
    }
    dispatchOneStatic<Balance::DynamicChunked>(desc);
  }

  // RAII guard that installs slot-0 producer TLS context and restores it on
  // scope exit. Used by `dispatchOneStaticLockedBody` so a throw out of the
  // producer's inline body cannot leave the producer thread's TLS pointing
  // at this pool. The destructor runs even during stack unwind, restoring
  // the prior values.
  class TlsContextScope {
  public:
    /// Saves the current TLS pool context and installs the slot-0
    /// producer view: `|pool|`, `|kind|`, `|arenaIndex|`, `|slot|`. The
    /// destructor restores the saved context, so a throw out of the
    /// producer body never leaks a stale pool tag onto the thread.
    TlsContextScope(ThreadContext &ctx, ThreadPool *pool, PoolKind kind,
                    std::uint32_t arenaIndex, std::size_t slot) noexcept
        : m_ctx(ctx), m_savedInside(ctx.insidePoolWorker) {
      // Save outgoing context unconditionally. Earlier we only saved when
      // m_savedInside was true, which left a non-worker producer's slot tag
      // unwritten when cold-collapse passed slot=bit. The user body's
      // `workerIndex()` then read the stale slot 0 instead of the rank it was
      // running on.
      m_savedSlot = ctx.slot;
      m_savedPool = ctx.pool;
      m_savedKind = ctx.kind;
      m_savedArenaIndex = ctx.arenaIndex;
      m_ctx.slot = slot;
      m_ctx.insidePoolWorker = true;
      m_ctx.pool = pool;
      m_ctx.kind = kind;
      m_ctx.arenaIndex = arenaIndex;
    }
    ~TlsContextScope() {
      m_ctx.slot = m_savedSlot;
      m_ctx.insidePoolWorker = m_savedInside;
      m_ctx.pool = m_savedPool;
      m_ctx.kind = m_savedKind;
      m_ctx.arenaIndex = m_savedArenaIndex;
    }
    TlsContextScope(const TlsContextScope &) = delete;
    TlsContextScope &operator=(const TlsContextScope &) = delete;
    TlsContextScope(TlsContextScope &&) = delete;
    TlsContextScope &operator=(TlsContextScope &&) = delete;

  private:
    /// Reference to the calling thread's `ThreadContext` slot.
    ThreadContext &m_ctx;
    /// Slot id captured at construction; restored by the destructor.
    std::size_t m_savedSlot;
    /// Saved value of `ctx.insidePoolWorker`.
    bool m_savedInside;
    /// Saved value of `ctx.pool`.
    ThreadPool *m_savedPool;
    /// Saved value of `ctx.kind`.
    PoolKind m_savedKind;
    /// Saved value of `ctx.arenaIndex`.
    std::uint32_t m_savedArenaIndex;
  };

  // RAII helper that holds `m_dispatchMutex` across pre-publish scratch
  // mutation. Some primitives (`parallelChain`, `parallelScan`, `forkJoin`)
  // mutate pool-owned structures (`m_chainDoneSlots`, `m_workerDeques`,
  // per-task `state` pointers) before publishing the dispatch generation;
  // without holding the gate while doing so, two concurrent producers race
  // on those structures. Callers acquire a `DispatchLease` first, perform
  // their mutation, then call `dispatchOneStaticLocked`, which skips the
  // redundant gate acquisition.
  class DispatchLease {
  public:
    /// Acquires the dispatch gate on `|pool|` for `|priority|`. Skips the
    /// acquire when the calling thread owns an active `LowLatencyGuard`
    /// on this pool, since the LL contract is single-producer.
    DispatchLease(ThreadPool &pool, Priority priority) noexcept : m_pool(pool) {
      // Hot-spin scope fast path: while a `LowLatencyGuard` is alive AND the
      // current thread is the guard's owner, the contract is single-producer
      // and the dispatch gate is dead overhead. Skip both the acquire and the
      // matching release.
      //
      // A second thread that tries to dispatch concurrently while another
      // thread holds the guard MUST take the mutex, otherwise its publish
      // would race with the owner thread on `activeJob` and per-worker
      // mailboxes. Without the TLS gate, the prior contract was enforced by
      // documentation only; multi-producer benchmark / test code routinely
      // tripped the data race under TSAN.
      if (pool.m_control.hotSpinDepth.load(std::memory_order_acquire) != 0U &&
          lowLatencyOwnerPoolTls() == &pool) {
        m_skipped = true;
        return;
      }
      m_pool.acquireDispatchGate(priority);
    }
    ~DispatchLease() {
      if (!m_skipped) {
        m_pool.releaseDispatchGate();
      }
    }
    /// True when the constructor took the LL fast path and the
    /// destructor will not release the gate. Threaded into
    /// `dispatchOneStaticLockedBody` so the inner body avoids re-probing
    /// the same atomic.
    [[nodiscard]] bool gateSkipped() const noexcept { return m_skipped; }
    DispatchLease(const DispatchLease &) = delete;
    DispatchLease &operator=(const DispatchLease &) = delete;
    DispatchLease(DispatchLease &&) = delete;
    DispatchLease &operator=(DispatchLease &&) = delete;

  private:
    /// Pool the lease holds the gate on.
    ThreadPool &m_pool;
    /// True when the constructor took the LL fast path and skipped the
    /// gate acquire.
    bool m_skipped = false;
  };

  /// Drain cold-stamped slots before the producer writes the next dispatch's
  /// `JobDescriptor`. Each pending worker's mailbox is acquire-loaded; the
  /// matching ack-side `fetch_or(kAckedBit, release)` orders the worker's
  /// prior desc reads before the producer's subsequent writes.
  [[gnu::always_inline]] void drainColdStampedAcks() noexcept {
    std::uint64_t mask = m_coldStampedMask.load(std::memory_order_acquire);
    if (mask == 0U) [[likely]] {
      return;
    }
    auto *workersBase = m_workers.get();
    const bool lowLatencyActive =
        m_control.hotSpinDepth.load(std::memory_order_acquire) != 0U;
    while (mask != 0U) {
      std::uint64_t scan = mask;
      std::uint64_t stillPending = 0U;
      while (scan != 0U) {
        const auto bit = detail::ctzll(scan);
        scan &= scan - 1U;
        auto *w = workersBase + bit;
        if ((w->mailbox.load(std::memory_order_acquire) &
             detail::PoolControl::kAckedBit) == 0U) {
          stillPending |= std::uint64_t{1} << bit;
        }
      }
      const std::uint64_t cleared = mask & ~stillPending;
      if (cleared != 0U) {
        m_coldStampedMask.fetch_and(~cleared, std::memory_order_release);
      }
      if (stillPending == 0U) {
        break;
      }
      if (!lowLatencyActive) {
        const std::uint32_t nextFutex =
            m_control.futexWord.load(std::memory_order_relaxed) + 1U;
        m_control.futexWord.store(nextFutex, std::memory_order_release);
        (void)detail::futexWakePrivate(&m_control.futexWord, 2);
      }
      detail::cpuRelax();
      mask = m_coldStampedMask.load(std::memory_order_acquire);
    }
  }

  /// Static-balance dispatch entry. Acquires a `DispatchLease` and
  /// forwards into `dispatchOneStaticLockedBody`.
  template <Balance BalanceV>
  void dispatchOneStatic(detail::JobDescriptor &desc) {
    const DispatchLease lease(*this, desc.priority);
    // `gateSkipped()` mirrors the `hotSpinDepth != 0` probe the lease just
    // performed; thread it through so the inner body does not re-load the same
    // atomic on a fresh cache-line touch.
    dispatchOneStaticLockedBody<BalanceV>(desc, lease.gateSkipped());
  }

  /// Typed entry into the static-balance dispatch path: the producer's
  /// slot-0 inline body call bypasses `desc.body`'s `FunctionRef`
  /// indirection by invoking `fn` directly. Workers still see the
  /// descriptor with `desc.body` set, so they go through
  /// `runStaticPartition` unchanged. Only the producer's slot-0 inline call
  /// benefits.
  ///
  /// Caller responsibility: `desc.body` must still be set to a `FunctionRef`
  /// wrapping `fn` so workers can drive their share. This entry assumes
  /// `BalanceV == StaticUniform`.
  template <class HintsT, class FOp>
  [[gnu::always_inline]] void
  dispatchOneStaticTypedSlot0Hinted(detail::JobDescriptor &desc, FOp &fn,
                                    bool reuseHint = false) {
    // Pass the constexpr priority directly; avoids the desc.priority load on
    // the producer's pre-lease path. Under LL (hotSpinDepth != 0) the priority
    // arg is unused by the lease ctor.
    const DispatchLease lease(*this, HintsT::priority);
    dispatchOneStaticLockedBody<Balance::StaticUniform, FOp, HintsT>(
        desc, lease.gateSkipped(), &fn, reuseHint);
  }

  /// Typed entry for the static-contiguous slot-0 path. Drops the
  /// rank-claim CAS and the atomic-tail counter; each rank walks its
  /// `contiguousRankBlockSpan`. Used by the parallel reducer engine.
  template <class HintsT, class FOp>
  [[gnu::always_inline]] void
  dispatchOneStaticContiguousTypedSlot0Hinted(detail::JobDescriptor &desc,
                                              FOp &fn, bool reuseHint = false) {
    const DispatchLease lease(*this, HintsT::priority);
    dispatchOneStaticLockedBody<Balance::StaticUniform, FOp, HintsT, true>(
        desc, lease.gateSkipped(), &fn, reuseHint);
  }

  /// Untyped-priority counterpart of `dispatchOneStaticTypedSlot0Hinted`.
  /// Pulls the priority from `|desc|` rather than a compile-time
  /// `HintsT::priority`.
  template <class FOp>
  void dispatchOneStaticTypedSlot0(detail::JobDescriptor &desc, FOp &fn,
                                   bool reuseHint = false) {
    const DispatchLease lease(*this, desc.priority);
    dispatchOneStaticLockedBody<Balance::StaticUniform>(
        desc, lease.gateSkipped(), &fn, reuseHint);
  }

  /// Typed entry into the dynamic-balance dispatch path: the producer's
  /// slot-0 inline body call bypasses `desc.body`'s `FunctionRef` indirection
  /// by invoking `fn` directly. Sibling of
  /// `dispatchOneStaticTypedSlot0Hinted` for `Balance::DynamicChunked`
  /// callers.
  template <class HintsT, class FOp>
  [[gnu::always_inline]] void
  dispatchOneDynamicTypedSlot0Hinted(detail::JobDescriptor &desc, FOp &fn,
                                     bool reuseHint = false) {
    const DispatchLease lease(*this, HintsT::priority);
    dispatchOneStaticLockedBody<Balance::DynamicChunked, FOp, HintsT>(
        desc, lease.gateSkipped(), &fn, reuseHint);
  }

  /// Same as `dispatchOneStatic` but assumes the caller already holds the
  /// dispatch gate. Call sites that mutate pool-owned scratch (e.g.,
  /// zero-resetting `m_chainDoneSlots` or pushing descriptors onto
  /// `m_workerDeques`) acquire a `DispatchLease` first, perform their
  /// mutation, then invoke this overload to publish/participate/join. The
  /// lease's destructor releases the gate after the call returns (or after
  /// an exception unwinds through it).
  template <Balance BalanceV>
  void dispatchOneStaticLocked(detail::JobDescriptor &desc) {
    // Locked entry points are reached after the caller already mutated
    // pool-owned scratch under its own lease, so the gate state has not been
    // re-probed; recompute the LL flag here.
    const bool lowLatencyActive =
        m_control.hotSpinDepth.load(std::memory_order_acquire) != 0U;
    dispatchOneStaticLockedBody<BalanceV>(desc, lowLatencyActive);
  }

  /// Inner body of the locked dispatch path. Publishes the new
  /// generation, wakes parked workers, runs slot 0 inline, joins on
  /// every worker's `doneEpoch`, and rethrows the captured first
  /// exception (if any). `Slot0Body` and `Slot0Hints` carry typed slot-0
  /// information when the caller passed a typed entry; `ContiguousRank`
  /// selects the static-contiguous worker path.
  template <Balance BalanceV, class Slot0Body = std::nullptr_t,
            class Slot0Hints = void, bool ContiguousRank = false>
  [[gnu::always_inline]] void
  dispatchOneStaticLockedBody(detail::JobDescriptor &desc,
                              bool lowLatencyActive, Slot0Body *slot0 = nullptr,
                              bool reuseHint = false) {
    CITOR_COUNTERS_INC(dispatches);
    const std::uint64_t prev =
        m_control.generation.load(std::memory_order_relaxed);
    const std::uint64_t nextGen = prev + detail::PoolControl::kPhaseStep;
    // Same-command reuse: when the producer's TLS key matched the previous
    // dispatch, the workers' cached job parameters are still valid. We OR
    // kReuseBit into the published mailbox value so workers skip reading desc
    // fields entirely. Producer also skips the mailboxDesc store in this case
    // (the workers won't dereference it).
    //
    // Pool-level invalidation: the producer's `keyMatches` only sees its own
    // TLS desc; it does not observe whether a different thread (different TLS
    // desc) or a different pool (different worker mailboxes) intervened. The
    // workers' caches are primed against `m_lastPublishedDesc`, so reuse is
    // only safe when the producer's `&desc` IS that pointer. The
    // compare-and-store happens here under the dispatch gate, so a relaxed load
    // is sufficient.
    const bool reuseSafe =
        reuseHint &&
        (m_lastPublishedDesc.load(std::memory_order_relaxed) == &desc);
    const std::size_t participantsLocal = m_control.participants;
    // Same-command multi-block reuse means every worker already has a primed
    // typed-runner cache and the dispatch shape has at least one follow-up
    // block beyond the first rank round. In that hot shape, cold-collapse's
    // per-worker `claimedAt` CAS is coherence traffic with no expected save:
    // workers are already polling their mailbox and can run their own ranks.
    // Keep cold-collapse for first publishes, one-block-per-rank fanouts, and
    // non-reused calls where the producer may need to cover a genuinely late
    // worker.
    const bool hotReuseMultiBlock =
        reuseSafe && desc.blockCount > participantsLocal;
    const bool skipColdCollapse = (lowLatencyActive || hotReuseMultiBlock) &&
                                  participantsLocal > 2U &&
                                  desc.workerStateBase != nullptr;
    const std::uint64_t publishedMb =
        nextGen | (reuseSafe ? detail::PoolControl::kReuseBit : 0ULL) |
        (skipColdCollapse ? detail::PoolControl::kSkipClaimBit : 0ULL);
    const std::uint64_t doneSentinelMb =
        publishedMb | detail::PoolControl::kDoneBit;
    // Stash the dispatch's generation onto the descriptor so the cold-collapse
    // CAS-claim path (worker side: `typedStaticUniformWorkerEntry`; producer
    // side: the join-wait fallback below) can use it as the comparator for
    // `WorkerState::claimedAt`. The release-store on each worker's mailbox
    // sequenced-after this relaxed store is the visibility edge: workers
    // acquire-load mailbox and see the matching `desc.generation` write. The
    // historical "desc.generation field is dead" comment no longer holds now
    // that cold-collapse reads it.
    desc.generation = nextGen;

    m_control.activeJob.store(static_cast<void *>(&desc),
                              std::memory_order_release);
    m_control.generation.store(nextGen, std::memory_order_release);

    // Per-worker mailbox publish: write `mailboxDesc` (relaxed; sequenced
    // before the mailbox store by program order), then `mailbox` (release).
    // Both fields share the worker's private cache line, so the worker's
    // acquire-load on `mailbox` picks both up in a single line transit and
    // bypasses the shared `m_control.activeJob` line that previously cost every
    // worker one cache-line read per dispatch.
    {
      auto *workersBaseForPublish = m_workers.get();
      auto *descRaw = static_cast<void *>(&desc);
      // Drain pending cold-stamped acks before any publish (reuse or
      // not). Overwriting a previously cold-collapsed worker's mailbox
      // while it is still parked sends it down the plain-store ack
      // branch on wake, which never sets `kAckedBit`; a later non-reuse
      // publish would then spin here forever. Wake while waiting
      // because the worker we need an ack from may already be parked;
      // a pure cpuRelax spin would not resolve.
      // Cold-stamped ack drain for primitives sharing this body that did not
      // call `drainColdStampedAcks` at their entry.
      std::uint64_t mask = m_coldStampedMask.load(std::memory_order_acquire);
      if (mask != 0U) [[unlikely]] {
        while (mask != 0U) {
          std::uint64_t scan = mask;
          std::uint64_t stillPending = 0U;
          while (scan != 0U) {
            const auto bit = detail::ctzll(scan);
            scan &= scan - 1U;
            auto *w = workersBaseForPublish + bit;
            if ((w->mailbox.load(std::memory_order_acquire) &
                 detail::PoolControl::kAckedBit) == 0U) {
              stillPending |= std::uint64_t{1} << bit;
            }
          }
          const std::uint64_t cleared = mask & ~stillPending;
          if (cleared != 0U) {
            m_coldStampedMask.fetch_and(~cleared, std::memory_order_release);
          }
          if (stillPending == 0U) {
            break;
          }
          if (!lowLatencyActive) {
            const std::uint32_t nextFutex =
                m_control.futexWord.load(std::memory_order_relaxed) + 1U;
            m_control.futexWord.store(nextFutex, std::memory_order_release);
            (void)detail::futexWakePrivate(&m_control.futexWord, 2);
          }
          detail::cpuRelax();
          mask = m_coldStampedMask.load(std::memory_order_acquire);
        }
      }
      if (reuseSafe) {
        // Same-command reuse: workers already cached desc fields on the prior
        // full publish. Skip the mailboxDesc store entirely; just bump mailbox
        // phase with kReuseBit set.
        for (std::size_t slot = 1; slot < participantsLocal; ++slot) {
          auto *w = workersBaseForPublish + slot;
          w->mailbox.store(publishedMb, std::memory_order_release);
        }
      } else {
        for (std::size_t slot = 1; slot < participantsLocal; ++slot) {
          auto *w = workersBaseForPublish + slot;
          if (w->mailboxDesc != descRaw) {
            w->mailboxDesc = descRaw;
          }
          w->mailbox.store(publishedMb, std::memory_order_release);
        }
        // Record the descriptor whose fields the workers' caches are now primed
        // with so the next dispatch's pool-level invalidation check can compare
        // against it.
        m_lastPublishedDesc.store(&desc, std::memory_order_relaxed);
      }
    }

    // Pre-wake hot-completion probe: when the descriptor's primitive is a
    // simple one-shot (parallelFor / parallelReduce / bulkForQueries), every
    // spinning background worker can observe the new mailbox, run a trivial
    // body, and stamp `doneEpoch == nextGen` in a handful of nanoseconds. If
    // all background workers have already stamped done by the time the producer
    // is about to bump `futexWord`, the syscall is unnecessary because there
    // are no parked workers to wake. The probe is bounded to a small number of
    // acquire-load sweeps so a probe miss adds at most a few hundred
    // nanoseconds before falling through to the unconditional wake. Skipping
    // the futex word bump on probe success is safe: a worker that parks between
    // this dispatch and the next will observe a stale `futexWord` value, but
    // the next dispatch's probe fails (parked worker has not stamped the next
    // epoch) and the unconditional wake bumps the word, releasing the parked
    // worker.
    bool preWakeAllDone = false;
    // Worker preserves all flag bits from `publishedMb` when stamping DONE on
    // its mailbox, so the producer's join waits for `publishedMb | kDoneBit`
    // (which already includes kReuseBit when set, matching the worker's stamp).
    const std::uint64_t doneSentinel = doneSentinelMb;
    if (!lowLatencyActive && desc.preWakeCompletionProbe &&
        participantsLocal > 1U) {
      auto *workersBase = m_workers.get();
      constexpr std::uint32_t kPreWakeProbeRounds = 4U;
      std::uint64_t pendingProbe = m_control.pendingMaskBits;
      for (std::uint32_t r = 0; r < kPreWakeProbeRounds; ++r) {
        std::uint64_t scan = pendingProbe;
        while (scan != 0U) {
          const auto bit = detail::ctzll(scan);
          scan &= scan - 1U;
          if (((workersBase + bit)->mailbox.load(std::memory_order_acquire) &
               ~detail::PoolControl::kAckedBit) == doneSentinel) {
            pendingProbe &= ~(std::uint64_t{1} << bit);
          }
        }
        if (pendingProbe == 0U) {
          preWakeAllDone = true;
          break;
        }
        detail::cpuRelax();
      }
    }

    if (!lowLatencyActive && !preWakeAllDone) {
      // Wake unconditionally. A prior TSC-based "hot cadence" skip
      // assumed a worker could not have parked since the last
      // dispatch, but the invariant TSC advances while a thread is
      // descheduled, so a preempted worker accrues delta past the spin
      // budget and parks; the skipped wake then hangs the join.
      // `LowLatencyGuard` still covers genuinely-hot dispatch through
      // the outer `!lowLatencyActive` gate.
      //
      // `m_dispatchMutex` makes the producer the only writer on
      // `futexWord`, so a non-atomic load plus release store suffices.
      const std::uint32_t nextFutex =
          m_control.futexWord.load(std::memory_order_relaxed) + 1U;
      m_control.futexWord.store(nextFutex, std::memory_order_release);
      // Chain-wake-2: producer wakes only the first two parked workers;
      // each woken worker fires futex_wake(N=2) on its post-park branch
      // (see `workerMainLoop`), doubling the chain at logarithmic depth.
      (void)detail::futexWakePrivate(&m_control.futexWord, 2);
      m_recentDispatchTsc = detail::readTsc();
    }

    // Producer participates as slot 0. Install the slot-0 TLS context once for
    // the inline body so a nested CPO call can detect the same-pool case via
    // the standard `tlsContext()` probe. The RAII guard restores TLS on every
    // exit path including exceptional unwind from the body, so a throwing user
    // closure cannot leave the producer's TLS pointing at this pool.
    {
      const TlsContextScope tlsGuard(tlsContext(), this, m_kind, m_arenaIndex,
                                     /*slot=*/0);
      if constexpr (std::is_same_v<Slot0Body, std::nullptr_t>) {
        // Untyped path: call through `desc.body`'s `FunctionRef` (callers that
        // don't have the body's type at this site, e.g. forkJoin's nested-state
        // dispatch).
        detail::runActiveJobStatic<BalanceV>(desc, 0U);
      } else if constexpr (BalanceV == Balance::StaticUniform) {
        // Typed path: bypass the indirect call by invoking `*slot0` directly.
        // Workers still see `desc.body` (FunctionRef) and run unchanged.
        if constexpr (ContiguousRank) {
          static_assert(!std::is_void_v<Slot0Hints>);
          detail::runContiguousRankPartitionTyped<Slot0Hints>(desc, 0U, *slot0);
        } else if constexpr (std::is_void_v<Slot0Hints>) {
          detail::runStaticPartitionTyped(desc, 0U, *slot0);
        } else {
          detail::runPartitionTypedHinted<BalanceV, Slot0Hints>(desc, 0U,
                                                                *slot0);
        }
      } else {
        // Typed Dynamic slot-0: same monomorphization as the Static fast path,
        // but with the shared atomic counter as the block-claim mechanism.
        // Avoids the FunctionRef indirect call cost on the producer's slot-0
        // body. After dropping Balance::Steal / Balance::Recursive (typed paths
        // only support StaticUniform + DynamicChunked) this is the
        // unconditional typed-Dynamic branch.
        static_assert(
            BalanceV == Balance::DynamicChunked,
            "typed slot-0 body only valid for StaticUniform / DynamicChunked");
        if constexpr (std::is_void_v<Slot0Hints>) {
          detail::runDynamicCounterTyped(desc, 0U, *slot0);
        } else {
          detail::runPartitionTypedHinted<BalanceV, Slot0Hints>(desc, 0U,
                                                                *slot0);
        }
      }
    }

    // Join: sweep every pending background worker's done-epoch each round,
    // count quiet rounds where no slot advanced, and yield to the scheduler
    // after a bounded number of quiet rounds. Yielding matters at
    // j=participants where the producer can displace a pinned worker on the
    // same CPU; spinning-only on a single slot's done line then prevents that
    // worker from ever stamping its epoch.
    //
    // Acquire load on doneEpoch is preserved -- it pairs with the worker's
    // release store and is the visibility edge that lets the producer observe
    // writes the worker performed inside its body.
    //
    // Reuse `participantsLocal` from the pre-wake probe block above; the field
    // is non-atomic and constant for the pool's lifetime, so re-reading it
    // would just be a redundant load.
    const std::size_t participants = participantsLocal;
    if (participants > 1) {
      auto *workersBase = m_workers.get();
      // Yield-only-on-CPU-collision: if a pending worker is pinned to the same
      // CPU the producer is currently running on, pure spinning cannot make
      // progress because only that worker can write its own done-epoch line.
      // Detect via `sched_getcpu` and yield to the scheduler. The probe is
      // gated by quietRounds to amortize the syscall.
      //
      // Cache `sched_getcpu()` once per join. The producer is auto-pinned by
      // the pool ctor for Standalone pools and by `bindProducerSlot()` for
      // explicit-pin call sites; in both shapes the CPU is invariant for the
      // call's lifetime. Pulling the syscall out of the per-64-rounds probe
      // saves ~10-20ns per gated probe under sustained join contention.
#ifdef __linux__
      const int producerCpu = sched_getcpu();
      const std::uint32_t producerCpuU =
          producerCpu < 0 ? UINT32_MAX
                          : static_cast<std::uint32_t>(producerCpu);
#elif defined(_WIN32)
      // `GetCurrentProcessorNumber` is the Windows peer of `sched_getcpu`:
      // a single instruction read from the local APIC, no syscall.
      // Cached once per join so the per-64-rounds collision probe does
      // not pay the read each time.
      const std::uint32_t producerCpuU =
          static_cast<std::uint32_t>(::GetCurrentProcessorNumber());
#endif
      const auto pendingPinnedToCurrentCpu = [workersBase
#if defined(__linux__) || defined(_WIN32)
                                              ,
                                              producerCpuU
#endif
      ](std::uint64_t pending) noexcept {
#if defined(__linux__) || defined(_WIN32)
        if (producerCpuU == UINT32_MAX) {
          return false;
        }
        std::uint64_t scan = pending;
        while (scan != 0U) {
          const auto bit = detail::ctzll(scan);
          scan &= scan - 1U;
          if ((workersBase + bit)->cpuId == producerCpuU) {
            return true;
          }
        }
        return false;
#else
        (void)workersBase;
        (void)pending;
        return false;
#endif
      };
      if (participants <= 64U) {
        // Pending bitmask is precomputed at construction on `PoolControl`'s
        // `participants` line; slot 0 already cleared since the producer
        // participates inline. The join spins on each pending slot in turn:
        // parallel observation across slots was the bitmask scan's only
        // theoretical advantage, but for uniform workloads the total wait time
        // equals `max(slot stamp time)` either way, and for the single-worker
        // fanout the inner scan is a single-bit degenerate. Per-slot spin
        // removes the `ctzll/and/cmovne/rol` chain and the redundant outer
        // `pending == before` tracking on every PAUSE round.
        std::uint64_t pending = m_control.pendingMaskBits;
        std::uint32_t quietRounds = 0;
        constexpr std::uint32_t kJoinQuietRoundsBeforeYield = 4096;
        // Tight-first-N spin per slot: after producer publishes, hot workers
        // often stamp DONE before the first PAUSE would help. Probe with a
        // small bounded load+compare window first; if the stamp is not visible,
        // fall back to PAUSE-spin for slower arrivals.
        constexpr std::uint32_t kTightProbeIters = 8U;
        while (pending != 0U) {
          const auto bit = detail::ctzll(pending);
          auto &w = *(workersBase + bit);
          bool tightDone = false;
          for (std::uint32_t i = 0; i < kTightProbeIters; ++i) {
            // Mask out `kAckedBit` so a worker's CAS-success stamp (which
            // carries `kAckedBit` on cold-collapse-eligible dispatches) is
            // recognized as `doneSentinel` here.
            if ((w.mailbox.load(std::memory_order_acquire) &
                 ~detail::PoolControl::kAckedBit) == doneSentinel) {
              tightDone = true;
              break;
            }
          }
          // Cold-collapse fallback: when the worker has not stamped within the
          // tight-probe window and the dispatch opted in (parallelFor sets
          // `workerStateBase`), the worker may still be parked or delayed. Race
          // the worker on `claimedAt`: if the producer wins, run the rank's
          // blocks while the worker is still catching up. The CAS itself is one
          // atomic op on a private cache line; on the hot path (tightDone=true)
          // this branch is never entered, so steady-state dispatch is
          // unchanged.
          //
          // Install the stolen-rank TLS context across the body call so a
          // nested same-pool CPO call from inside the user closure detects the
          // same-pool case via `tlsContext()`. Primitives with nested same-pool
          // support take that path; the rest fall through to inline instead of
          // re-entering `dispatchOneStatic` and self-deadlocking on the
          // non-recursive `m_dispatchMutex`. The RAII guard restores TLS on
          // every exit path including exceptional unwind.
          if (!tightDone && !skipColdCollapse &&
              desc.workerStateBase != nullptr &&
              detail::tryClaimRank(w, nextGen)) {
            if constexpr (!std::is_same_v<Slot0Body, std::nullptr_t> &&
                          BalanceV == Balance::StaticUniform) {
              // Static typed path runs rank `bit`'s strided blocks via the
              // monomorphized typed runner; under the cold-collapse race the
              // worker's own entry has not yet started and the strided blocks
              // are still pending.
              const TlsContextScope tlsGuard(tlsContext(), this, m_kind,
                                             m_arenaIndex,
                                             /*slot=*/bit);
              if constexpr (ContiguousRank) {
                static_assert(!std::is_void_v<Slot0Hints>);
                detail::runContiguousRankPartitionTyped<Slot0Hints>(desc, bit,
                                                                    *slot0);
              } else if constexpr (std::is_void_v<Slot0Hints>) {
                detail::runStaticPartitionTyped(desc, bit, *slot0);
              } else {
                detail::runPartitionTypedHinted<BalanceV, Slot0Hints>(desc, bit,
                                                                      *slot0);
              }
            } else if constexpr (BalanceV == Balance::DynamicChunked) {
              // Dynamic path: rank `bit`'s Phase A block is still pending
              // because the worker has not entered its typed entry. Run it
              // inline. Prefer the typed `*slot0` callable when the
              // dispatcher passed one (typed entry); fall back to
              // `desc.body`'s `FunctionRef` for the untyped entry.
              if (bit < desc.blockCount) {
                const TlsContextScope tlsGuard(tlsContext(), this, m_kind,
                                               m_arenaIndex,
                                               /*slot=*/bit);
                if constexpr (!std::is_same_v<Slot0Body, std::nullptr_t> &&
                              !std::is_void_v<Slot0Hints>) {
                  detail::runPartitionTypedHinted<BalanceV, Slot0Hints>(
                      desc, bit, *slot0);
                } else {
                  const std::size_t lo = desc.first + (bit * desc.chunk);
                  const std::size_t hi = std::min(lo + desc.chunk, desc.last);
                  if (lo < hi) {
                    if constexpr (!std::is_same_v<Slot0Body, std::nullptr_t>) {
                      (*slot0)(lo, hi);
                    } else {
                      desc.body(lo, hi);
                    }
                  }
                }
              }
            }
            // The CAS preserves a worker's `kAckedBit` if the worker raced
            // ahead of us (keeps the wait protocol's signal intact). On the
            // reuse-fast-path we never consult the mask, so skip the OR;
            // workers do not read prior-dispatch desc fields on reuse.
            std::uint64_t expectedMb = publishedMb;
            const bool stamped = w.mailbox.compare_exchange_strong(
                expectedMb, doneSentinel, std::memory_order_release,
                std::memory_order_acquire);
            if (stamped && !reuseSafe) {
              m_coldStampedMask.fetch_or(1ULL << bit,
                                         std::memory_order_relaxed);
            }
            tightDone = true;
          }
          while (!tightDone &&
                 (w.mailbox.load(std::memory_order_acquire) &
                  ~detail::PoolControl::kAckedBit) != doneSentinel) {
            detail::cpuRelax();
            // Quiet-rounds bookkeeping is dead weight under low-latency scope:
            // workers are hot-spinning so the CPU-collision yield is the wrong
            // remedy, and the iteration cap's yield never triggers within the
            // dispatch cadence either.
            if (!lowLatencyActive) {
              ++quietRounds;
              // Probe the CPU-collision predicate every 64 quiet rounds:
              // amortizes the `sched_getcpu` syscall while still yielding
              // promptly when the producer is sharing a CPU with a pending
              // pinned worker. Two independent yield triggers: (a) every 64th
              // quiet round, yield if a pending worker is pinned to the
              // producer's current CPU (collision); (b) after
              // `kJoinQuietRoundsBeforeYield` total quiet rounds, yield
              // unconditionally as the timeout fallback. Either trigger resets
              // the counter.
              const bool collisionYield = (quietRounds & 0x3FU) == 0U &&
                                          pendingPinnedToCurrentCpu(pending);
              const bool timeoutYield =
                  quietRounds >= kJoinQuietRoundsBeforeYield;
              if (collisionYield || timeoutYield) {
                quietRounds = 0;
                std::this_thread::yield();
              }
            }
          }
          pending &= pending - 1U;
          if (!lowLatencyActive) {
            quietRounds = 0;
          }
        }
      } else {
        for (std::size_t slot = 1; slot < participants; ++slot) {
          auto &w = *(workersBase + slot);
          while (true) {
            const std::uint64_t mb = w.mailbox.load(std::memory_order_acquire);
            if ((mb & ~detail::PoolControl::kAckedBit) == doneSentinel) {
              break;
            }
            detail::cpuRelax();
          }
        }
      }
    }
    // The active-job slot is overwritten by the next dispatch's release-store
    // and cleared by shutdown before the shutdown bit is set. Between
    // dispatches it may carry the previous descriptor pointer; workers only
    // consult it after observing the matching `generation`.

    // The dispatch gate is released by the caller's `DispatchLease` (or by
    // the wrapping `dispatchOneStatic`'s own lease) when the scope exits,
    // including on the rethrow path below: the lease's destructor runs as
    // the exception unwinds, so a concurrent producer can begin its own
    // dispatch as soon as the unwinder leaves this frame.

    // Rethrow the first captured exception, if any. Elide the load when the
    // slot-0 callable is statically known to be nothrow_invocable AND the typed
    // worker runner is in use; in that case neither slot-0 nor workers can
    // populate `desc.firstException`, so the load and rethrow path are dead.
    // Untyped dispatch (Slot0Body == nullptr_t) keeps the load because the
    // worker side falls back to the untyped runStaticPartition which uses
    // desc.body whose throw-ness cannot be determined at this site.
    constexpr bool kSlot0Nothrow =
        !std::is_same_v<Slot0Body, std::nullptr_t> &&
        std::is_nothrow_invocable_v<Slot0Body &, std::size_t, std::size_t>;
    if constexpr (!kSlot0Nothrow) {
      rethrowIfCaptured(desc);
    }
  }

  /// Common shutdown sequence shared by `~ThreadPool` and the constructor's
  /// failure path. Idempotent; calling twice is a no-op because the shutdown
  /// bit is already set on the second pass.
  void shutdownAndJoin() noexcept {
    if (m_shutdownComplete) {
      return;
    }
    m_shutdownComplete = true;

    // Clear `activeJob` BEFORE setting the shutdown bit so worker's
    // `shouldExit` (which checks `activeJob == nullptr` once it observes the
    // shutdown bit on `generation`) sees a coherent pair: shutdown bit set +
    // activeJob nullptr means "no more work, exit cleanly". The dispatch hot
    // path no longer clears `activeJob` per-dispatch (the next dispatch
    // overwrites it via release-store), so the slot may carry the last
    // published descriptor pointer until shutdown -- valid only as a value the
    // worker compares against `nullptr`, never dereferenced.
    m_control.activeJob.store(nullptr, std::memory_order_release);
    m_control.generation.fetch_or(detail::PoolControl::kShutdownBit,
                                  std::memory_order_release);
    // Stamp each background worker's mailbox with the shutdown sentinel so
    // spinning workers (which read their own mailbox, not
    // `m_control.generation`) observe shutdown without having to load the
    // shared control line on every spin iteration.
    {
      auto *workersBase = m_workers.get();
      const std::size_t participantsLocal = m_control.participants;
      for (std::size_t slot = 1; slot < participantsLocal; ++slot) {
        auto *w = workersBase + slot;
        const std::uint64_t prev = w->mailbox.load(std::memory_order_relaxed);
        const std::uint64_t flagBits =
            prev & (detail::PoolControl::kPhaseStep - 1);
        const std::uint64_t shutdownMb =
            ((prev + detail::PoolControl::kPhaseStep) &
             ~(detail::PoolControl::kPhaseStep - 1)) |
            flagBits | detail::PoolControl::kShutdownBit;
        w->mailbox.store(shutdownMb, std::memory_order_release);
      }
    }
    // Shutdown is single-writer (the destructor) by lifetime contract -- no
    // concurrent dispatch races shutdown -- so a load + release-store is enough
    // to bump the parking token.
    {
      const std::uint32_t nextFutex =
          m_control.futexWord.load(std::memory_order_relaxed) + 1U;
      m_control.futexWord.store(nextFutex, std::memory_order_release);
    }
    (void)detail::futexWakePrivate(&m_control.futexWord, INT_MAX);

#ifdef __linux__
    for (const pthread_t &th : m_workerThreads) {
      (void)pthread_join(th, nullptr);
    }
    m_workerThreads.clear();
    // Pair the constructor's best-effort mlock so we don't leak locked pages
    // past pool lifetime when the WorkerArrayDeleter then frees the block.
    // munlock failure is non-fatal (mlock may have failed too, in which case
    // there's nothing to undo).
    if (m_workers != nullptr) {
      (void)munlock(m_workers.get(),
                    sizeof(detail::WorkerState) * m_control.participants);
    }
#else
    for (auto &th : m_fallbackThreads) {
      if (th.joinable()) {
        th.join();
      }
    }
    m_fallbackThreads.clear();
#endif
    m_workerSpawnArgs.clear();
  }

  /// Custom deleter that destroys each `WorkerState` in place and frees the
  /// aligned heap block.
  struct WorkerArrayDeleter {
    /// Number of `WorkerState` instances stored in the block; supplied by
    /// the constructor.
    std::size_t count = 0;

    // Destroy and deallocate the worker block previously created via
    // aligned `operator new`. |ptr| may be `nullptr`.
    void operator()(detail::WorkerState *ptr) const noexcept {
      if (ptr == nullptr) {
        return;
      }
      for (std::size_t i = count; i > 0; --i) {
        (ptr + (i - 1))->~WorkerState();
      }
      ::operator delete(static_cast<void *>(ptr), std::align_val_t{kCacheLine});
    }
  };

  /// Custom deleter that destroys each `ChainDoneSlot` in place and frees
  /// the aligned heap block.
  struct ChainDoneSlotDeleter {
    /// Number of slots stored in the block; supplied by the constructor.
    std::size_t count = 0;

    // Destroy and deallocate the slot block previously created via aligned
    // `operator new`. |ptr| may be `nullptr`.
    void operator()(detail::ChainDoneSlot *ptr) const noexcept {
      if (ptr == nullptr) {
        return;
      }
      for (std::size_t i = count; i > 0; --i) {
        (ptr + (i - 1))->~ChainDoneSlot();
      }
      ::operator delete(static_cast<void *>(ptr), std::align_val_t{kCacheLine});
    }
  };

  /// Deleter for the pool-owned `PlexDoneSlot` block; mirrors
  /// `ChainDoneSlotDeleter`.
  struct PlexDoneSlotDeleter {
    /// Number of slots the deleter must destroy in reverse order before
    /// freeing the cache-line-aligned backing allocation.
    std::size_t count = 0;
    void operator()(detail::PlexDoneSlot *ptr) const noexcept {
      if (ptr == nullptr) {
        return;
      }
      for (std::size_t i = count; i > 0; --i) {
        (ptr + (i - 1))->~PlexDoneSlot();
      }
      ::operator delete(static_cast<void *>(ptr), std::align_val_t{kCacheLine});
    }
  };

  /// Worker-affinity policy chosen at construction. Drives the pool ctor's
  /// `pthread_attr_setaffinity_np` mask -- single-CPU mask for `PerCpu`,
  /// CCD's full CPU set for `PerCluster`, no pin for `None`. Declared here
  /// so its initializer-list position matches the ctor's leading-field
  /// initializer.
  Affinity m_workerAffinity = Affinity::PerCpu;

  /// Origin tag: distinguishes user-owned pools from `PoolGroup`-owned
  /// arenas.
  PoolKind m_kind = PoolKind::Standalone;

  /// Zero-based arena index inside the owning `PoolGroup`; `0` for
  /// `Standalone` pools.
  std::uint32_t m_arenaIndex = 0;

  /// Pool topology snapshot; immutable after construction.
  detail::Topology m_topology;

  /// Shared control block; cache-line aligned for false-sharing avoidance.
  detail::PoolControl m_control{};

  /// Diagnostic counters incremented at the dispatch / inline-fallback /
  /// park / wake / dynamic counter / cancellation sites. Read via
  /// `snapshotCounters()`. Lives on its own cache line so the relaxed RMWs
  /// never bounce with `m_control`'s contended atomics.
  detail::PoolCounters m_counters{};

  /// Owning pointer to the aligned worker-state block; the deleter destroys
  /// each element.
  std::unique_ptr<detail::WorkerState, WorkerArrayDeleter> m_workers;

  /// Pool-owned per-worker `done` slot block reused by every `parallelChain`
  /// and `parallelScan` dispatch. Allocated once at construction time, sized
  /// `participants()`. Each call reserves a fresh interval in
  /// `m_chainEpochBase` and stamps absolute targets during the dispatch; the
  /// block is never zero-reset between calls because the per-call interval
  /// is disjoint from every prior call's stamps. Reusing the block keeps the
  /// hot dispatch path off `operator new` / `operator delete`.
  std::unique_ptr<detail::ChainDoneSlot, ChainDoneSlotDeleter> m_chainDoneSlots;

  /// Pool-owned per-worker `done` slot block reused by every `runPlex`
  /// dispatch. Allocated once at construction time, sized `participants()`.
  /// Each plex call reserves a fresh interval in `m_plexEpochBase` and
  /// stamps absolute targets; the block is never zero-reset between calls.
  std::unique_ptr<detail::PlexDoneSlot, PlexDoneSlotDeleter> m_plexDoneSlots;

  /// Monotonic epoch counter for plex done-slot stamps; advanced under the
  /// dispatch gate by `nPhases + 1` per call so successive calls reserve
  /// disjoint intervals.
  std::uint64_t m_plexEpochBase = 0;

  /// Monotonically-advancing epoch counter for chain and scan done-slot
  /// stamps. Each `parallelChain` / `parallelScan` call captures the
  /// current value as its `state.epochBase` under the dispatch gate, then
  /// advances by `nStages + 1` (chain) or `3` (scan) so successive calls
  /// reserve disjoint intervals. Workers stamp `done = epochBase +
  /// relative_target` and peers wait on `done >= epochBase + target`;
  /// prior-dispatch stamps cannot satisfy a current wait because they are
  /// strictly less than the new base. Plain integral because every mutation
  /// happens under the dispatch gate that already serializes chain / scan
  /// dispatches.
  std::uint64_t m_chainEpochBase = 0;

  /// TSC of the most recent dispatch's publish window, written by
  /// `dispatchOneStaticLockedBody` under `m_dispatchMutex` (single-writer).
  /// Used as a hot-cadence predicate to skip the `futex_wake` syscall when
  /// the previous dispatch was within a worker's spin-budget ago: in that
  /// window no worker can have parked, so the kernel transit would find
  /// zero waiters. Plain integral because every mutation happens under the
  /// dispatch gate. Read by the same writer path in immediate succession,
  /// so torn-read concerns do not apply.
  std::uint64_t m_recentDispatchTsc = 0;

  /// Per-worker Chase-Lev work-stealing deques. One deque per participant;
  /// allocated at construction time so the `forkJoin` steal probe never
  /// pays an allocator round-trip on its hot path. Each deque is owner-only
  /// on its matching slot's worker thread (the producer for slot 0); any
  /// thread may invoke `detail::ChaseLevDeque::steal` on a victim deque.
  std::vector<std::unique_ptr<detail::ChaseLevDeque<detail::Task *>>>
      m_workerDeques;

  /// Per-slot CCD index, cached for fast lookup by `forkJoin`'s
  /// victim-selection probe. Sized `participants()`; populated at
  /// construction time from each worker's `ccdId`. The victim-selection RNG
  /// reads this array (not `WorkerState`) so the steal probe stays off the
  /// worker's hot identity line.
  std::vector<std::uint32_t> m_ccdOfSlot;

  /// One-time coherence ping-pong probe result, run at pool construction on
  /// multi-CCD topologies. Primitives that benefit from CCD-aware
  /// partitioning (currently `parallelScan`) read
  /// `m_coherenceProbe.crossOverIntraRatio` to derive their cross-CCD work
  /// share without any hardware-specific constants in the engine. Default
  /// value (`valid == false`, ratio == 1.0) on single-CCD pools and on
  /// arenas owned by a `PoolGroup`.
  detail::CoherenceProbe m_coherenceProbe;

  /// Pre-resolved `parallelScan` topology, computed once at pool ctor (after
  /// the coherence probe lands) from invariant pool inputs (`m_ccdOfSlot`,
  /// `m_workers[i].cpuId`, `m_coherenceProbe.matrix.cpus`,
  /// `m_coherenceProbe.clusters.clusterIdOfCpuIndex`,
  /// `m_coherenceProbe.maxCrossOverIntraRatio`). The scan path reads these
  /// fields directly instead of recomputing the slot-to-cluster mapping +
  /// contiguity check + asymmetric-bias derivation on every call.
  ///
  /// `m_scanClusterIdOfSlot[s]` is the cluster id for slot `s` (per the
  /// probe's clustering or zero when the probe is invalid).
  /// `m_scanClusterFirstSlot[k]` and `m_scanClusterSlotCount[k]` are the
  /// contiguous slot range belonging to cluster `k`; only valid when
  /// `m_scanUseHierarchical` is true. `m_scanAsymmetricNum` is the
  /// producer-CCD volume fraction (out of 16) used by `slotRange`'s
  /// asymmetric path.
  std::vector<std::uint32_t> m_scanClusterIdOfSlot;
  /// First slot in each cluster's contiguous range. Only meaningful when
  /// `m_scanUseHierarchical` is true.
  std::vector<std::uint32_t> m_scanClusterFirstSlot;
  /// Slot count for each cluster's contiguous range. Companion to
  /// `m_scanClusterFirstSlot`.
  std::vector<std::uint32_t> m_scanClusterSlotCount;
  /// Number of distinct clusters discovered by the coherence probe; zero
  /// when the hierarchical path is disabled.
  std::uint32_t m_scanNumClusters = 0;
  /// True when the scan engine is allowed to take the hierarchical
  /// per-cluster reduce path. Set when the probe found at least two
  /// non-empty contiguous clusters and slot 0 lives in cluster 0.
  bool m_scanUseHierarchical = false;
  /// Producer-CCD volume fraction (out of 16) used by `slotRange`'s
  /// asymmetric partition. Defaults to half.
  std::uint32_t m_scanAsymmetricNum = 8;
  /// CCD index that owns slot 0; `UINT32_MAX` until set.
  std::uint32_t m_scanProducerCcd = UINT32_MAX;
  /// Number of slots whose `ccdOfSlot` equals `m_scanProducerCcd`.
  std::uint32_t m_scanSlotsOnProducerCcd = 0;

  /// Cached background-worker CPU ids for `bindProducerSlot`'s worker-CPU
  /// exclusion logic. Populated once at construction so the producer-side
  /// affinity guard never allocates.
  std::vector<std::uint32_t> m_workerCpus;

  /// Precomputed same-CCD victim list for each slot. Indexed by self-slot;
  /// each row contains every other slot whose `ccdOfSlot` matches. The
  /// fork-join steal probe iterates this list directly when CCD-local
  /// affinity is requested, skipping the per-step `% participants` modulo
  /// and the per-step CCD comparison.
  std::vector<std::vector<std::uint32_t>> m_sameCcdVictims;

  /// Precomputed cross-CCD victim list for each slot (used as the CCD-local
  /// fallback ring). Indexed by self-slot; each row contains every other
  /// slot whose `ccdOfSlot` differs from the self slot's CCD. Pairs with
  /// `m_sameCcdVictims` so a CCD-local steal probe scans same-CCD victims,
  /// then falls back to cross-CCD victims without re-probing the same-CCD
  /// set.
  std::vector<std::vector<std::uint32_t>> m_crossCcdVictims;

  /// Precomputed all-victim list for each slot (used when CCD affinity is
  /// not requested). Indexed by self-slot; each row contains every other
  /// slot in monotonic order. The steal probe wraps around this row using
  /// a precomputed start index instead of `(start + step) % participants`.
  std::vector<std::vector<std::uint32_t>> m_allVictims;

#ifdef __linux__
  /// Background pthreads (Linux). Sized `participants() - 1`.
  std::vector<pthread_t> m_workerThreads;
#else
  /// Background `std::thread` instances (non-Linux fallback). Sized
  /// `participants() - 1`.
  std::vector<std::thread> m_fallbackThreads;
#endif

  /// Persistent storage for the per-worker spawn arguments handed to the
  /// pthread entry function.
  std::vector<WorkerSpawnArg> m_workerSpawnArgs;

  /// Count of workers that have completed startup (affinity bind + TLS +
  /// ready to enter `workerMainLoop`). The constructor waits on this so
  /// the first dispatch never races worker startup; without the barrier,
  /// first-dispatch latency includes the slowest worker's
  /// time-to-reach-loop.
  alignas(kCacheLine) std::atomic<std::uint32_t> m_workersReady{0};

  /// Whether `shutdownAndJoin` has already run; protects against
  /// double-join in the dtor.
  bool m_shutdownComplete = false;

  /// In-flight detached-task counter incremented by `submitDetached` and
  /// decremented by the trampoline. The destructor blocks until this
  /// reaches zero.
  std::atomic<std::uint64_t> m_detachedInFlight{0};

  /// Mutex serializing the detached-throw slot, the dtor's wait predicate,
  /// and the spawn rollback path. Also used by `lastDetachedException` for
  /// race-free reads.
  mutable std::mutex m_detachedMutex;

  /// Condition variable signalled when an in-flight detached task
  /// decrements the counter. The destructor's wait predicate observes the
  /// counter going to zero.
  std::condition_variable m_detachedDone;

  /// Latched on the first detached-task throw; subsequent throws drop.
  /// Read out via `lastDetachedException`; read takes `m_detachedMutex`
  /// for race freedom.
  std::exception_ptr m_detachedException;

  /// Gate serialising concurrent `dispatchOne` callers through the priority
  /// lanes. Held for one primitive's publish/participate/join cycle so
  /// producers from different threads do not interleave dispatches against
  /// the same worker pool. Single-producer call sites pay one uncontended
  /// lock/unlock pair on the dispatch hot path. Linux uses `std::mutex`
  /// directly; the Windows branch substitutes a futex-backed hybrid lock.
#ifdef _WIN32
  /// Windows lock word (atomic). See `dispatchGateLock` for the bit
  /// layout and the post-park has-waiter-preservation invariant.
  std::atomic<std::uint32_t> m_dispatchMutex{0U};
#else
  /// Linux dispatch mutex (glibc futex pair).
  std::mutex m_dispatchMutex;
#endif

  /// Pointer to the descriptor whose fields the workers' TLS caches are
  /// currently primed with. Updated under `m_dispatchMutex` on every full
  /// (non-reuse) publish.
  ///
  /// The same-command reuse fast path must only fire when the workers'
  /// cached job parameters match the producer's TLS descriptor. The
  /// producer's `keyMatches` check (field-by-field equality against its
  /// own thread-local `desc`) does not see whether a different thread or a
  /// different pool intervened between two of this producer's calls. A
  /// second pointer compare against this pool-level "last full publish"
  /// pointer closes that window: reuse is only valid when the producer's
  /// descriptor IS the one the workers most recently cached. Mutated only
  /// under the dispatch gate, so a relaxed atomic load is sufficient on
  /// the producer's pre-lease read.
  std::atomic<const detail::JobDescriptor *> m_lastPublishedDesc{nullptr};

  /// Bitmask of worker slots the producer self-stamped on a prior
  /// !reuseSafe cold-collapse. Drained at next-dispatch entry via
  /// `drainColdStampedAcks` before the producer writes desc fields. Atomic
  /// so the drain can run outside `m_dispatchMutex`; `fetch_or` on set,
  /// `fetch_and` on clear so a concurrent stamp cannot be lost.
  std::atomic<std::uint64_t> m_coldStampedMask{0};

  /// Number of `Priority::Latency` callers currently waiting on the
  /// dispatch gate. Throughput and Background callers spin on this until
  /// it drops to zero before acquiring `m_dispatchMutex`, giving latency
  /// callers first access to the lock.
  std::atomic<std::uint32_t> m_latencyWaiting{0};

  /// Number of `Priority::Throughput` callers currently waiting on the
  /// dispatch gate. Background callers wait on both this and
  /// `m_latencyWaiting` to drain before locking, so any concurrent
  /// throughput producer reaches the workers first. Background may be
  /// reordered behind any number of higher-priority dispatches.
  std::atomic<std::uint32_t> m_throughputWaiting{0};
};

} // namespace citor

// ===== citor/coro.h =====

// Coroutine wrappers for citor's synchronous primitives. Opt-in
// header: each wrapper returns an awaitable that runs the body on a
// per-pool driver thread and resumes the coroutine when the body
// returns.



namespace citor::coro {

namespace detail {

/// Holds the result of a primitive wrapped as a coroutine.
template <class T>
struct ResultStorage {
  /// Populated when the wrapped primitive returns.
  std::optional<T> value;
  /// Move-construct the stored value in place.
  template <class U>
  void set(U &&v) {
    value.emplace(std::forward<U>(v));
  }
  /// Move the stored value out for return to the awaiting coroutine.
  T take() { return std::move(*value); }
};

/// Void specialisation; carries no payload.
template <>
struct ResultStorage<void> {
  /// No-op for symmetry with the value specialisation.
  void set() {}
  /// No-op for symmetry with the value specialisation.
  void take() {}
};

/// Forward declaration for `Task<T>::promise_type`.
template <class T>
class Promise;

} // namespace detail

/// Coroutine handle wrapper used as the return type of every wrapper. Eager
/// suspension at `initial_suspend` lets the awaiter set its continuation
/// before the first body executes.
template <class T = void>
class Task {
public:
  /// Required by the coroutine machinery.
  using promise_type = detail::Promise<T>;
  /// Convenience alias for the typed coroutine handle.
  using Handle = std::coroutine_handle<promise_type>;

  /// Default-constructed task carries no handle and is not awaitable.
  Task() = default;
  /// Wrap an existing handle; used by `Promise::get_return_object`.
  explicit Task(Handle h) noexcept : m_handle(h) {}

  /// Non-copyable.
  Task(const Task &) = delete;
  /// Non-copyable.
  Task &operator=(const Task &) = delete;
  /// Steals the handle from |o|.
  Task(Task &&o) noexcept : m_handle(std::exchange(o.m_handle, {})) {}
  /// Destroys this handle (if any) and steals |o|'s.
  Task &operator=(Task &&o) noexcept {
    if (this != &o) {
      if (m_handle) {
        m_handle.destroy();
      }
      m_handle = std::exchange(o.m_handle, {});
    }
    return *this;
  }
  /// Destroys the owned coroutine handle if still present.
  ~Task() {
    if (m_handle) {
      m_handle.destroy();
    }
  }

  /// Read the underlying coroutine handle without transferring ownership.
  [[nodiscard]] Handle handle() const noexcept { return m_handle; }
  /// Release ownership of the handle to the caller.
  [[nodiscard]] Handle release() noexcept {
    return std::exchange(m_handle, {});
  }

  /// Per-task awaiter returned by `operator co_await`.
  struct Awaiter {
    /// Inner task's handle.
    Handle inner;
    /// True when the inner task has no handle or already ran to completion.
    [[nodiscard]] bool await_ready() const noexcept {
      return !inner || inner.done();
    }
    /// Records the outer coroutine as the continuation and resumes the
    /// inner task via symmetric transfer.
    Handle await_suspend(std::coroutine_handle<> outer) noexcept {
      inner.promise().continuation = outer;
      return inner;
    }
    /// Rethrows the inner exception if any, otherwise returns its value.
    T await_resume() {
      if (inner.promise().exc) {
        std::rethrow_exception(inner.promise().exc);
      }
      return inner.promise().result.take();
    }
  };
  /// Make the task awaitable by another coroutine.
  Awaiter operator co_await() && noexcept { return Awaiter{m_handle}; }

private:
  /// Owned coroutine handle; null after move or default construction.
  Handle m_handle = nullptr;
};

namespace detail {

/// Awaiter returned by every `Promise::final_suspend`; transfers control
/// back to the stored continuation or to a no-op coroutine.
struct FinalAwaiter {
  /// Always suspends so the continuation runs in a clean stack frame.
  [[nodiscard]] static bool await_ready() noexcept { return false; }
  /// Symmetric-transfers into the stored continuation, or no-op if absent.
  template <class P>
  std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) noexcept {
    if (h.promise().continuation) {
      return h.promise().continuation;
    }
    return std::noop_coroutine();
  }
  /// Unreachable; the coroutine is suspended for good at final_suspend.
  static void await_resume() noexcept {}
};

/// Promise type for `Task<T>` with non-void payload.
template <class T>
class Promise {
public:
  /// Captured return value or exception.
  ResultStorage<T> result;
  /// Latched exception from the body if any.
  std::exception_ptr exc;
  /// Continuation handle resumed at final_suspend; set by the inner Awaiter.
  std::coroutine_handle<> continuation;

  /// Coroutine machinery hook.
  Task<T> get_return_object() {
    return Task<T>{std::coroutine_handle<Promise>::from_promise(*this)};
  }
  /// Lazy initial suspend: the coroutine does not run until first awaited.
  static std::suspend_always initial_suspend() noexcept { return {}; }
  /// Transfer to the continuation when the coroutine returns.
  static FinalAwaiter final_suspend() noexcept { return {}; }
  /// Capture the returned value.
  template <class U>
  void return_value(U &&v) {
    result.set(std::forward<U>(v));
  }
  /// Capture an exception thrown from the body.
  void unhandled_exception() { exc = std::current_exception(); }
};

/// Void specialisation of `Promise`.
template <>
class Promise<void> {
public:
  /// Unused payload kept for symmetry with the value specialisation.
  ResultStorage<void> result;
  /// Latched exception from the body if any.
  std::exception_ptr exc;
  /// Continuation handle resumed at final_suspend; set by the inner Awaiter.
  std::coroutine_handle<> continuation;

  /// Coroutine machinery hook.
  Task<void> get_return_object() {
    return Task<void>{std::coroutine_handle<Promise>::from_promise(*this)};
  }
  /// Lazy initial suspend: the coroutine does not run until first awaited.
  static std::suspend_always initial_suspend() noexcept { return {}; }
  /// Transfer to the continuation when the coroutine returns.
  static FinalAwaiter final_suspend() noexcept { return {}; }
  /// No payload to capture.
  void return_void() noexcept {}
  /// Capture an exception thrown from the body.
  void unhandled_exception() { exc = std::current_exception(); }
};

/// Wrapper-destruction handshake for `syncWait`. `state` packs a done
/// bit (set by the wrapper body) and an in-flight worker counter
/// (bumped in `PoolAwaiter::await_suspend`, dropped after `h.resume`
/// returns). `syncWait` destroys the wrapper only when both signal.
struct SyncWaitGate {
  /// High bit of `state`: set when the wrapper coroutine body completes.
  static constexpr std::uint64_t kDoneBit = std::uint64_t{1} << 63;
  /// Low 63 bits of `state`: in-flight worker count.
  static constexpr std::uint64_t kCounterMask = kDoneBit - 1;
  /// Combined state word; the producer waits for `kDoneBit` set and the
  /// counter at zero before destroying the wrapper.
  std::atomic<std::uint64_t> state{0};
};

/// Thread-local pointer to the gate of the syncWait the current thread
/// is participating in. `syncWait` sets it on the producer thread for
/// the initial resume, and every `PoolAwaiter` worker lambda re-sets it
/// for the duration of `h.resume` so that nested `co_await`s constructed
/// on the worker side inherit the same gate. Null when no syncWait is
/// active on this thread.
inline thread_local std::shared_ptr<SyncWaitGate> *tlSyncWaitGate = nullptr;

/// One persistent driver thread per pool. Runs the body and resumes
/// the coroutine for each `co_await`, so the wrapper does not pay a
/// `pthread_create` per call. Constructed lazily, joined at process
/// exit. Holds no pool reference; destroying the pool while the
/// driver still exists is safe as long as no coroutine work is in
/// flight.
class CoroDriver {
public:
  /// Type-erased closure the driver invokes for each enqueued await.
  using Task = std::function<void()>;

  /// Spawns the driver thread.
  CoroDriver() : m_thread([this]() { run(); }) {}

  CoroDriver(const CoroDriver &) = delete;
  CoroDriver &operator=(const CoroDriver &) = delete;

  /// Signals shutdown and joins the driver thread.
  ~CoroDriver() {
    {
      const std::lock_guard<std::mutex> lk(m_mu);
      m_shutdown = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) {
      m_thread.join();
    }
  }

  /// Enqueue |t| for the driver thread to run. Returns immediately.
  void submit(Task t) {
    {
      const std::lock_guard<std::mutex> lk(m_mu);
      m_queue.push_back(std::move(t));
    }
    m_cv.notify_one();
  }

private:
  /// Driver loop: wait for work or shutdown, dequeue, run, repeat.
  void run() {
    while (true) {
      std::unique_lock<std::mutex> lk(m_mu);
      m_cv.wait(lk, [this]() { return m_shutdown || !m_queue.empty(); });
      if (m_queue.empty()) {
        return;
      }
      Task t = std::move(m_queue.front());
      m_queue.pop_front();
      lk.unlock();
      t();
    }
  }

  /// Guards `m_queue` and `m_shutdown`.
  std::mutex m_mu;
  /// Signalled on submit and at shutdown.
  std::condition_variable m_cv;
  /// FIFO of pending body-and-resume closures.
  std::deque<Task> m_queue;
  /// Set by the destructor to wind the driver loop down.
  bool m_shutdown = false;
  /// The driver loop runs here.
  std::thread m_thread;
};

/// Lazy per-pool accessor; the first `co_await` against |pool|
/// spawns its driver, subsequent awaits reuse it.
inline CoroDriver &driverFor(ThreadPool &pool) {
  static std::mutex mu;
  static std::unordered_map<ThreadPool *, std::unique_ptr<CoroDriver>> drivers;
  const std::lock_guard<std::mutex> lk(mu);
  auto it = drivers.find(&pool);
  if (it == drivers.end()) {
    it = drivers.emplace(&pool, std::make_unique<CoroDriver>()).first;
  }
  return *it->second;
}

/// Wraps a synchronous callable so the coroutine awaiting it resumes once
/// the callable returns. Lives in the awaiting coroutine's frame.
template <class Body>
struct PoolAwaiter {
  /// Pool whose driver thread runs the body.
  ThreadPool *pool;
  /// Captured callable executed by the worker.
  Body body;
  /// Deduced return type of the callable.
  using Result = std::invoke_result_t<Body &>;
  /// Captured return value from the body.
  ResultStorage<Result> result;
  /// Latched exception thrown from the body if any.
  std::exception_ptr exc;
  /// Gate inherited from the surrounding `syncWait`, captured at awaiter
  /// construction time. Null outside any `syncWait` scope; the awaiter
  /// then runs without producer-side counter coordination.
  std::shared_ptr<SyncWaitGate> gate{tlSyncWaitGate != nullptr
                                         ? *tlSyncWaitGate
                                         : std::shared_ptr<SyncWaitGate>{}};

  /// Always suspends; the body runs on a worker.
  [[nodiscard]] static bool await_ready() noexcept { return false; }

  /// Enqueues the body on the pool's driver thread and resumes the
  /// coroutine when the body returns.
  void await_suspend(std::coroutine_handle<> h) {
    auto g = gate;
    if (g) {
      g->state.fetch_add(1, std::memory_order_relaxed);
    }
    driverFor(*pool).submit([this, h, g]() mutable {
      std::shared_ptr<SyncWaitGate> *savedTL = nullptr;
      if (g) {
        savedTL = std::exchange(tlSyncWaitGate, &g);
      }
      try {
        if constexpr (std::is_void_v<Result>) {
          body();
        } else {
          result.set(body());
        }
      } catch (...) {
        exc = std::current_exception();
      }
      h.resume();
      // h.resume has returned. The wrapper coroutine and every coroutine
      // it transferred to have fully unwound on this thread; the frame
      // members (body, result, exc) are no longer touched below.
      if (g) {
        tlSyncWaitGate = savedTL;
        const auto prev = g->state.fetch_sub(1, std::memory_order_release);
        if ((prev & SyncWaitGate::kCounterMask) == 1 &&
            (prev & SyncWaitGate::kDoneBit) != 0) {
          g->state.notify_all();
        }
      }
    });
  }

  /// Rethrows a body exception or returns the captured value.
  Result await_resume() {
    if (exc) {
      std::rethrow_exception(exc);
    }
    if constexpr (!std::is_void_v<Result>) {
      return result.take();
    }
  }
};

} // namespace detail

/// Run an arbitrary callable on the pool and await its return value.
template <class F>
[[nodiscard]] auto async(ThreadPool &pool, F body) {
  return detail::PoolAwaiter<F>{&pool, std::move(body), {}, {}};
}

/// Coroutine wrapper for `ThreadPool::parallelFor`.
template <class HintsT = HintsDefaults, class Body>
[[nodiscard]] auto parallelFor(ThreadPool &pool, std::size_t lo, std::size_t hi,
                               Body body,
                               CancellationToken tok = CancellationToken{}) {
  auto fn = [&pool, lo, hi, body = std::move(body),
             tok = std::move(tok)]() mutable {
    pool.parallelFor<HintsT>(lo, hi, std::move(body), std::move(tok));
  };
  return detail::PoolAwaiter<decltype(fn)>{&pool, std::move(fn), {}, {}};
}

/// Coroutine wrapper for `ThreadPool::parallelReduce`.
template <class HintsT = HintsDefaults, class T, class Map, class Combine>
[[nodiscard]] auto parallelReduce(ThreadPool &pool, std::size_t lo,
                                  std::size_t hi, T identity, Map map,
                                  Combine combine,
                                  CancellationToken tok = CancellationToken{}) {
  auto fn = [&pool, lo, hi, identity = std::move(identity),
             map = std::move(map), combine = std::move(combine),
             tok = std::move(tok)]() mutable -> T {
    return pool.parallelReduce<HintsT>(lo, hi, identity, std::move(map),
                                       std::move(combine), std::move(tok));
  };
  return detail::PoolAwaiter<decltype(fn)>{&pool, std::move(fn), {}, {}};
}

/// Coroutine wrapper for `ThreadPool::parallelScan`.
template <class HintsT = HintsDefaults, class T, class BodyFn, class PrefixFn>
[[nodiscard]] auto parallelScan(ThreadPool &pool, std::size_t n, T identity,
                                BodyFn body, PrefixFn prefix,
                                CancellationToken tok = CancellationToken{}) {
  auto fn = [&pool, n, identity = std::move(identity), body = std::move(body),
             prefix = std::move(prefix), tok = std::move(tok)]() mutable -> T {
    return pool.parallelScan<HintsT>(n, identity, std::move(body),
                                     std::move(prefix), std::move(tok));
  };
  return detail::PoolAwaiter<decltype(fn)>{&pool, std::move(fn), {}, {}};
}

/// Coroutine wrapper for `ThreadPool::parallelChain`.
template <class ChainHintsT = ChainHintsDefaults, class... Stages>
[[nodiscard]] auto parallelChain(ThreadPool &pool, std::size_t n,
                                 Stages... stages) {
  auto fn = [&pool, n,
             stagesTuple = std::make_tuple(std::move(stages)...)]() mutable {
    std::apply(
        [&pool, n](auto &&...st) {
          pool.parallelChain<ChainHintsT>(n, std::forward<decltype(st)>(st)...);
        },
        stagesTuple);
  };
  return detail::PoolAwaiter<decltype(fn)>{&pool, std::move(fn), {}, {}};
}

/// Coroutine wrapper for `ThreadPool::runPlex`.
template <class HintsT = HintsDefaults, class Phase>
[[nodiscard]] auto runPlex(ThreadPool &pool, std::size_t nPhases, std::size_t n,
                           Phase phase,
                           CancellationToken tok = CancellationToken{}) {
  auto fn = [&pool, nPhases, n, phase = std::move(phase),
             tok = std::move(tok)]() mutable {
    pool.runPlex<HintsT>(nPhases, n, std::move(phase), std::move(tok));
  };
  return detail::PoolAwaiter<decltype(fn)>{&pool, std::move(fn), {}, {}};
}

/// Coroutine wrapper for `ThreadPool::bulkForQueries`.
template <class HintsT = HintsDefaults, class QueryFn>
[[nodiscard]] auto bulkForQueries(ThreadPool &pool, std::size_t q,
                                  QueryFn query,
                                  CancellationToken tok = CancellationToken{}) {
  auto fn = [&pool, q, query = std::move(query),
             tok = std::move(tok)]() mutable {
    pool.bulkForQueries<HintsT>(q, std::move(query), std::move(tok));
  };
  return detail::PoolAwaiter<decltype(fn)>{&pool, std::move(fn), {}, {}};
}

/// Coroutine wrapper for `ThreadPool::forkJoin`.
template <class HintsT = HintsDefaults, class... TaskFns>
[[nodiscard]] auto forkJoin(ThreadPool &pool, TaskFns... fns) {
  auto fn = [&pool, fnsTuple = std::make_tuple(std::move(fns)...)]() mutable {
    std::apply(
        [&pool](auto &&...f) {
          pool.forkJoin<HintsT>(std::forward<decltype(f)>(f)...);
        },
        fnsTuple);
  };
  return detail::PoolAwaiter<decltype(fn)>{&pool, std::move(fn), {}, {}};
}

/// Coroutine variant that waits for the body to complete. Use the pool's
/// direct `submitDetached` for fire-and-forget without the round-trip.
template <class HintsT = HintsDefaults, class F>
[[nodiscard]] auto submitDetached(ThreadPool &pool, F body,
                                  CancellationToken tok = CancellationToken{}) {
  auto fn = [body = std::move(body), tok = std::move(tok)]() mutable {
    if (tok.stop_requested()) {
      return;
    }
    body();
  };
  return detail::PoolAwaiter<decltype(fn)>{&pool, std::move(fn), {}, {}};
}

/// Block the calling thread until |task| completes; return its value
/// or rethrow its exception. Destruction waits on the `SyncWaitGate`
/// so no worker is still inside `h.resume` when the wrapper frame
/// goes away.
template <class T>
T syncWait(Task<T> task) {
  std::exception_ptr exc;
  detail::ResultStorage<T> result;
  auto gate = std::make_shared<detail::SyncWaitGate>();

  auto wrapper = [&, gate]() -> Task<void> {
    try {
      if constexpr (std::is_void_v<T>) {
        co_await std::move(task);
      } else {
        result.set(co_await std::move(task));
      }
    } catch (...) {
      exc = std::current_exception();
    }
    gate->state.fetch_or(detail::SyncWaitGate::kDoneBit,
                         std::memory_order_release);
    // Skip the notify here: the last worker to fetch_sub its counter slot
    // will see (prev & kCounterMask) == 1 and the done bit set, and will
    // notify then. Notifying here would only produce spurious wakeups
    // (counter still non-zero).
  };

  auto *savedTL = std::exchange(detail::tlSyncWaitGate, &gate);
  Task<void> outer = wrapper();
  outer.handle().resume();
  detail::tlSyncWaitGate = savedTL;

  std::uint64_t s = gate->state.load(std::memory_order_acquire);
  while ((s & detail::SyncWaitGate::kDoneBit) == 0 ||
         (s & detail::SyncWaitGate::kCounterMask) != 0) {
    gate->state.wait(s, std::memory_order_acquire);
    s = gate->state.load(std::memory_order_acquire);
  }

  if (exc) {
    std::rethrow_exception(exc);
  }
  if constexpr (!std::is_void_v<T>) {
    return result.take();
  }
}

} // namespace citor::coro

// ===== citor/pool_group.h =====



namespace citor {

// Collection of `Arena`-kind `ThreadPool` instances, one per CCD.
//
// Two construction modes share the same class:
//
//   - Default-constructed `PoolGroup group;` is a normal RAII value: the
//     destructor joins every arena's workers when the variable goes out of
//     scope. Use this when the caller wants the worker fleet's lifetime
//     bounded (per-test fixtures, comparative benchmark cells, library
//     consumers that prefer scoped resource ownership).
//   - `PoolGroup::global()` returns a Meyers function-local-static; it
//     constructs lazily on first call, never destructs, and is intended for
//     library users who want one process-wide arena fleet that any thread
//     can access without coordinating ownership.
//
// Each arena is a normal `ThreadPool` whose workers are pinned to the CPUs
// of one CCD (or shared-L3 cluster). Arenas have disjoint workers and no
// cross-arena stealing.
//
// Deadlock rule (enforced by the cross-arena guard in `ThreadPool`): a thread
// participating in any `Arena` pool MUST NOT submit synchronous work to a
// different `Arena` pool; that would block the worker on a queue its own
// arena does not service. Each primitive's dispatch path checks
// `shouldFallThroughCrossArena()` and runs the work inline on the caller when
// the rule would be violated. The cross-arena call still produces correct
// results, but does not parallelize on the target arena.
//
// Thread-local participant token: every worker spawned by an `Arena` pool
// stores its arena index in `ThreadContext::arenaIndex` for the duration of
// its body. `localArena()` reads the token and returns the calling thread's
// owning arena, falling back to `arena(0)` when the calling thread is not a
// `PoolGroup` worker (the producer, a user-spawned `std::thread`, etc.).
class PoolGroup {
public:
  /// Returns the process-wide singleton, constructing it on first call.
  /// Construction is one-shot and thread-safe by virtue of the C++
  /// function-local-static rule: initialization runs under an implicit guard
  /// variable so concurrent first-callers do not race. After the first call,
  /// every invocation is a barrier-free pointer return. Workers spawned by
  /// `global()` live for the rest of the process; callers that need bounded
  /// worker lifetime should default-construct a `PoolGroup` instead.
  static PoolGroup &global() noexcept {
    static PoolGroup instance;
    return instance;
  }

  /// Returns the arena pinned to the CPUs of a single CCD. |ccdIndex| must be
  /// `< ccdCount()`.
  [[nodiscard]] ThreadPool &arena(std::size_t ccdIndex) noexcept {
    return *m_arenas[ccdIndex];
  }
  /// `const` overload of `arena(std::size_t)`. `|ccdIndex|` must be
  /// `< ccdCount()`.
  [[nodiscard]] const ThreadPool &arena(std::size_t ccdIndex) const noexcept {
    return *m_arenas[ccdIndex];
  }

  /// Returns the number of CCD arenas owned by this group. Equal to the size
  /// of `detail::enumerateCcds()` at construction time. Always at least 1.
  [[nodiscard]] std::size_t ccdCount() const noexcept {
    return m_arenas.size();
  }

  /// Returns the arena owning the calling thread.
  ///
  /// Reads the `ThreadContext::arenaIndex` participant token. When the calling
  /// thread is not a `PoolGroup` worker (the producer thread or any
  /// user-spawned `std::thread`), returns `arena(0)` so callers always get a
  /// valid arena to dispatch work to. The cross-arena deadlock guard in each
  /// primitive protects the deeper case where an `Arena` worker accidentally
  /// hands a different arena's reference around; `localArena()` is the safe
  /// default.
  [[nodiscard]] ThreadPool &localArena() noexcept {
    const std::size_t hint = currentArenaHint();
    if (hint >= m_arenas.size()) {
      return *m_arenas[0];
    }
    return *m_arenas[hint];
  }

  /// Constructs one arena per CCD reported by the topology probe. Falls back
  /// to a single arena over the host's allowed CPU set when sysfs is
  /// unavailable; in that case the resulting `PoolGroup` has `ccdCount() == 1`
  /// and behaves like a single full-machine pool from the caller's point of
  /// view. Each arena spawns its workers eagerly; the destructor joins them
  /// when the `PoolGroup` goes out of scope. This is the entry point for
  /// callers that want bounded worker-fleet lifetime; see `global()` for the
  /// process-wide singleton.
  PoolGroup() {
    const std::vector<std::vector<unsigned>> ccdCpus = detail::enumerateCcds();
    m_arenas.reserve(ccdCpus.size());
    std::uint32_t arenaIndex = 0;
    for (const auto &cpus : ccdCpus) {
      const std::vector<std::uint32_t> pins(cpus.begin(), cpus.end());
      const std::size_t participants =
          pins.empty() ? std::size_t{1} : pins.size();
      m_arenas.emplace_back(std::unique_ptr<ThreadPool>(new ThreadPool(
          ThreadPool::ArenaTag{}, participants, pins, arenaIndex)));
      ++arenaIndex;
    }
    if (m_arenas.empty()) {
      // Defensive fallback: enumerateCcds always returns at least one CCD,
      // but if a future platform port returns an empty list, spin up a
      // single-thread arena so callers never see an empty group.
      const std::vector<std::uint32_t> pins;
      m_arenas.emplace_back(std::unique_ptr<ThreadPool>(
          new ThreadPool(ThreadPool::ArenaTag{}, std::size_t{1}, pins, 0U)));
    }
  }

  // Non-copyable, non-movable: the arena pools own pinned worker threads
  // and the topology probe runs in the constructor body, so duplicating or
  // relocating the group would require re-running the probe and rebuilding
  // the arena vector. Pass by reference instead.
  PoolGroup(const PoolGroup &) = delete;
  PoolGroup &operator=(const PoolGroup &) = delete;
  PoolGroup(PoolGroup &&) = delete;
  PoolGroup &operator=(PoolGroup &&) = delete;

private:
  /// Reads the calling thread's arena participant token. Wraps
  /// `ThreadPool::currentArenaIndexHint`: when the calling thread is a worker
  /// on an `Arena` pool, returns `ThreadContext::arenaIndex`. Otherwise
  /// returns `kNoArenaHint` so `localArena()` can apply the safe default.
  [[nodiscard]] static std::size_t currentArenaHint() noexcept {
    const std::uint32_t index = ThreadPool::currentArenaIndexHint();
    if (index == kNoArenaSentinel) {
      return kNoArenaHint;
    }
    return static_cast<std::size_t>(index);
  }

  /// Sentinel returned when the calling thread is not a `PoolGroup` worker.
  static constexpr std::size_t kNoArenaHint = static_cast<std::size_t>(-1);

  /// 32-bit sentinel matching `ThreadPool::currentArenaIndexHint`'s "no arena"
  /// return.
  static constexpr std::uint32_t kNoArenaSentinel =
      static_cast<std::uint32_t>(-1);

  /// Owning storage for the per-CCD arenas.
  std::vector<std::unique_ptr<ThreadPool>> m_arenas;
};

} // namespace citor

// ===== citor/version.h =====

// Compile-time version string. Bumped automatically by `cz bump` per the
// `version_files` list in `pyproject.toml`. The amalgamated header reads the
// same string from `CMakeLists.txt`'s `project(... VERSION ...)` line.
//
// Integer comparison is omitted here: CMake consumers use
// `find_package(citor X.Y REQUIRED)` for version gating, and runtime callers
// can split this string. Inventing `CITOR_VERSION_MAJOR/MINOR/PATCH` macros
// is one more place commitizen would have to keep in sync, and lines like
// `#define CITOR_VERSION_PATCH 0` lose the version literal a regex can pin
// against.

#define CITOR_VERSION_STRING "0.4.4"
