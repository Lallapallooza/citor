#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

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
///
/// - `StaticUniform`: worker-strided block partition, no atomics on the hot
/// path. Block ids are a
///   stable function of `(n, site_tag)` and independent of worker count, which
///   gives deterministic reductions cross-`nJobs` bit-identity for free.
/// - `DynamicChunked`: workers race on a single relaxed counter
/// (`nextBlock.fetch_add`). One atomic,
///   no deque, no stealing. Use when per-block cost varies (kNN, threshold
///   emit).
enum class Balance : std::uint8_t {
  StaticUniform,
  DynamicChunked,
};

/// Floating-point determinism contract a reduction primitive promises.
///
/// - `FixedBlockOrder`: chunk-id pairwise tree combine; bit-identical across
/// worker counts when
///   `chunk_size = f(n, site_tag)`.
/// - `KahanCompensated`: Kahan/Neumaier compensated FP sum on top of the
/// fixed-block tree.
enum class Determinism : std::uint8_t {
  FixedBlockOrder,
  KahanCompensated,
};

/// CPU-affinity policy applied to the pool's workers.
///
/// Worker placement on the pool is fixed at construction
/// (one-logical-per-physical-core, all pinned to slot 0's CCD by default; see
/// `Affinity::CcdLocal`). The hint controls where fork-join steal probes look
/// for victims, NOT where workers spawn.
///
/// - `CcdLocal`: bias steal probes toward same-CCD workers first; fall back to
/// cross-CCD only if
///   the local CCD has no work to take. Matches the default placement and is
///   the right choice for data-locality-sensitive recursive workloads.
/// - `None`: no steal-direction preference; probe every worker uniformly.
enum class Affinity : std::uint8_t {
  None,
  CcdLocal,
};

/// Per-call priority class consulted when concurrent producers contend on the
/// same pool.
///
/// The pool serializes concurrent `dispatchOne` calls through a small
/// two-bucket gate: `Priority::Latency` callers jump ahead of
/// `Priority::Throughput` callers waiting in the gate; `Priority::Background`
/// callers yield to throughput on dispatch contention. Within a single
/// primitive (one producer at a time) the priority is hint-only and the workers
/// see the same job either way; the gate only matters when two or more threads
/// call into the pool concurrently.
///
/// The bucket is minimal (two preferred, one yielding) to keep the gate's
/// overhead off the dispatch hot path. `Background` is best-effort: it may be
/// reordered behind any number of higher-priority dispatches and offers no
/// progress guarantee in the presence of sustained higher-priority traffic.
enum class Priority : std::uint8_t {
  Latency,
  Throughput,
  Background,
};

/// Synchronization barrier inserted between two adjacent stages of
/// `parallelChain`.
///
/// - `None`: worker proceeds to the next stage without synchronizing.
/// - `Global`: rendezvous across all `participants`.
/// - `DeterministicReduce`: `Global` followed by a chunk-id pairwise-tree
/// reduction.
/// - `ProducerSerial`: workers other than rank 0 spin on a producer-done flag
/// while rank 0 runs
///   the serial body.
enum class BarrierKind : std::uint8_t {
  None,
  Global,
  DeterministicReduce,
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
  Affinity affinity = Affinity::CcdLocal;
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
  static constexpr Affinity affinity = Affinity::CcdLocal;
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

/// Fork-join preset with same-CCD victim biasing for cross-CCD locality.
///
/// Inherits from `HintsDefaults` and only changes the steal-direction hint.
/// forkJoin uses its own Chase-Lev deques; the `Balance` field is not consulted
/// on the fork-join hot path.
struct CcdLocalForkJoinHints : HintsDefaults {
  static constexpr Affinity affinity = Affinity::CcdLocal;
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
