#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <thread>
#include <utility>
#include <vector>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

#include "citor/detail/cpu_relax.h"
#include "citor/hints.h"

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
[[gnu::always_inline]] inline void coherenceProbePin(int cpu) noexcept {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<std::size_t>(cpu), &set);
  (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}
#else
/// No-op fallback on non-Linux platforms; the probe runs unpinned.
[[gnu::always_inline]] inline void coherenceProbePin(int /*cpu*/) noexcept {}
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

} // namespace citor::detail
