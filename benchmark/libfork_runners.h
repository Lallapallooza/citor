#pragma once

#include "bench_format.h"
#include "cycle_clock.h"

namespace citor::bench {

#ifdef CITOR_BENCH_HAS_LIBFORK

[[nodiscard]] BenchRow runLibforkFib28(std::size_t participants, const CyclesPerNanosecond &cal);

[[nodiscard]] BenchRow runLibforkNQueens12(std::size_t participants, const CyclesPerNanosecond &cal);

[[nodiscard]] BenchRow runLibforkUtsT1(std::size_t participants, const CyclesPerNanosecond &cal);

#endif

} // namespace citor::bench
