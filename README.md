# citor

> Header-only C++20 thread pool tuned for sub-microsecond dispatch. Eight cooperating primitives, decentralized per-slot done-epoch barriers, Chase-Lev work-stealing, per-CCD arenas. MIT.

[![ci](https://github.com/Lallapallooza/citor/actions/workflows/ci.yml/badge.svg)](https://github.com/Lallapallooza/citor/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)

The name comes from Latin *cito* -- "swiftly, quickly".

| | |
|---|---|
| Version | `0.1.0` |
| Build | header-only, CMake `INTERFACE` target `citor::citor` |
| Supported target | Linux x86_64 + AVX2 |
| Compilers | GCC 14, Clang 18 (CI-backed) |
| C++ standard | C++20 |
| Runtime deps | `Threads::Threads` / pthread |
| License | MIT |

---

## Table of contents

- [Contract](#contract)
- [Install](#install)
- [Hello world](#hello-world)
- [Performance](#performance)
- [Repository layout](#repository-layout)
- [Primitives](#primitives)
  - [`parallelFor`](#parallelfor)
  - [`parallelReduce`](#parallelreduce)
  - [`parallelScan`](#parallelscan)
  - [`parallelChain`](#parallelchain)
  - [`runPlex`](#runplex)
  - [`bulkForQueries`](#bulkforqueries)
  - [`forkJoin`](#forkjoin)
  - [`submitDetached`](#submitdetached)
- [Nested calls](#nested-calls)
- [Cookbook](#cookbook)
- [Hints reference](#hints-reference)
- [PoolGroup and per-CCD arenas](#poolgroup-and-per-ccd-arenas)
- [Cancellation](#cancellation)
- [Build options](#build-options)
- [Supported compilers](#supported-compilers)
- [Reproducing benchmarks](#reproducing-benchmarks)
- [License](#license)

---

## Contract

- **Header-only.** Drop `single_include/citor.hpp` into a project, or `find_package(citor)` after `cmake --install`. Linked C++ runtime + `pthread` are the only runtime dependencies.
- **Linux x86_64 + AVX2 only.** macOS / Windows / AArch64 are explicitly out of scope today; the pool relies on `futex`, sysfs CCD enumeration, and `__rdtsc` directly.
- **Closure lifetime >= call lifetime.** Every primitive captures the body via a 16-byte non-owning `FunctionRef`. The closure must outlive the synchronous call. Captures in the producer's stack frame satisfy this naturally.
- **Producer participates as slot 0.** Single-participant pools fall through to the inline path and never wake a worker. `n` participants means `n - 1` background pthreads plus the calling thread.
- **`PoolGroup::global()` is one arena per CCD.** Cross-arena synchronous calls fall through to inline on the caller (a TLS participant token enforces the rule); they never deadlock.
- **Nested parallelism is safe everywhere.** `parallelFor` and `forkJoin` have first-class same-pool nested paths (children land on the calling worker's deque). The other synchronous primitives detect same-pool reentrancy and fall through to inline-on-caller -- safe, but the inner call is not parallel.

## Install

Pick whichever path matches your project's existing dependency story.

### 1. Drop-in single header (zero build system)

```bash
curl -L -o third_party/citor.hpp \
  https://raw.githubusercontent.com/Lallapallooza/citor/v0.1.0/single_include/citor.hpp
```

```cpp
#include "third_party/citor.hpp"
```

Compile with `-std=c++20 -pthread` and (recommended) `-mavx2 -mfma -DCITOR_USE_AVX2`. Works with any C++20 compiler.

### 2. CMake `FetchContent`

```cmake
include(FetchContent)
FetchContent_Declare(citor
  GIT_REPOSITORY https://github.com/Lallapallooza/citor.git
  GIT_TAG        v0.1.0)
FetchContent_MakeAvailable(citor)

target_link_libraries(my_app PRIVATE citor::citor)
```

### 3. CPM

```cmake
CPMAddPackage("gh:Lallapallooza/citor#v0.1.0")
target_link_libraries(my_app PRIVATE citor::citor)
```

### 4. vcpkg (overlay port until upstream merge)

```bash
vcpkg install citor \
  --overlay-ports=path/to/citor/packaging/vcpkg/ports
```

The overlay flag goes away once the microsoft/vcpkg PR is accepted; until then, point vcpkg at this repo's `packaging/vcpkg/ports/` directory.

### 5. Conan (Conan 2.x)

```bash
conan create packaging/conan --version 0.1.0
conan install --requires=citor/0.1.0 --build=missing
```

The recipe is `package_type = "header-library"`, `no_copy_source = True`, `package_id().clear()`.

### 6. System install (`cmake --install`)

```bash
cmake -S . -B build -DCITOR_BUILD_TESTS=OFF -DCITOR_BUILD_BENCHMARK=OFF
cmake --build build
sudo cmake --install build
```

```cmake
find_package(citor 0.1 REQUIRED)
target_link_libraries(my_app PRIVATE citor::citor)
```

## Hello world

```cpp
#include <citor/cpos/parallel_for.h>
#include <citor/hints.h>
#include <citor/thread_pool.h>

#include <vector>

int main() {
  citor::ThreadPool pool(/*participants=*/8);

  std::vector<int> data(1'000'000, 1);

  pool.parallelFor<citor::HintsDefaults>(
      0, data.size(),
      [&](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
          data[i] *= 2;
        }
      });
}
```

The producer is slot 0. With one participant the call collapses to an inline loop and never wakes a worker. The body lives on the producer's stack for the call.

The `<citor/cpos/parallel_for.h>` header surfaces the primitive as a customization point object (a C++20 niebloid built on `tag_invoke`); each header under `include/citor/cpos/` exposes one primitive as a non-member callable so generic algorithms can dispatch through `parallel_for(pool, ...)` without depending on `pool.parallelFor(...)` being a member. Both spellings work.

## Performance

The bench harness measures dispatch latency and per-primitive throughput against ten peer pools (BS, dp, task, riften, oneTBB, Taskflow, Eigen, OpenMP, Leopard, dispenso); two coroutine schedulers (libfork, TooManyCooks) join the recursive fork-join cells. Numbers are not embedded in this README -- they age out on the next compiler / microarchitecture bump.

Reproduce on your hardware:

```bash
cmake -S . -B build -G Ninja -DCITOR_BUILD_BENCHMARK=ON
cmake --build build --target parallel_bench -j
taskset -c 0-15 ./build/benchmark/parallel_bench --export bench_out/results.json
python -m tools.plot_bench --input bench_out/results.json --output charts/
```

The harness probes host invariants (governor, turbo, SMT, ASLR, libomp blocktime) at startup and flags any failed gate in the table output and the JSON `context` block.

---

## Repository layout

```text
include/citor/                 Public modular headers
include/citor/cpos/            Customization-point objects per primitive
include/citor/detail/          Engine internals: dispatch, deque, futex park, topology, state
single_include/citor.hpp       Generated single header (regen via tools/amalgamate.py)
benchmark/                     parallel_bench harness + competitor wiring
tests/                         GTest suite: primitives, regressions, TSan stress
cmake/                         CMake options, target, install config, warnings, tooling
packaging/conan/               Conan 2.x recipe
packaging/vcpkg/ports/citor/   vcpkg overlay port
tools/                         amalgamate.py, bench wrappers, plot_bench
scripts/                       pre-commit helpers (ctest, clang-tidy, doc-string)
.github/workflows/             ci.yml, release.yml
```

Anything under `include/citor/{cpos,detail}/` is reachable but not part of the public API surface. Top-level headers in `include/citor/` (`thread_pool.h`, `hints.h`, `cancellation.h`, `chain.h`, `pool_group.h`, `function_ref.h`, `version.h`) are the user-facing entry points.

---

## Primitives

Every primitive is reachable as a member of `citor::ThreadPool` and as a free-standing CPO that dispatches through `tag_invoke`. The two surfaces share an engine and monomorphize identically. The compile-time hint type carries the policy (balance, determinism, affinity, priority, cost gates, chunk grain) so each call site picks its own policy without runtime branching.

### `parallelFor`

Bulk fan-out over a uniform `[first, last)` range. The headline path.

```cpp
template <class HintsT, class F>
void parallelFor(std::size_t first, std::size_t last, F &&fn,
                 CancellationToken tok = {});
```

`fn` is invoked once per block as `fn(std::size_t lo, std::size_t hi)`; the body must process every index in `[lo, hi)`. Block boundaries are hint-driven. The producer participates as slot 0 and runs at least its block's worth of work before joining.

**Inline-fallback gates** (compile-time, derived from `HintsT`):
- `participants == 1` -> inline.
- Cross-arena call from inside another arena's worker -> inline (the cross-arena guard).
- `n * estimatedItemNs * 1e-3 < minTaskUs * participants` -> inline. Disabled by default (`estimatedItemNs = 0.0`).

**Nested same-pool calls** re-route through `forkJoinAll`: the inner blocks land on the calling worker's own deque so peers can steal them. No deadlock, no dispatch-mutex re-entry. Use this freely for tiled / blocked workloads:

```cpp
// Outer: tiles along one dimension. Inner: tiles along the other.
pool.parallelFor<citor::HintsDefaults>(
    0, rows, [&](std::size_t r0, std::size_t r1) {
      pool.parallelFor<citor::HintsDefaults>(
          0, cols, [&](std::size_t c0, std::size_t c1) {
            for (std::size_t r = r0; r < r1; ++r) {
              for (std::size_t c = c0; c < c1; ++c) {
                out(r, c) = kernel(in, r, c);
              }
            }
          });
    });
```

**Hint knobs that matter for `parallelFor`**: `balance` (`StaticUniform` vs `DynamicChunked`), `chunk` (block grain, `0` derives from `n / participants / 2`), `minTaskUs` + `estimatedItemNs` (inline-fallback gate), `cancellationChecks` (compile out the per-chunk poll for tokens that cannot stop), `affinity`, `priority`.

```cpp
#include <citor/cpos/parallel_for.h>
#include <citor/hints.h>
#include <citor/thread_pool.h>

#include <vector>

void scaleVector(citor::ThreadPool &pool, std::vector<float> &v, float k) {
  pool.parallelFor<citor::HintsDefaults>(
      0, v.size(),
      [&](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
          v[i] *= k;
        }
      });
}
```

The CPO surface is equivalent and dispatches through `tag_invoke`:

```cpp
citor::parallelFor<citor::HintsDefaults>(
    pool, 0, v.size(),
    [&](std::size_t lo, std::size_t hi) { /* ... */ });
```

For runtime hints (build a `Hints` POD at run time):

```cpp
citor::Hints h;
h.balance = citor::Balance::DynamicChunked;
h.minTaskUs = 25.0;
pool.parallelForRuntime(0, v.size(), body, h);
```

**When to use it**: your work is a simple loop over a uniform range, you have >= a few microseconds of work per chunk, and you want straight fan-out.

**When to use something else**: deep recursion -> `forkJoin`. Multi-stage pipeline -> `parallelChain`. Iterated phases over the same partition -> `runPlex`.

### `parallelReduce`

Parallel reduction over `[first, last)` with deterministic combine semantics. Reduces to a value identical across worker counts when `Determinism::FixedBlockOrder` or `Determinism::KahanCompensated` is requested.

```cpp
template <class HintsT, class T, class Map, class Combine>
[[nodiscard]] T parallelReduce(std::size_t first, std::size_t last, T init,
                               Map &&map, Combine &&combine,
                               CancellationToken tok = {});
```

`map(lo, hi)` produces the partial value for one chunk; `combine(a, b)` combines two partials. The pool runs a chunk-id pairwise tree combine off the FixedBlockOrder shape so the result is bit-identical across worker counts when you ask for it.

**Determinism shapes** (selected on `HintsT::determinism`):
- `Determinism::FixedBlockOrder` -- chunk-id pairwise tree combine. Bit-identical across worker counts when `chunk_size` is a stable function of `(n, site_tag)`.
- `Determinism::KahanCompensated` -- Kahan/Neumaier compensated FP sum on top of the fixed-block tree.

The reduce-side hint presets (`KahanReduceHints`, `FixedBlockReduceHints`) override `balance` to `StaticUniform` because deterministic chunk-id-to-rank mapping requires it. Use them rather than rolling your own.

```cpp
#include <citor/cpos/parallel_reduce.h>
#include <citor/hints.h>
#include <citor/thread_pool.h>

#include <vector>

double sumKahan(citor::ThreadPool &pool, const std::vector<double> &xs) {
  return pool.parallelReduce<citor::KahanReduceHints>(
      0, xs.size(),
      /*init=*/0.0,
      [&](std::size_t lo, std::size_t hi) {
        double s = 0.0;
        for (std::size_t i = lo; i < hi; ++i) {
          s += xs[i];
        }
        return s;
      },
      [](double a, double b) { return a + b; });
}
```

**When to use it**: numeric sums where bit-identical results across runs and worker counts matter. Anything ill-conditioned where naive summation loses precision.

**Cancellation**: a stopped token throws `cancelled_value_exception<T>` carrying the deterministic combine of the chunks that completed before the cancellation was observed.

**Nesting**: invoked from inside another primitive on the same pool, the call falls through to inline-on-caller (single-threaded). For nested fan-out, fan out at the outer level and reduce serially per chunk:

```cpp
// GOOD: outer parallelFor over chunks, sequential reduce per chunk.
std::vector<double> partials(pool.participants(), 0.0);
pool.parallelFor<citor::HintsDefaults>(
    0, xs.size(), [&](std::size_t lo, std::size_t hi) {
      double s = 0.0;
      for (std::size_t i = lo; i < hi; ++i) s += xs[i];
      partials[citor::ThreadPool::workerIndex()] += s;
    });
double total = 0.0;
for (double p : partials) total += p;

// BAD: nested parallelReduce inside a parallelFor body runs single-threaded.
//   pool.parallelFor(..., [&](auto lo, auto hi) {
//     pool.parallelReduce(...);  // collapses to inline-on-caller
//   });
```

### `parallelScan`

Two-pass Blelloch inclusive prefix scan over `[0, n)`. Returns the inclusive accumulator at the right edge.

```cpp
template <class HintsT, class T, class BodyFn, class PrefixFn>
[[nodiscard]] T parallelScan(std::size_t n, T identity,
                             BodyFn &&body, PrefixFn &&prefix,
                             CancellationToken tok = {});
```

`body(chunkId, lo, hi, initial, out)` is invoked **twice per slot**:
- Pass 1 with `initial == identity`: compute and return the chunk's partial.
- Pass 2 with `initial == exclusivePrefix[slot]`: write the per-element scan into `out[lo..hi)`.

The producer computes chunk-level exclusive prefixes serially in `O(participants)` between the two passes. Two passes avoid the `O(n^2/p)` sequential bottleneck of split-recombine; the body's per-element work runs once per pass, twice total.

```cpp
#include <citor/cpos/parallel_scan.h>
#include <citor/hints.h>
#include <citor/thread_pool.h>

#include <cstdint>
#include <vector>

void inclusiveScan(citor::ThreadPool &pool,
                   const std::vector<std::int64_t> &in,
                   std::vector<std::int64_t> &out) {
  out.assign(in.size(), 0);
  int pass = 0;

  pool.parallelScan<citor::HintsDefaults>(
      in.size(),
      /*identity=*/std::int64_t{0},
      [&](std::size_t /*chunkId*/, std::size_t lo, std::size_t hi,
          std::int64_t initial, std::int64_t * /*unusedOut*/) {
        if (pass == 0) {
          std::int64_t s = 0;
          for (std::size_t i = lo; i < hi; ++i) s += in[i];
          return s;
        }
        std::int64_t running = initial;
        for (std::size_t i = lo; i < hi; ++i) {
          running += in[i];
          out[i] = running;
        }
        return running - initial;
      },
      [](std::int64_t a, std::int64_t b) { return a + b; });
}
```

**When to use it**: inclusive prefix sums or any associative scan over a contiguous buffer. With `participants == 1` the call collapses to a single body invocation (single-pass shape) -- the two-pass contract only applies when at least two participants exist.

**Nesting**: same-pool reentrancy falls through to inline-on-caller; the inner scan is single-threaded. Compose at the outer level instead:

```cpp
// GOOD: scan at the top, parallelFor inside the body for per-element work.
pool.parallelScan<citor::HintsDefaults>(
    n, std::int64_t{0},
    [&](std::size_t /*chunkId*/, std::size_t lo, std::size_t hi,
        std::int64_t initial, std::int64_t * /*out*/) {
      // Sequential body inside the scan -- nesting parallelFor here would
      // collapse to inline-on-caller.
      std::int64_t running = initial;
      for (std::size_t i = lo; i < hi; ++i) running += in[i];
      return running - initial;
    },
    [](std::int64_t a, std::int64_t b) { return a + b; });
```

### `parallelChain`

Multi-stage pipeline. One descriptor publish covers the entire chain; per-stage rendezvous is fully decentralized (per-slot done-epoch scan).

```cpp
template <class ChainHintsT, class... Stages>
void parallelChain(std::size_t n, Stages &&...stages);

template <class ChainHintsT, class... Stages>
void parallelChainWithToken(std::size_t n, CancellationToken tok, Stages &&...stages);
```

The cancellation overload is named `parallelChainWithToken` because the variadic `Stages` pack would otherwise absorb the leading token argument.

Each stage is built with one of the helpers from `<citor/chain.h>`:
- `staticStage(name, fn)`         -- `BarrierKind::None` (no rendezvous after).
- `globalStage(name, fn)`         -- `BarrierKind::Global` (rendezvous across all slots).
- `reduceStage(name, fn)`         -- `BarrierKind::DeterministicReduce`.
- `serialStage(name, fn)`         -- `BarrierKind::ProducerSerial` (rank 0 runs serially while others spin).
- `makeStage<BarrierKind::X>(fn)` -- explicit barrier kind without a name.

The stage body signature is `void(stageIdx, slot, lo, hi)`.

```cpp
#include <citor/chain.h>
#include <citor/cpos/parallel_chain.h>
#include <citor/hints.h>
#include <citor/thread_pool.h>

#include <atomic>
#include <cstdint>
#include <vector>

void runPipeline(citor::ThreadPool &pool, std::size_t n) {
  std::vector<std::atomic<std::int64_t>> stage1(pool.participants());
  std::vector<std::atomic<std::int64_t>> stage2(pool.participants());

  pool.parallelChain<citor::ChainHintsDefaults>(
      n,
      citor::globalStage(
          "fill",
          [&](std::size_t /*stageIdx*/, std::uint32_t slot,
              std::size_t lo, std::size_t hi) {
            stage1[slot].fetch_add(static_cast<std::int64_t>(hi - lo),
                                   std::memory_order_relaxed);
          }),
      citor::staticStage(
          "consume",
          [&](std::size_t /*stageIdx*/, std::uint32_t slot,
              std::size_t /*lo*/, std::size_t /*hi*/) {
            stage2[slot].store(
                stage1[slot].load(std::memory_order_relaxed) * 2,
                std::memory_order_relaxed);
          }));
}
```

**Hint knobs**: `pipelineSameChunk` (workers reuse their chunk across stages for cache locality, default true), `balance` (`StaticUniform` for same-chunk, `DynamicChunked` requires `pipelineSameChunk = false`), `chunk` (dynamic block grain when not same-chunk).

**When to use it**: your work has 2+ data-dependent stages over the same row range and the inter-stage transition latency is on the same order as a single `parallelFor` dispatch. Below that, a sequence of `parallelFor` calls is simpler and the chain has no advantage.

**Nesting**: same-pool reentrancy falls through to inline-on-caller (the chain runs single-threaded). Drive the chain from the producer; if a stage body wants fan-out, use the stage's own `(slot, lo, hi)` partition rather than a nested primitive:

```cpp
// GOOD: the chain itself fans out; the stage body works on its own (lo, hi).
pool.parallelChain<citor::ChainHintsDefaults>(
    n,
    citor::globalStage("compute",
        [&](std::size_t, std::uint32_t slot,
            std::size_t lo, std::size_t hi) {
          for (std::size_t i = lo; i < hi; ++i) buf[i] = work(i);
        }),
    citor::staticStage("write",
        [&](std::size_t, std::uint32_t slot,
            std::size_t lo, std::size_t hi) {
          for (std::size_t i = lo; i < hi; ++i) sink(buf[i]);
        }));

// BAD: chain nested inside a parallelFor body collapses to inline-on-caller.
```

### `runPlex`

The name is short for "persistent-worker phased loop": workers stay live across all `nPhases` phases of one `runPlex` call instead of being woken and joined per phase. Inter-phase transitions stay in user-space spin-wait, so the per-phase rendezvous overhead is in the hundreds of nanoseconds rather than a futex round-trip per phase.

```cpp
template <class HintsT, class Phase>
void runPlex(std::size_t nPhases, std::size_t n, Phase &&phaseFn,
             CancellationToken tok = {});
```

`phaseFn(phaseIdx, slot, lo, hi)` is invoked exactly once per `(phase, slot)` pair, in stable phase-then-slot order.

```cpp
#include <citor/cpos/run_plex.h>
#include <citor/hints.h>
#include <citor/thread_pool.h>

#include <cstdint>
#include <vector>

void stencil(citor::ThreadPool &pool,
             std::vector<float> &grid, std::size_t steps) {
  pool.runPlex<citor::HintsDefaults>(
      steps, grid.size(),
      [&](std::size_t /*phaseIdx*/, std::uint32_t /*slot*/,
          std::size_t lo, std::size_t hi) {
        for (std::size_t i = (lo == 0 ? 1 : lo);
             i < (hi == grid.size() ? hi - 1 : hi); ++i) {
          grid[i] = 0.5f * grid[i] + 0.25f * (grid[i - 1] + grid[i + 1]);
        }
      });
}
```

**When to use it**: iterative numeric kernels (Jacobi / Gauss-Seidel / stencil sweeps) where the same partition is reused across phases and the per-phase body is small. For one-shot fan-outs `parallelFor` is cheaper because `runPlex` keeps workers spinning between phases.

**Nesting**: same-pool reentrancy falls through to inline-on-caller; the inner phased loop is single-threaded. `runPlex` is meant to be the outermost driver, with a sequential body per phase:

```cpp
// GOOD: runPlex at the top, sequential per-phase body.
pool.runPlex<citor::HintsDefaults>(
    steps, grid.size(),
    [&](std::size_t /*phase*/, std::uint32_t /*slot*/,
        std::size_t lo, std::size_t hi) {
      for (std::size_t i = (lo == 0 ? 1 : lo);
           i < (hi == grid.size() ? hi - 1 : hi); ++i) {
        next[i] = 0.5f * grid[i] + 0.25f * (grid[i - 1] + grid[i + 1]);
      }
    });

// BAD: runPlex nested inside parallelFor / forkJoin collapses to single-threaded.
```

### `bulkForQueries`

Many independent queries fanned across the pool. Differs from `parallelFor` in semantics: the body must process **every** query index in the chunk, and per-query results must be written to a per-query slot keyed on the query index (chunk dispatch order varies across worker counts).

```cpp
template <class HintsT, class QueryFn>
void bulkForQueries(std::size_t q, QueryFn &&fn,
                    CancellationToken tok = {});
```

```cpp
#include <citor/cpos/bulk_for_queries.h>
#include <citor/hints.h>
#include <citor/thread_pool.h>

#include <cstdint>
#include <vector>

struct Query;
struct Hit;

void runQueries(citor::ThreadPool &pool,
                const std::vector<Query> &queries,
                std::vector<Hit> &out) {
  out.resize(queries.size());
  pool.bulkForQueries<citor::HintsDefaults>(
      queries.size(),
      [&](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
          out[i] = /* run queries[i] */ Hit{};
        }
      });
}
```

**When to use it**: spatial-index lookups, batched key/value gets, KD-tree or BVH ray queries -- workloads where per-query depth varies and a `Balance::DynamicChunked` policy amortises the skew. Use `parallelFor` instead when the per-item cost is uniform.

**Nesting**: same-pool reentrancy falls through to inline-on-caller. If a single query body itself wants fan-out, prefer `forkJoin` inside it (which has a first-class nested path) over another `bulkForQueries`:

```cpp
pool.bulkForQueries<citor::HintsDefaults>(
    queries.size(), [&](std::size_t lo, std::size_t hi) {
      for (std::size_t i = lo; i < hi; ++i) {
        // Per-query recursive descent: forkJoin nests cleanly here.
        Hit left, right;
        pool.forkJoin<citor::CcdLocalForkJoinHints>(
            [&] { left  = descend(queries[i], tree.left); },
            [&] { right = descend(queries[i], tree.right); });
        out[i] = merge(left, right);
      }
    });
```

### `forkJoin`

Recursive divide-and-conquer over per-worker Chase-Lev work-stealing deques. Tasks may call back into `forkJoin` from inside their bodies; nested fork-join is the headline use case. The nested call uses the Cilk-5 spawn-parent shape: children are pushed onto the calling worker's own deque (visible to peers via Chase-Lev), the last child runs inline, and the join is a per-frame `pendingTasks` counter.

```cpp
template <class HintsT, class... TaskFns>
void forkJoin(TaskFns &&...fns);

template <class HintsT, class... TaskFns>
void forkJoin(CancellationToken tok, TaskFns &&...fns);
```

The producer participates as slot 0 and steals from other workers' deques when its own drains. `Affinity::CcdLocal` (the default and the named preset `CcdLocalForkJoinHints`) biases steal probes to same-CCD victims first.

```cpp
#include <citor/cpos/fork_join.h>
#include <citor/hints.h>
#include <citor/thread_pool.h>

int fib(citor::ThreadPool &pool, int n) {
  if (n < 2) return n;
  if (n < 12) return fib(pool, n - 1) + fib(pool, n - 2);

  int a = 0;
  int b = 0;
  pool.forkJoin<citor::CcdLocalForkJoinHints>(
      [&] { a = fib(pool, n - 1); },
      [&] { b = fib(pool, n - 2); });
  return a + b;
}
```

**Exception handling**: the first exception escaping any task body is captured and rethrown from the producer after the join. Subsequent throws drop. The remaining tasks are cancelled so the join doesn't block on quiescence.

**When to use it**: divide-and-conquer with non-uniform recursion (Strassen, cilksort, DAC matmul, UTS, BVH builds). For straight loops over uniform ranges, `parallelFor` has lower dispatch overhead and bigger blocks.

### `submitDetached`

Fire-and-forget. The pool's destructor blocks until every detached body has retired; until then, the pool's lifetime extends every in-flight body.

```cpp
template <class HintsT, class TaskFn>
void submitDetached(TaskFn fn, CancellationToken tok = {});
```

```cpp
#include <citor/cpos/submit_detached.h>
#include <citor/hints.h>
#include <citor/thread_pool.h>

#include <atomic>

void scheduleBackground(citor::ThreadPool &pool) {
  pool.submitDetached<citor::HintsDefaults>(
      [] { /* background work */ });
  // Pool destructor will wait for the body before returning.
}
```

**Exception handling**: a throw from a detached body is captured into a per-pool slot and surfaced via `pool.lastDetachedException()`. Subsequent throws drop. The pool does not call `std::terminate` on a detached throw.

**When to use it**: tear-down work whose completion is not needed on the caller's join path, but whose retirement is needed before the pool itself can go away (logging flush, async finalisation). For anything the caller actually waits on, use a synchronous primitive.

---

## Nested calls

What happens when a synchronous primitive runs inside another primitive's body on the same pool:

| inner call (from a same-pool worker)  | behavior                                                                  |
|----------------------------------------|---------------------------------------------------------------------------|
| `parallelFor`                          | First-class nested path; inner chunks dispatch in parallel.               |
| `forkJoin`                             | First-class recursive path; children land on the calling worker's deque. |
| `parallelReduce`                       | Same-pool reentrancy detected; inner call runs inline on the caller.      |
| `parallelScan`                         | Same as above.                                                            |
| `parallelChain`                        | Same as above.                                                            |
| `runPlex`                              | Same as above.                                                            |
| `bulkForQueries`                       | Same as above.                                                            |
| `submitDetached`                       | Always submits; not synchronous, so no reentrancy concern.                |

Cross-arena calls (worker on `PoolGroup` arena A invokes a synchronous primitive on arena B) fall through to inline-on-caller as well; the TLS participant token enforces this so cross-arena synchronous submissions cannot deadlock.

---

## Cookbook

Worked recipes for common parallel patterns. Each assumes a `citor::ThreadPool pool;` is in scope. Pick the primitive that matches the shape, not the name.

### Tiled dense matrix multiply (`parallelFor`)

Tile along rows, keep inner loops serial so each tile fits in L1/L2.

```cpp
void matmul(citor::ThreadPool &pool, const float *A, const float *B, float *C,
            std::size_t n) {
  constexpr std::size_t kTileRows = 32;
  const std::size_t tiles = (n + kTileRows - 1) / kTileRows;

  pool.parallelFor<citor::HintsDefaults>(
      0, tiles,
      [&](std::size_t loTile, std::size_t hiTile) {
        for (std::size_t t = loTile; t < hiTile; ++t) {
          const std::size_t i0 = t * kTileRows;
          const std::size_t i1 = std::min(i0 + kTileRows, n);
          for (std::size_t i = i0; i < i1; ++i) {
            for (std::size_t j = 0; j < n; ++j) {
              float acc = 0.0f;
              for (std::size_t k = 0; k < n; ++k) {
                acc += A[i * n + k] * B[k * n + j];
              }
              C[i * n + j] = acc;
            }
          }
        }
      });
}
```

### Histogram (`parallelReduce`)

Per-chunk private bins combine into a global histogram. No atomics on the hot loop.

```cpp
constexpr std::size_t kBins = 256;
using Bins = std::array<std::uint64_t, kBins>;

Bins total = pool.parallelReduce<citor::FixedBlockReduceHints>(
    0, samples.size(), Bins{},
    [&](std::size_t lo, std::size_t hi) {
      Bins bin{};
      for (std::size_t i = lo; i < hi; ++i) {
        ++bin[samples[i] & 0xFFu];
      }
      return bin;
    },
    [](const Bins &a, const Bins &b) {
      Bins c{};
      for (std::size_t k = 0; k < kBins; ++k) c[k] = a[k] + b[k];
      return c;
    });
```

For very large bin counts, prefer per-slot heap allocation (one `Bins` per worker) and a separate post-pass; the reduce-shaped tree above is the right default for small-to-medium bin counts.

### Stream compaction (`parallelScan`)

Mark surviving elements, exclusive-scan to write offsets, scatter into the compacted output.

```cpp
template <class Pred>
std::size_t compact(citor::ThreadPool &pool, const std::vector<int> &in,
                    std::vector<int> &out, Pred &&pred) {
  const std::size_t n = in.size();
  std::vector<std::int64_t> mark(n);

  pool.parallelFor<citor::HintsDefaults>(
      0, n,
      [&](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) mark[i] = pred(in[i]) ? 1 : 0;
      });

  std::vector<std::int64_t> offsets(n);
  std::int64_t kept = 0;
  for (std::size_t i = 0; i < n; ++i) {            // serial exclusive scan
    offsets[i] = kept;
    kept += mark[i];
  }

  out.resize(static_cast<std::size_t>(kept));
  pool.parallelFor<citor::HintsDefaults>(
      0, n,
      [&](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
          if (mark[i]) out[offsets[i]] = in[i];
        }
      });
  return static_cast<std::size_t>(kept);
}
```

The serial scan is fine up to a few million elements; replace with `parallelScan` when the marks cost is dwarfed by the scan itself.

### Separable image blur (`parallelChain`)

Two-pass blur: horizontal first, full barrier, then vertical. Each row is independent within a pass.

```cpp
pool.parallelChain<citor::ChainHintsDefaults>(
    height,
    citor::globalStage("blur-horizontal",
        [&](std::size_t /*stage*/, std::uint32_t /*slot*/,
            std::size_t lo, std::size_t hi) {
          for (std::size_t y = lo; y < hi; ++y) blurRowH(in, tmp, y, width);
        }),
    citor::staticStage("blur-vertical",
        [&](std::size_t /*stage*/, std::uint32_t /*slot*/,
            std::size_t lo, std::size_t hi) {
          for (std::size_t y = lo; y < hi; ++y) blurRowV(tmp, out, y, width);
        }));
```

`globalStage` rendezvous all participants before the next stage; the vertical pass observes a fully-written `tmp`.

### Heat equation with ping-pong buffers (`runPlex`)

Phase parity selects source / destination so workers never write to the same buffer they're reading.

```cpp
std::vector<float> a(n), b(n);
// ... initialize a[] ...

pool.runPlex<citor::HintsDefaults>(
    iterations, n - 2,
    [&](std::size_t phase, std::uint32_t /*slot*/,
        std::size_t lo, std::size_t hi) {
      const auto &src = (phase & 1U) ? b : a;
      auto &dst = (phase & 1U) ? a : b;
      for (std::size_t i = lo + 1; i < hi + 1; ++i) {
        dst[i] = 0.5f * src[i] + 0.25f * (src[i - 1] + src[i + 1]);
      }
    });

const auto &result = (iterations & 1U) ? b : a;
```

Compared to a plain `parallelFor` loop with `std::swap` between iterations, `runPlex` keeps workers in user-space across phase transitions instead of paying a wake/park round-trip per step.

### BVH ray-batch query (`bulkForQueries`)

One independent query per index; per-query cost varies with traversal depth, so dynamic chunking matters.

```cpp
pool.bulkForQueries<citor::DynamicHints>(
    rays.size(),
    [&](std::size_t lo, std::size_t hi) {
      for (std::size_t q = lo; q < hi; ++q) {
        out[q] = bvh.intersect(rays[q]);   // each query writes its own slot
      }
    });
```

Result must be keyed by query index (`out[q]`), not by chunk order: a fast worker can finish many adjacent queries while a slow worker is still in one. Same shape works for spatial-hash lookups, KD-tree nearest-neighbor, and per-row sparse matrix queries.

### Quicksort with cutoff (`forkJoin`)

Recurse in parallel above a grain threshold; below it, hand off to a serial sort. Workers steal subranges via per-worker Chase-Lev deques so an unbalanced partition does not stall the slowest leaf.

```cpp
void quicksort(citor::ThreadPool &pool, std::span<int> a) {
  if (a.size() < 2048) {
    std::sort(a.begin(), a.end());
    return;
  }
  // Standard partition: returns the index where the pivot lands.
  const std::size_t mid = partition(a);
  pool.forkJoin<citor::CcdLocalForkJoinHints>(
      [&] { quicksort(pool, a.subspan(0, mid)); },
      [&] { quicksort(pool, a.subspan(mid + 1)); });
}
```

The same shape covers cilksort, BVH build, branch-and-bound, and recursive divide-and-conquer matrix algorithms (Strassen). For branch-and-bound, also pass a `CancellationToken` so a global-best update can prune outstanding work.

### Per-CCD shard processing (`PoolGroup`)

Process partitions of a large array on the L3 cluster that owns each partition's memory. Drive each arena from its own producer thread so all CCDs run in parallel.

```cpp
auto &group = citor::PoolGroup::global();
const std::size_t shards = group.ccdCount();
const std::size_t per = (data.size() + shards - 1) / shards;

std::vector<std::thread> drivers;
for (std::size_t ccd = 0; ccd < shards; ++ccd) {
  drivers.emplace_back([&, ccd] {
    auto &arena = group.arena(ccd);
    const std::size_t lo = ccd * per;
    const std::size_t hi = std::min(lo + per, data.size());
    arena.parallelFor<citor::HintsDefaults>(
        lo, hi,
        [&](std::size_t i0, std::size_t i1) {
          for (std::size_t i = i0; i < i1; ++i) data[i] = transform(data[i]);
        });
  });
}
for (auto &t : drivers) t.join();
```

The arena's workers are pinned to that CCD's cores at construction; `parallelFor` inside the arena keeps memory traffic on the local L3 instead of crossing the inter-CCD fabric.

### Cancellation across primitives

```cpp
auto tok = citor::CancellationToken::makeOwned();

std::thread watchdog([&] {
  if (deadline_passed()) tok.request_stop();
});

pool.parallelFor<citor::HintsDefaults>(
    0, n,
    [&](std::size_t lo, std::size_t hi) {
      pool.forkJoin(
          [&] { search_left(lo, hi, tok);  },
          [&] { search_right(lo, hi, tok); });
    },
    tok);
```

Pass the same token down through nested calls. The outer `parallelFor` polls between blocks and stops admitting new ones; the inner `forkJoin` propagates the same token. Set `HintsT::cancellationChecks = true` (the default) when bodies should poll; set it `false` for hot loops where the token is known never to fire.

---

## Hints reference

Every primitive templates on `HintsT` (or `ChainHintsT`). Inherit `HintsDefaults` and override only what differs:

```cpp
struct MyHints : citor::HintsDefaults {
  static constexpr citor::Affinity affinity  = citor::Affinity::CcdLocal;
  static constexpr double          minTaskUs = 25.0;
  static constexpr std::size_t     chunk     = 4096;
};
```

| field                | type            | default            | what it controls |
|----------------------|-----------------|--------------------|------------------|
| `balance`            | `Balance`       | `DynamicChunked`   | `StaticUniform` (worker-strided block partition, deterministic block->rank) vs `DynamicChunked` (atomic counter, straggler-tolerant). |
| `determinism`        | `Determinism`   | `FixedBlockOrder`  | `parallelReduce` only. `FixedBlockOrder` = chunk-id pairwise tree. `KahanCompensated` = Kahan/Neumaier on top. |
| `affinity`           | `Affinity`      | `CcdLocal`         | `forkJoin` steal-victim direction. `CcdLocal` biases same-CCD victims first. |
| `priority`           | `Priority`      | `Throughput`       | Two-bucket gate when concurrent producers contend. `Latency` jumps the gate; `Background` yields. |
| `estimatedItemNs`    | `double`        | `0.0`              | Per-item cost estimate. With `minTaskUs > 0`, gates the inline fallback as `n * estimatedItemNs * 1e-3 < minTaskUs * participants`. |
| `minTaskUs`          | `double`        | `0.0`              | Minimum task wall time that justifies fan-out. Pair with `estimatedItemNs`. `0.0` disables the gate. |
| `chunk`              | `std::size_t`   | `0`                | Static block grain (when `balance == StaticUniform`). `0` = derive from `n / participants`. |
| `cancellationChecks` | `bool`          | `true`             | Whether worker bodies poll the cancellation token at chunk boundaries. Compile out with `false` for tokens that cannot stop. |

Bundled presets (in `<citor/hints.h>`):

| preset                    | what it changes                                           | use when |
|---------------------------|-----------------------------------------------------------|----------|
| `HintsDefaults`           | the defaults above                                        | every primitive's first cut. |
| `StaticHints`             | `balance = StaticUniform`                                 | uniform-cost loops that benefit from cold-collapse's typed monomorphised fast path. |
| `DynamicHints`            | `balance = DynamicChunked`                                | a stable name for the future-proof default. |
| `LatencyHints`            | `priority = Latency`                                      | short jobs that want fast first response over peak throughput. |
| `BulkHints`               | `minTaskUs = 25.0`, `cancellationChecks = false`          | hot uniform-cost loops with no cancellation. |
| `KahanReduceHints`        | `determinism = KahanCompensated`, `minTaskUs = 25.0`      | numerically sensitive sums (`parallelReduce`). |
| `FixedBlockReduceHints`   | `minTaskUs = 25.0`                                        | integer or order-insensitive reductions (`parallelReduce`). |
| `CcdLocalForkJoinHints`   | `affinity = CcdLocal`                                     | recursive fork-join workloads with cross-CCD locality. |
| `ChainHintsDefaults`      | chain shape: `balance = StaticUniform`, `pipelineSameChunk = true` | most chains. |
| `DynamicChainHints`       | chain shape: `balance = DynamicChunked`, `pipelineSameChunk = false` | stage packs with skewed bodies and only Global / DeterministicReduce barriers. |

For runtime-driven decisions (benchmark drivers, CLI tools), every primitive has a `*Runtime` sibling that takes a `Hints` POD by value. The runtime path goes through the same engine -- it just trades the compile-time monomorphisation for an in-register hint struct.

## PoolGroup and per-CCD arenas

`PoolGroup::global()` lazily constructs one `ThreadPool` arena per CCD detected by sysfs. Each arena's `pool.kind()` is `PoolKind::Arena`. Cross-arena synchronous calls fall through to the inline path on the caller -- the TLS participant token guards against a worker on arena A submitting work to arena B and blocking on a queue arena A doesn't service.

```cpp
#include <citor/cpos/parallel_for.h>
#include <citor/hints.h>
#include <citor/pool_group.h>

void perCcd() {
  citor::PoolGroup &group = citor::PoolGroup::global();
  for (std::size_t i = 0; i < group.ccdCount(); ++i) {
    citor::ThreadPool &arena = group.arena(i);
    arena.parallelFor<citor::HintsDefaults>(
        0, 1'000'000,
        [&](std::size_t lo, std::size_t hi) { /* per-CCD work */ });
  }
}

void localArenaPath() {
  // Whichever CCD the caller is pinned to (or arena 0 on a non-worker thread).
  citor::ThreadPool &arena = citor::PoolGroup::global().localArena();
  arena.parallelFor<citor::HintsDefaults>(0, 1024, [](auto, auto) {});
}
```

`localArena()` returns `arena(0)` when the calling thread is not a `PoolGroup` worker (the producer or any user-spawned `std::thread`), so callers always get a valid arena to dispatch to.

## Cancellation

`citor::CancellationToken` is a copy-cheap handle wrapping a heap-allocated atomic. The default-constructed sentinel is allocation-free and `stop_requested()` always returns `false`; obtain a token whose flag can actually be set via `CancellationToken::makeOwned()`.

```cpp
#include <citor/cancellation.h>
#include <citor/cpos/parallel_for.h>
#include <citor/hints.h>
#include <citor/thread_pool.h>

#include <thread>

void cancellable(citor::ThreadPool &pool) {
  citor::CancellationToken tok = citor::CancellationToken::makeOwned();

  std::thread killer([tok]() mutable {
    using namespace std::chrono_literals;
    std::this_thread::sleep_for(50ms);
    tok.request_stop();
  });

  try {
    pool.parallelFor<citor::HintsDefaults>(
        0, 1'000'000'000,
        [&](std::size_t lo, std::size_t hi) {
          for (std::size_t i = lo; i < hi; ++i) { /* heavy work */ }
        },
        tok);
  } catch (const citor::cancelled_exception &) {
    // void-returning primitives throw cancelled_exception on a stopped token.
  }

  killer.join();
}
```

`parallelReduce` throws `citor::cancelled_value_exception<T>` carrying the deterministic combine of the chunks completed before cancellation. `forkJoin` cancels by clamping the outstanding count and returning normally; tasks that haven't started just don't run.

The `cancellationChecks` hint compiles out the per-chunk poll for tokens that are statically known not to stop. Pair it with the never-stopped sentinel for hot loops where cancellation is not a possibility.

## Build options

| option                          | default      | effect |
|---------------------------------|--------------|--------|
| `CITOR_BUILD_TESTS`             | ON top-level | Build the GTest suite. |
| `CITOR_BUILD_BENCHMARK`         | ON top-level | Build the comparative bench (fetches ten peer pools via CPM; first cold configure is 5-10 minutes). |
| `CITOR_USE_AVX2`                | ON           | Compile with `-mavx2 -mfma`, define `CITOR_USE_AVX2`. Auto-detected via `check_cxx_compiler_flag`. |
| `CITOR_BUILD_WITH_SANITIZER`    | OFF          | Build with `-fsanitize=thread -fno-omit-frame-pointer`. |
| `CITOR_ENABLE_POOL_COUNTERS`    | OFF          | Compile in pool-level diagnostic counters (dispatches, inline fallbacks, cancellation stops). Hot path pays no extra atomics when OFF. |
| `CITOR_WORKER_STACK_KIB`        | `8192`       | Per-worker pthread stack size (KiB). |

clang-tidy is not a build option -- it runs only as a pre-commit hook (over staged files) and as a dedicated CI job (over the whole tree).

When citor is consumed via `add_subdirectory(...)` or CPM, all of the above default to OFF -- the consumer gets the public INTERFACE target only.

## Supported compilers

- **GCC 14+**, **Clang 18+** on `ubuntu-24.04`. CI runs both on every push.
- **GCC 15** and **Clang 19** are exercised on a weekly cron and allowed to fail-soft.
- libstdc++ on GCC, libc++ on the TSan job (libstdc++ produces false-positive races on standard library internals under TSan; the LLVM project's own check-tsan suite uses libc++).

Linux kernel 6.x with `futex` and sysfs `cpu/cpuX/cache/index*` is required. AVX2 is auto-detected; the pool builds without it but loses the AVX2-tuned code paths.

## Reproducing benchmarks

The full sweep fetches ten peer pools and runs ~94 workloads; wall-clock is 40-50 minutes on a 16-core CCD. Use `python -m tools.bench isolated` to run each cell in its own process so a competitor's segfault does not kill the whole run.

```bash
cmake -S . -B build -G Ninja -DCITOR_BUILD_BENCHMARK=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --target parallel_bench -j
python -m tools.bench run
```

Disable ASLR (`/proc/sys/kernel/randomize_va_space=0`), set `cpufreq` governor to `performance`, and disable boost before timing-sensitive runs. The harness's startup checklist surfaces any gate that did not pass.

## License

MIT. See [`LICENSE`](LICENSE).
