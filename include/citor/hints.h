#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

namespace citor {

/// Cache-line size used for false-sharing avoidance.
///
/// Hardcoded to 128 bytes rather than `std::hardware_destructive_interference_size` because the
/// standard library value is implementation-defined, varies by `-mtune`, and is therefore an ABI
/// hand grenade across translation units compiled with different flags (P1119R0). 128 bytes is
/// the conservative value for x86-64: Zen-family L1 lines are 64 bytes but the L2 / data-line
/// fill prefetcher pulls in pairs, so the smallest false-sharing-safe stride is 128.
///
/// Every contended atomic in the pool is aligned to this value via `alignas(kCacheLine)`. AArch64
/// ports may want a different value; that path is gated behind a future `#if defined(__x86_64__)`
/// block.
inline constexpr std::size_t kCacheLine = 128;

/// Load-balancing strategy a primitive uses across its participants.
///
/// - `StaticUniform`: worker-strided block partition, no atomics on the hot path. Block ids are a
///   stable function of `(n, site_tag)` and independent of worker count, which gives deterministic
///   reductions cross-`nJobs` bit-identity for free.
/// - `DynamicChunked`: workers race on a single relaxed counter (`nextBlock.fetch_add`). One atomic,
///   no deque, no stealing. Use when per-block cost varies (kNN, threshold emit).
/// - `Steal`: opt-in Chase-Lev work-stealing on a range bag, halving the remaining range on a
///   successful steal. Reserved for skewed range workloads.
/// - `Recursive`: child-task work stealing for fork-join (recursive divide-and-conquer (e.g.
/// tree-build subtrees)).
enum class Balance : std::uint8_t {
  StaticUniform,
  DynamicChunked,
  Steal,
  Recursive,
};

/// Floating-point determinism contract a reduction primitive promises.
///
/// - `FixedBlockOrder`: chunk-id pairwise tree combine; bit-identical across worker counts when
///   `chunk_size = f(n, site_tag)`.
/// - `KahanCompensated`: Kahan/Neumaier compensated FP sum on top of the fixed-block tree.
/// - `OrderTolerant`: racy combine via a shared accumulator; only safe when the call site has
///   verified rounding does not depend on order (e.g., integer counts).
enum class Determinism : std::uint8_t {
  FixedBlockOrder,
  KahanCompensated,
  OrderTolerant,
};

/// CPU-affinity policy applied to the pool's workers.
///
/// Pinning is performed once at thread creation; `pthread_setaffinity_np` is never called on the
/// hot path. `OPENBLAS_NUM_THREADS=1` is a contract: when violated, the pool falls back to
/// `Affinity::None` and emits a one-shot trace warning.
enum class Affinity : std::uint8_t {
  None,
  PhysicalCores,
  SplitCcd,
  CcdLocal,
  FullMachine,
  CallerArena,
};

/// Per-call priority class consulted when concurrent producers contend on the same pool.
///
/// The pool serializes concurrent `dispatchOne` calls through a small two-bucket gate:
/// `Priority::Latency` callers jump ahead of `Priority::Throughput` callers waiting in the gate;
/// `Priority::Background` callers yield to throughput on dispatch contention. Within a single
/// primitive (one producer at a time) the priority is hint-only and the workers see the same
/// job either way; the gate only matters when two or more threads call into the pool concurrently.
///
/// The bucket is minimal (two preferred, one yielding) to keep the gate's overhead
/// off the dispatch hot path. `Background` is best-effort: it may be reordered behind any number of
/// higher-priority dispatches and offers no progress guarantee in the presence of sustained
/// higher-priority traffic.
enum class Priority : std::uint8_t {
  Latency,
  Throughput,
  Background,
};

/// Synchronization barrier inserted between two adjacent stages of `parallelChain`.
///
/// - `None`: worker proceeds to the next stage without synchronizing.
/// - `PerChunk`: each worker waits only on the upstream worker's flag for the same chunk id;
///   pipeline shape across stages keeps L1/L2 hot for a chunk.
/// - `Global`: sense-reversing barrier across all `participants`.
/// - `DeterministicReduce`: `Global` followed by a chunk-id pairwise-tree reduction.
/// - `ProducerSerial`: workers other than rank 0 spin on a producer-done flag while rank 0 runs
///   the serial body.
enum class BarrierKind : std::uint8_t {
  None,
  PerChunk,
  Global,
  DeterministicReduce,
  ProducerSerial,
};

/// Runtime-configurable hint POD passed through `parallelForRuntime` and CPO surfaces.
///
/// Compile-time call sites template on a named hint type (typically inheriting from
/// `HintsDefaults`); this struct is the runtime sibling for benchmark drivers and CLI
/// consumers that build a hint at runtime. The fields mirror the static-constexpr members of
/// `HintsDefaults` one-for-one, so the two dispatch paths run the same engine.
struct Hints {
  Balance balance = Balance::StaticUniform;
  Determinism determinism = Determinism::FixedBlockOrder;
  Affinity affinity = Affinity::None;
  Priority priority = Priority::Throughput;
  /// Estimated per-item cost in nanoseconds; `n * estimatedItemNs` gates the inline fallback.
  double estimatedItemNs = 0.0;
  /// Minimum wall time per task that justifies fan-out; below this the producer runs inline.
  /// Defaults to `0.0` so the inline-fallback gate is disabled; presets override upward.
  double minTaskUs = 0.0;
  /// Static block grain when `balance == StaticUniform`; 0 means "derive from `n / participants`".
  std::size_t chunk = 0;
  /// Whether worker bodies must check the cancellation token at chunk boundaries.
  bool cancellationChecks = true;
};

/// Compile-time defaults for every field a citor primitive may read off `HintsT`.
///
/// User hint presets inherit from this and override only the fields that differ:
///
///
/// struct MyKahanReduceHints : citor::HintsDefaults {
///   static constexpr Determinism determinism = Determinism::KahanCompensated;
///   static constexpr double minTaskUs = 25.0;
/// };
///
///
/// Fields mirror `Hints` one-for-one. The defaults are conservative: `StaticUniform` balance,
/// `FixedBlockOrder` reductions, no affinity, `Throughput` priority, no estimated cost (the
/// inline-fallback gate is disabled by default), 5us minimum task, derived chunk, and
/// cancellation polls on. Override down for hot loops that have already verified the path.
struct HintsDefaults {
  static constexpr Balance balance = Balance::StaticUniform;
  static constexpr Determinism determinism = Determinism::FixedBlockOrder;
  static constexpr Affinity affinity = Affinity::None;
  static constexpr Priority priority = Priority::Throughput;
  static constexpr double estimatedItemNs = 0.0;
  static constexpr double minTaskUs = 0.0;
  static constexpr std::size_t chunk = 0;
  static constexpr bool cancellationChecks = true;
};

/// Latency-biased preset: dynamic-chunked balance with `Priority::Latency`. Good first
///        cut for short jobs that want fast first response over peak throughput.
struct LatencyHints : HintsDefaults {
  static constexpr Balance balance = Balance::DynamicChunked;
  static constexpr Priority priority = Priority::Latency;
};

/// Bulk preset: cancellation polls disabled and a 25us minimum task size, tuned for hot
///        cost-uniform parallel-for loops where the body is known not to be cancelled mid-flight.
struct BulkHints : HintsDefaults {
  static constexpr double minTaskUs = 25.0;
  static constexpr bool cancellationChecks = false;
};

/// Reduction preset that selects Kahan-compensated determinism on top of the fixed-block
///        tree. Inherits the rest of `HintsDefaults`.
struct KahanReduceHints : HintsDefaults {
  static constexpr Determinism determinism = Determinism::KahanCompensated;
  static constexpr double minTaskUs = 25.0;
};

/// Reduction preset for plain fixed-block-order reductions without Kahan, for integer or
///        order-insensitive partials. Inherits `HintsDefaults`.
struct FixedBlockReduceHints : HintsDefaults {
  static constexpr double minTaskUs = 25.0;
};

/// Fork-join preset: `Balance::Recursive` (the work-stealing path) with same-CCD victim
///        biasing for cross-CCD locality.
struct CcdLocalForkJoinHints : HintsDefaults {
  static constexpr Balance balance = Balance::Recursive;
  static constexpr Affinity affinity = Affinity::CcdLocal;
};

/// Single stage of a `parallelChain`: a callable plus the barrier following it.
///
/// `Stage` owns no resources; it is a lightweight value carrying a reference (or moved-in
/// callable) and the compile-time `BarrierKind` driving the post-stage synchronization. The
/// variadic `parallelChain<ChainHintsT, Stages...>` accepts a parameter pack of these.
///
/// F     Callable type invoked with the chunk descriptor for that stage.
/// After Compile-time barrier inserted after this stage.
template <class F, BarrierKind After> struct Stage {
  /// Callable invoked once per chunk during this stage of the chain.
  F fn;

  /// Compile-time accessor for the post-stage barrier kind.
  static constexpr BarrierKind barrier = After;
};

/// Helper that constructs a `Stage` while deducing the callable type.
///
/// `makeStage<BarrierKind::Global>(lambda)` returns a `Stage<Lambda, BarrierKind::Global>` without
/// the caller spelling out the callable type. The post-stage barrier is the only template
/// argument the user must supply.
///
/// After Barrier inserted after this stage.
/// F     Deduced callable type.
/// fn     The callable to wrap.
template <BarrierKind After, class F>
constexpr auto
makeStage(F &&fn) noexcept(noexcept(Stage<std::decay_t<F>, After>{std::forward<F>(fn)})) {
  return Stage<std::decay_t<F>, After>{std::forward<F>(fn)};
}

/// Runtime-configurable chain-shape hints supplied to `parallelChainRuntime`.
///
/// Compile-time call sites template on a named chain hint type (typically inheriting from
/// `ChainHintsDefaults`). This struct mirrors the static-constexpr members of those types for
/// the runtime overload.
struct ChainHints {
  Balance balance = Balance::StaticUniform;
  Priority priority = Priority::Throughput;
  /// Whether downstream stages reuse the upstream worker's chunk for cache locality.
  bool pipelineSameChunk = true;
  /// Whether worker bodies must check the cancellation token at chunk boundaries.
  bool cancellationChecks = true;
  /// Static block grain shared by every stage when `balance == StaticUniform`.
  std::size_t chunk = 0;
};

/// Compile-time defaults for every field `parallelChain` reads off `ChainHintsT`.
///
/// User chain hint presets inherit from this and override only the fields that differ.
struct ChainHintsDefaults {
  static constexpr Balance balance = Balance::StaticUniform;
  static constexpr Priority priority = Priority::Throughput;
  static constexpr bool pipelineSameChunk = true;
  static constexpr bool cancellationChecks = true;
  static constexpr std::size_t chunk = 0;
};

} // namespace citor
