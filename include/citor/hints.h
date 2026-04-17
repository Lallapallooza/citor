#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

namespace citor {

/// Cache-line size used for false-sharing avoidance.
///
/// Hardcoded to 128 bytes rather than
/// `std::hardware_destructive_interference_size` because the standard library
/// value is implementation-defined, varies by `-mtune`, and is therefore an
/// ABI hand-grenade across translation units compiled with different flags
/// (P1119R0). 128 bytes is the conservative value for x86-64: Zen-family L1
/// lines are 64 bytes but the L2 / data-line fill prefetcher pulls in pairs,
/// so the smallest false-sharing-safe stride is 128.
///
/// Every contended atomic in the pool is aligned to this value via
/// `alignas(kCacheLine)`. AArch64 ports may want a different value; that
/// path is gated behind a future `#if defined(__x86_64__)` block.
inline constexpr std::size_t kCacheLine = 128;

/// Load-balancing strategy a primitive uses across its participants.
///
/// - `StaticUniform`: worker-strided block partition, no atomics on the hot
///   path. Block ids are a stable function of `(n, site_tag)` and independent
///   of worker count, which gives deterministic reductions cross-`nJobs`
///   bit-identity for free.
/// - `DynamicChunked`: workers race on a single relaxed counter
///   (`nextBlock.fetch_add`). One atomic, no deque, no stealing. Use when
///   per-block cost varies (kNN, threshold emit).
/// - `Steal`: opt-in Chase-Lev work-stealing on a range bag, halving the
///   remaining range on a successful steal. Reserved for skewed range
///   workloads.
/// - `Recursive`: child-task work stealing for fork-join (recursive
///   divide-and-conquer such as tree-build subtrees).
enum class Balance : std::uint8_t {
  StaticUniform,
  DynamicChunked,
  Steal,
  Recursive,
};

/// Floating-point determinism contract a reduction primitive promises.
///
/// - `FixedBlockOrder`: chunk-id pairwise tree combine; bit-identical across
///   worker counts when `chunk_size = f(n, site_tag)`.
/// - `KahanCompensated`: Kahan/Neumaier compensated FP sum on top of the
///   fixed-block tree.
/// - `OrderTolerant`: racy combine via a shared accumulator; only safe when
///   the call site has verified rounding does not depend on order (e.g.,
///   integer counts).
enum class Determinism : std::uint8_t {
  FixedBlockOrder,
  KahanCompensated,
  OrderTolerant,
};

/// CPU-affinity policy applied to the pool's workers.
///
/// Pinning is performed once at thread creation; `pthread_setaffinity_np` is
/// never called on the hot path. `OPENBLAS_NUM_THREADS=1` is a contract: when
/// violated, the pool falls back to `Affinity::None` and emits a one-shot
/// trace warning.
enum class Affinity : std::uint8_t {
  None,
  PhysicalCores,
  SplitCcd,
  CcdLocal,
  FullMachine,
  CallerArena,
};

/// Per-call priority class consulted when concurrent producers contend on the
/// same pool.
///
/// The pool serializes concurrent dispatch calls through a small two-bucket
/// gate: `Priority::Latency` callers jump ahead of `Priority::Throughput`
/// callers waiting in the gate; `Priority::Background` callers yield to
/// throughput on dispatch contention. Within a single primitive (one producer
/// at a time) the priority is hint-only and the workers see the same job
/// either way; the gate only matters when two or more threads call into the
/// pool concurrently.
///
/// `Background` is best-effort: it may be reordered behind any number of
/// higher-priority dispatches and offers no progress guarantee in the
/// presence of sustained higher-priority traffic.
enum class Priority : std::uint8_t {
  Latency,
  Throughput,
  Background,
};

/// Block-shape used by static-partition primitives.
///
/// - `CyclicBlocks`: worker `w` handles blocks `w, w+P, w+2P, ...`.
///   Cache-streaming pattern.
/// - `ContiguousRanges`: worker `w` handles a single contiguous range.
///   Preserves first-touch locality so per-worker scratch stays hot.
/// - `CcdContiguousRanges`: contiguous within a CCD; required when
///   `Affinity::SplitCcd` and the working set fits in CCD L3.
enum class Partition : std::uint8_t {
  CyclicBlocks,
  ContiguousRanges,
  CcdContiguousRanges,
};

/// Synchronization barrier inserted between two adjacent stages of
/// `parallelChain`.
///
/// - `None`: worker proceeds to the next stage without synchronizing.
/// - `PerChunk`: each worker waits only on the upstream worker's flag for
///   the same chunk id; pipeline shape across stages keeps L1/L2 hot for a
///   chunk.
/// - `Global`: sense-reversing barrier across all participants.
/// - `DeterministicReduce`: `Global` followed by a chunk-id pairwise-tree
///   reduction.
/// - `ProducerSerial`: workers other than rank 0 spin on a producer-done
///   flag while rank 0 runs the serial body.
enum class BarrierKind : std::uint8_t {
  None,
  PerChunk,
  Global,
  DeterministicReduce,
  ProducerSerial,
};

/// Runtime-configurable hint POD consumed by every primitive's CPO surface.
///
/// Compile-time call sites template on a named hint type; this struct is the
/// runtime sibling for benchmark drivers and CLI consumers that build a hint
/// at runtime. The fields mirror the static-constexpr members of every named
/// hint type one-for-one, so the two dispatch paths run the same engine.
struct Hints {
  Balance balance = Balance::StaticUniform;
  Determinism determinism = Determinism::FixedBlockOrder;
  Affinity affinity = Affinity::None;
  Priority priority = Priority::Throughput;
  Partition partition = Partition::ContiguousRanges;

