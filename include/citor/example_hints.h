#pragma once

/// @file example_hints.h
/// A small library of named hint presets covering the dispatch shapes most users want.
///
/// Each struct exposes the static-constexpr members `citor::Hints` consumes, so any of these can
/// be passed as the `HintsT` template argument to `parallelFor`, `parallelReduce`, `runPlex`,
/// `bulkForQueries`, `parallelChain`, `parallelScan`, or `forkJoin`. Users are expected to copy
/// one of these into their own header, rename it to something call-site-specific (e.g.
/// `MyImageBlurHints`), and tune the constants for their workload; this header just makes the
/// starting points explicit so a fresh consumer does not have to invent the spelling.

#include <cstddef>

#include "citor/hints.h"

namespace citor {

/// Default for parallel-for over uniform-cost rows: static-uniform balance, throughput
///        priority, contiguous range partitioning, no per-chunk cancellation polls.
struct BulkBalancedHints {
  static constexpr Balance balance = Balance::StaticUniform;
  static constexpr Affinity affinity = Affinity::SplitCcd;
  static constexpr Priority priority = Priority::Throughput;
  static constexpr Partition partition = Partition::ContiguousRanges;
  static constexpr double estimatedItemNs = 0.0;
  static constexpr double minTaskUs = 25.0;
  static constexpr std::size_t chunk = 0;
  static constexpr bool tlsRequired = true;
  static constexpr bool cancellationChecks = false;
};

/// Low-latency parallel-for: dynamic guided balance, latency priority, smaller minimum
///        task size, biased toward fast first response over peak throughput.
struct LatencyParallelForHints {
  static constexpr Balance balance = Balance::DynamicChunked;
  static constexpr Affinity affinity = Affinity::CcdLocal;
  static constexpr Priority priority = Priority::Latency;
  static constexpr Partition partition = Partition::ContiguousRanges;
  static constexpr double estimatedItemNs = 0.0;
  static constexpr double minTaskUs = 5.0;
  static constexpr std::size_t chunk = 0;
  static constexpr bool tlsRequired = false;
  static constexpr bool cancellationChecks = false;
};

/// Bulk many-query workload (e.g. spatial-index lookups, batched key-value gets) where
///        per-item cost is roughly uniform but the producer wants a stride-friendly partition.
struct BulkQueryHints {
  static constexpr Balance balance = Balance::StaticUniform;
  static constexpr Affinity affinity = Affinity::SplitCcd;
  static constexpr Priority priority = Priority::Throughput;
  static constexpr Partition partition = Partition::CyclicBlocks;
  static constexpr double estimatedItemNs = 0.0;
  static constexpr double minTaskUs = 25.0;
  static constexpr std::size_t chunk = 0;
  static constexpr bool tlsRequired = false;
  static constexpr bool cancellationChecks = true;
};

/// Scatter-then-fold parallel-for: workers scatter into per-slot private buffers, the
///        producer folds. Tuned for medium granularity and producer-side reduce.
struct ScatterFoldHints {
  static constexpr Balance balance = Balance::StaticUniform;
  static constexpr Affinity affinity = Affinity::SplitCcd;
  static constexpr Priority priority = Priority::Throughput;
  static constexpr Partition partition = Partition::ContiguousRanges;
  static constexpr double estimatedItemNs = 0.0;
  static constexpr double minTaskUs = 25.0;
  static constexpr std::size_t chunk = 0;
  static constexpr bool tlsRequired = true;
  static constexpr bool cancellationChecks = false;
};

/// Same shape as `ScatterFoldHints` but selects the Kahan-compensated determinism mode
///        when the scatter target is a floating-point partial. Use this when the per-chunk fold
///        wants a Kahan accumulator instead of a plain `+`.
struct ScatterFoldKahanHints {
  static constexpr Balance balance = Balance::StaticUniform;
  static constexpr Affinity affinity = Affinity::SplitCcd;
  static constexpr Priority priority = Priority::Throughput;
  static constexpr Partition partition = Partition::ContiguousRanges;
  static constexpr Determinism determinism = Determinism::KahanCompensated;
  static constexpr double estimatedItemNs = 0.0;
  static constexpr double minTaskUs = 25.0;
  static constexpr std::size_t chunk = 0;
  static constexpr bool tlsRequired = true;
  static constexpr bool cancellationChecks = false;
};

/// Deterministic Kahan-compensated reduction. The fixed-block tree plus Kahan
///        compensation keeps the result bit-identical across runs at fixed `nJobs`.
struct KahanReduceHints {
  static constexpr Balance balance = Balance::StaticUniform;
  static constexpr Determinism determinism = Determinism::KahanCompensated;
  static constexpr Affinity affinity = Affinity::SplitCcd;
  static constexpr Priority priority = Priority::Throughput;
  static constexpr Partition partition = Partition::ContiguousRanges;
  static constexpr double estimatedItemNs = 0.0;
  static constexpr double minTaskUs = 25.0;
  static constexpr std::size_t chunk = 0;
};

/// Plain fixed-block-order reduction without Kahan, when the partial type is integer or
///        the user does not need FP compensation.
struct FixedBlockReduceHints {
  static constexpr Balance balance = Balance::StaticUniform;
  static constexpr Determinism determinism = Determinism::FixedBlockOrder;
  static constexpr Affinity affinity = Affinity::SplitCcd;
  static constexpr Priority priority = Priority::Throughput;
  static constexpr Partition partition = Partition::ContiguousRanges;
  static constexpr double estimatedItemNs = 0.0;
  static constexpr double minTaskUs = 25.0;
  static constexpr std::size_t chunk = 0;
};

/// Plex hint preset for frontier-update style workloads where each phase reads the
///        previous frontier and writes the next one. The producer-serial barrier between phases
///        is the natural rendezvous shape for this.
struct FrontierPlexHints {
  static constexpr Balance balance = Balance::StaticUniform;
  static constexpr Affinity affinity = Affinity::SplitCcd;
  static constexpr Priority priority = Priority::Throughput;
  static constexpr Partition partition = Partition::ContiguousRanges;
  static constexpr BarrierKind barrier = BarrierKind::ProducerSerial;
  static constexpr double estimatedItemNs = 0.0;
  static constexpr double minTaskUs = 5.0;
  static constexpr std::size_t chunk = 0;
};

/// Bulk frontier hint for parallel-for over a frontier (e.g. BFS layer) where the per-
///        chunk cost is uniform but the call may be cancelled mid-flight.
struct BulkFrontierHints {
  static constexpr Balance balance = Balance::StaticUniform;
  static constexpr Affinity affinity = Affinity::SplitCcd;
  static constexpr Priority priority = Priority::Throughput;
  static constexpr Partition partition = Partition::ContiguousRanges;
  static constexpr double estimatedItemNs = 0.0;
  static constexpr double minTaskUs = 25.0;
  static constexpr std::size_t chunk = 0;
  static constexpr bool cancellationChecks = true;
};

/// Local-join hint preset for parallel-for over a per-worker join structure. Tuned to
///        minimise cross-CCD coherence on the hot inner loop.
struct LocalJoinHints {
  static constexpr Balance balance = Balance::StaticUniform;
  static constexpr Affinity affinity = Affinity::CcdLocal;
  static constexpr Priority priority = Priority::Throughput;
  static constexpr Partition partition = Partition::ContiguousRanges;
  static constexpr double estimatedItemNs = 0.0;
  static constexpr double minTaskUs = 25.0;
  static constexpr std::size_t chunk = 0;
  static constexpr bool tlsRequired = true;
};

/// Default chain hints for a balanced multi-stage pipeline with global rendezvous between
///        stages.
struct ChainBalancedHints {
  static constexpr Balance balance = Balance::StaticUniform;
  static constexpr Affinity affinity = Affinity::SplitCcd;
  static constexpr Priority priority = Priority::Throughput;
  static constexpr Partition partition = Partition::ContiguousRanges;
  static constexpr bool pipelineSameChunk = true;
  static constexpr bool fpDeterministicTree = true;
  static constexpr bool cancellationChecks = false;
  static constexpr std::size_t chunk = 0;
};

} // namespace citor
