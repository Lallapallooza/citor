#pragma once

#include <vector>

#include "bench_format.h"
#include "cycle_clock.h"

namespace citor::bench {

/// Registration record for one workload (name + runner).
///
/// Each workload TU's anonymous-namespace registrar pushes one of these into
/// `registry()` at TU initialization time; `bench_main.cpp` iterates the
/// registry in registration order and prints each table.
struct WorkloadRegistration {
  /// Human-readable workload name shown in the table header.
  const char *name;

  /// Runner function returning a fully-populated `BenchTable`.
  BenchTable (*run)(const CyclesPerNanosecond &);
};

/// Process-global registry of bench workloads.
///
/// The function-local static avoids the static initialization order fiasco;
/// each workload TU's anonymous-namespace constructor pushes its registration
/// record from inside its own constructor body.
///
/// Mutable reference to the workload registry.
inline std::vector<WorkloadRegistration> &registry() {
  static std::vector<WorkloadRegistration> instance;
  return instance;
}

/// Add a workload to the registry; called from each workload TU.
///
/// reg Registration record describing the workload.
inline void registerWorkload(WorkloadRegistration reg) {
  registry().push_back(reg);
}

} // namespace citor::bench
