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

## Benchmarks

`benchmark/parallel_bench` measures dispatch latency and per-primitive throughput against ten peer pools (twelve in the recursive cells) across the eight primitives' workload shapes. The harness is opt-in via `CITOR_BUILD_BENCHMARK` so a tests-only configure stays fast; first configure with the bench on fetches every peer pool via CPM and takes a few minutes on a cold cache.

### Run

```bash
cmake -S . -B build -G Ninja -DCITOR_BUILD_BENCHMARK=ON
cmake --build build --target parallel_bench -j
taskset -c 0-15 ./build/benchmark/parallel_bench
```

`taskset` matters: pinning to a single CPU collapses the topology probe to one allowed core and the dispatch path falls through to the inline fallback. Run unpinned or with a mask wide enough to host every participant the cells request.

### Pools compared

| Pool | Notes |
|---|---|
| `BS::thread_pool` / `dp::thread_pool` / `task_thread_pool` / `riften::Thiefpool` | Small modern pools |
| `oneTBB` / `Taskflow` / `Eigen::ThreadPool` | Established frameworks |
| `OpenMP` | Pragma-based baseline |
| `Leopard::ThreadPool` / `dispenso::ThreadPool` | Newer experimental pools |
| `libfork` / `TooManyCooks` | Coroutine recursive schedulers; appear in fork-join cells only |

Recursive fork-join cells gate eligibility through `RecursiveForkJoinTraits`: pools that block on `std::future::get()` (BS, dp, task, riften) deadlock on nested spawn and are excluded from those cells. Every other comparison runs the full pool list.

### Workload taxonomy

| Primitive | Cells |
|---|---|
| `parallelFor` | empty fan-out, body granularity sweep, cold dispatch, Pareto body, plain matmul, balance sweep (citor only), affinity sweep (citor only), cross-CCD placement (citor only) |
| `parallelReduce` | sum-int64, plus-double with Kahan, Pareto body |
| `parallelChain` | pipelined chain, dynamic global chain, Pareto-body 7-stage chain |
| `parallelScan` | inclusive scan over int64 + double |
| `runPlex` | stencil sweep, heat-equation iterative |
| `bulkForQueries` | Q x N x dim search |
| `forkJoin` | nqueens, skynet, fib, UTS T1, Strassen, cilksort, matmul DAC, knapsack-cancel, mixed detached + sync, cross-CCD PoolGroup |
| `submitDetached` | covered by mixed detached + sync |
| differential | byte-equality across pools (correctness-as-bench) |

### Methodology

- **Cycle stamps** via `__rdtscp` + `lfence` bracketed around the timed body. The sampler converts cycles to ns through a startup TSC calibration.
- **Time-budget sampling**: each cell runs for a fixed wall-clock budget (default 250 ms) instead of a fixed iteration count. Cells with widely varying body times produce comparable sample sizes; the sampler stabilises the median internally.
- **Headline column is p25** (lower-quartile). p25 is more stable than mean under bimodal cells where one mode dominates the mean.
- **err% column** is the relative MAD over a centred window around the median; an outlier on either tail does not blow up the column.
- **One invocation per A/B**: the sampler covers repetition internally; outer repetition only adds host-jitter noise.

`--with-tail-percentiles` adds p25/p50/p99 columns backed by HdrHistogram with bootstrap CI for cells that opt in.

### JSON export and plotting

```bash
./build/benchmark/parallel_bench --export results.json
python3 -m tools.plot_bench results.json --out charts/
```

`--export` writes one record per `(workload, pool, rep)` plus a host-state provenance block (governor, boost, SMT, ASLR, TSan, libomp blocktime). Cycles are emitted as decimal strings (the JSON-no-int64 trap); ns are native numbers. `tools/plot_bench` renders per-workload bar figures plus a multi-workload overview heatmap.

### Two-pool BLAS coexistence

`parallel_bench_two_pool` hosts cells where citor and a competitor pool share the process. Standard `parallel_bench` cannot host two pools without the second pool's worker fleet poisoning the first cell's TLS state and idle-park accounting; the two-pool binary keeps these cells isolated and runs them with a 300 ms inter-cell cool-off so the prior cell's workers park before the next measures.

### Reproducing

For a fair run, disable ASLR, set the performance governor, and disable boost:

```bash
echo 0    | sudo tee /proc/sys/kernel/randomize_va_space
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
echo 0    | sudo tee /sys/devices/system/cpu/cpufreq/boost   # AMD
# echo 1  | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo  # Intel
```

Only one `parallel_bench` process at a time per host: concurrent runs poison each other's TLS state. The full sweep takes 40-50 minutes wall-clock on a 16-core CCD; use `--filter SUBSTR` to scope to one workload family while iterating.

### Caveats

- Linux x86_64 + AVX2 only. Numbers from any other platform are out of scope.
- The bench harness fetches every peer pool via CPM at configure time and pins each to a commit hash for reproducibility.
- Continuous-bench services that report retired-instruction counts are the wrong physics for a thread pool. Dispatch latency is futex park/unpark and cache coherency on done-epoch counters, not uops retired.

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
