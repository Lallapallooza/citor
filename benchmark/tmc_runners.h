#pragma once

#include "bench_format.h"
#include "cycle_clock.h"

namespace citor::bench {

#ifdef CITOR_BENCH_HAS_TMC

[[nodiscard]] BenchRow runTmcFib28(std::size_t participants, const CyclesPerNanosecond &cal);

[[nodiscard]] BenchRow runTmcNQueens12(std::size_t participants, const CyclesPerNanosecond &cal);

[[nodiscard]] BenchRow runTmcUtsT1(std::size_t participants, const CyclesPerNanosecond &cal);

#endif

} // namespace citor::bench
