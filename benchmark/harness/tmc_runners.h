#pragma once

#include "bench_format.h"
#include "cycle_clock.h"

namespace citor::bench {

#ifdef CITOR_BENCH_HAS_TMC

[[nodiscard]] BenchRow runTmcFib28(std::size_t participants, const CyclesPerNanosecond &cal);

[[nodiscard]] BenchRow runTmcFibFine(std::size_t participants, int n, int cutoff,
                                     const CyclesPerNanosecond &cal);

[[nodiscard]] BenchRow runTmcNQueens12(std::size_t participants, const CyclesPerNanosecond &cal);

[[nodiscard]] BenchRow runTmcUtsT1(std::size_t participants, const CyclesPerNanosecond &cal);

[[nodiscard]] BenchRow runTmcStrassen(std::size_t participants, std::size_t n,
                                      const CyclesPerNanosecond &cal);

[[nodiscard]] BenchRow runTmcCilksort(std::size_t participants, std::size_t n,
                                      const CyclesPerNanosecond &cal);

[[nodiscard]] BenchRow runTmcMatmulDac(std::size_t participants, std::size_t n,
                                       const CyclesPerNanosecond &cal);

[[nodiscard]] BenchRow runTmcSkynet(std::size_t participants, const CyclesPerNanosecond &cal);

#endif

} // namespace citor::bench
