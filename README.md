# citor

Header-only C++20 thread pool tuned for sub-microsecond dispatch on modern x86_64 (Linux + AVX2). Eight cooperating primitives, decentralized barriers, Chase-Lev work-stealing, per-CCD arenas.

The name is from Latin *cito* ("swiftly, quickly").

## Status

Pre-release. Linux x86_64 + AVX2 only. The public API is stable enough to consume; perf numbers and bench harness are still being hardened. macOS / Windows / AArch64 ports are not in scope yet.

## Quickstart

```bash
cmake -S . -B build -G Ninja
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Consume from your own CMake project:

```cmake
include(cmake/CPM.cmake)
CPMAddPackage(NAME citor SOURCE_DIR /path/to/citor)
target_link_libraries(my_app PRIVATE citor::citor)
```

## Primitives

| Primitive | Use it for |
|---|---|
| `parallelFor` | Bulk fan-out over a uniform range. The headline path. |
| `parallelReduce` | Deterministic reduction (FixedBlockOrder / KahanCompensated). |
| `parallelScan` | Two-pass Blelloch inclusive prefix scan. |
| `parallelChain` | Multi-stage pipeline; one descriptor publish covers every stage. |
| `runPlex<NPhases>` | Persistent-worker phased loop (sub-microsecond per phase). |
| `bulkForQueries` | Many-query workload (spatial-index lookups, batched gets). |
| `forkJoin` | Recursive divide-and-conquer over a Chase-Lev deque. |
| `submitDetached` | Fire-and-forget; pool waits for in-flight tasks at destruction. |

`PoolGroup::global()` lazily constructs one `ThreadPool` arena per CCD; the participant token enforces "no cross-arena synchronous call" so the model stays deadlock-free without locks.

## Hint API

Every primitive is templated on a `HintsT` policy type. Inherit `citor::HintsDefaults` and override only the fields that differ:

```cpp
struct MyHints : citor::HintsDefaults {
  static constexpr citor::Affinity affinity  = citor::Affinity::SplitCcd;
  static constexpr double          minTaskUs = 25.0;
};
```

A small library of named presets ships in `<citor/hints.h>` alongside `HintsDefaults`: `LatencyHints`, `BulkHints`, `KahanReduceHints`, `FixedBlockReduceHints`, `CcdLocalForkJoinHints`. Chains use `ChainHintsDefaults` the same way.

## Bench

```bash
cmake -S . -B build -G Ninja -DCITOR_BUILD_BENCHMARK=ON
cmake --build build --target parallel_bench
taskset -c 0-15 ./build/benchmark/parallel_bench
```

Workloads compare the citor pool against BS::thread_pool, dp::thread_pool, task_thread_pool, riften::Thiefpool, oneTBB, Taskflow, Eigen NonBlockingThreadPool, and OpenMP. See `benchmark/README` (TBD) for the methodology and current numbers.

## Build options

| Option | Default | Effect |
|---|---|---|
| `CITOR_BUILD_TESTS` | ON top-level | Build the GTest suite. |
| `CITOR_BUILD_BENCHMARK` | ON top-level | Build the comparative bench. |
| `CITOR_USE_AVX2` | ON | Compile with `-mavx2 -mfma`, define `CITOR_USE_AVX2`. |
| `CITOR_BUILD_WITH_SANITIZER` | OFF | Build with `-fsanitize=thread`. |
| `CITOR_ENABLE_CLANG_TIDY` | ON top-level | Run clang-tidy in the build. |

## License

MIT. See `LICENSE`.
