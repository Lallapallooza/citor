#pragma once

#include "bench_format.h"
#include "cycle_clock.h"

namespace citor::bench {

#ifdef CITOR_BENCH_HAS_LIBFORK

[[nodiscard]] BenchRow runLibforkFib28(std::size_t participants,
                                       const CyclesPerNanosecond &cal);

/// Fine-grained fib (no sequential cutoff) -- the regime libfork's published
/// "beats TBB" numbers come from. Same N + cutoff parameters as the
/// `forkjoin_fib_fine_*` cells in fork_join_bench.cpp so all rows align.
[[nodiscard]] BenchRow runLibforkFibFine(std::size_t participants, int n,
                                         int cutoff,
                                         const CyclesPerNanosecond &cal);

[[nodiscard]] BenchRow runLibforkNQueens12(std::size_t participants,
                                           const CyclesPerNanosecond &cal);

[[nodiscard]] BenchRow runLibforkUtsT1(std::size_t participants,
                                       const CyclesPerNanosecond &cal);

[[nodiscard]] BenchRow runLibforkStrassen(std::size_t participants,
                                          std::size_t n,
                                          const CyclesPerNanosecond &cal);

[[nodiscard]] BenchRow runLibforkCilksort(std::size_t participants,
                                          std::size_t n,
                                          const CyclesPerNanosecond &cal);

/// Recursive divide-and-conquer dgemm. Mirrors libfork's published bench
/// shape (`bench/source/matmul/libfork.cpp`).
[[nodiscard]] BenchRow runLibforkMatmulDac(std::size_t participants,
                                           std::size_t n,
                                           const CyclesPerNanosecond &cal);

/// Skynet 1M -- 10-way fanout, depth 6, sum 0..(10^6 - 1).
[[nodiscard]] BenchRow runLibforkSkynet(std::size_t participants,
                                        const CyclesPerNanosecond &cal);

#endif

} // namespace citor::bench