  /// Estimated per-item cost in nanoseconds; `n * estimatedItemNs` gates the
  /// inline fallback.
  double estimatedItemNs = 0.0;

  /// Minimum wall time per task that justifies fan-out; below this the
  /// producer runs inline.
  double minTaskUs = 5.0;

  /// Static block grain when `balance == StaticUniform`; 0 means "derive
  /// from `n / participants`".
  std::size_t chunk = 0;

  /// Whether the call body needs a per-worker TLS scratch slot reserved
  /// before dispatch.
  bool tlsRequired = false;

  /// Whether the producer thread participates as slot 0 (true) or parks
  /// until completion.
  bool allowProducer = true;

  /// Whether other workers may steal blocks from this site's range bag.
  bool allowWorkerSteal = false;

  /// Whether nested calls into the pool from inside a worker body fan out
  /// (true) or run inline.
  bool allowNestedParallelism = false;

  /// Whether the reduction tree must be bit-reproducible across worker
  /// counts.
  bool fpDeterministicTree = true;

  /// Whether worker bodies must check the cancellation token at chunk
  /// boundaries.
  bool cancellationChecks = true;

  /// Whether a compatible-shape adjacent stage may pipeline on the same
  /// chunk per worker.
  bool pipelineSameChunk = false;
};

/// Single stage of a `parallelChain`: a callable plus the barrier following
/// it.
///
/// `Stage` owns no resources; it is a lightweight value carrying a reference
/// (or moved-in callable) and the compile-time `BarrierKind` driving the
/// post-stage synchronization. The variadic
/// `parallelChain<ChainHintsT, Stages...>` accepts a parameter pack of these.
template <class F, BarrierKind After>
struct Stage {
  /// Callable invoked once per chunk during this stage of the chain.
  F fn;

  /// Compile-time accessor for the post-stage barrier kind.
  static constexpr BarrierKind barrier = After;
};

/// Helper that constructs a `Stage` while deducing the callable type.
///
/// `makeStage<BarrierKind::Global>(lambda)` returns a
/// `Stage<Lambda, BarrierKind::Global>` without the caller spelling out the
/// callable type. The post-stage barrier is the only template argument the
/// user must supply.
template <BarrierKind After, class F>
constexpr auto
makeStage(F &&fn) noexcept(noexcept(Stage<std::decay_t<F>, After>{std::forward<F>(fn)})) {
  return Stage<std::decay_t<F>, After>{std::forward<F>(fn)};
}

/// Runtime-configurable chain-shape hints consumed by every chain CPO surface.
///
/// Compile-time call sites template on a named chain hint type. This struct
/// mirrors the static-constexpr members of those types for the runtime
/// overload.
struct ChainHints {
  Balance balance = Balance::StaticUniform;
  Affinity affinity = Affinity::None;
  Priority priority = Priority::Throughput;
  Partition partition = Partition::ContiguousRanges;

  /// Whether downstream stages reuse the upstream worker's chunk for cache
  /// locality.
  bool pipelineSameChunk = true;

  /// Whether the chain's reductions must be bit-reproducible across worker
  /// counts.
  bool fpDeterministicTree = true;

  /// Whether worker bodies must check the cancellation token at chunk
  /// boundaries.
  bool cancellationChecks = true;

  /// Static block grain shared by every stage when `balance == StaticUniform`.
  std::size_t chunk = 0;
};

} // namespace citor
