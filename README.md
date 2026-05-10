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
| Validated target | Linux x86_64 + AVX2 |
| Compilers | GCC 14, Clang 18 (CI-backed) |
| C++ standard | C++20 |
| Runtime deps | `Threads::Threads` / pthread |
| License | MIT |

---

## Table of contents

- [What citor is](#what-citor-is)
- [Hard contract](#hard-contract)
- [Install](#install)
- [Quick start](#quick-start)
- [Repository layout](#repository-layout)
- [Public API shape](#public-api-shape)
- [ThreadPool lifecycle](#threadpool-lifecycle)
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
- [Hints](#hints)
- [Cancellation and deadlines](#cancellation-and-deadlines)
- [PoolGroup and per-CCD arenas](#poolgroup-and-per-ccd-arenas)
- [Diagnostics and counters](#diagnostics-and-counters)
- [Build, test, and release workflow](#build-test-and-release-workflow)
- [Benchmarks](#benchmarks)
- [Supported targets](#supported-targets)
- [License](#license)

---

## What citor is

`citor` exposes one pool type and eight primitives over it. The producer participates as slot 0 on every synchronous call -- small jobs stay on the producer with zero wake-up cost; large jobs fan out to workers and join in the same call.

```cpp
citor::ThreadPool pool(/*participants=*/8);
```

| primitive          | shape                                                              |
|--------------------|--------------------------------------------------------------------|
| `parallelFor`      | fan out a contiguous `[first, last)` loop                          |
| `parallelReduce`   | map chunks and combine partials with a deterministic tree shape    |
| `parallelScan`     | two-pass inclusive prefix scan over `[0, n)`                       |
| `parallelChain`    | run a multi-stage pipeline from one dispatch descriptor            |
| `runPlex`          | keep workers live across repeated phases over the same partition  |
| `bulkForQueries`   | run many independent query indices with variable per-query cost    |
| `forkJoin`         | recursive divide-and-conquer with per-worker Chase-Lev deques      |
| `submitDetached`   | fire-and-forget; the pool destructor waits for retirement          |

The library is CPU-bound and synchronous. There is no `co_await` surface, no future, no I/O reactor. Bodies that block on I/O, sleep, or wait on external locks defeat the latency contract.

## Hard contract

These points are API contract, not implementation trivia.

- **Header-only.** Including modular headers under `include/citor/` or the generated `single_include/citor.hpp` is enough; there is no library binary to link. Linked C++ runtime + `pthread` are the only runtime dependencies.
- **`ThreadPool(participants)` is the total participant count, including the calling thread.** A pool of `8` runs the caller plus `7` background pthreads, subject to topology and affinity-mask clamping. Query the effective count with `pool.participants()`. `participants == 0` throws `std::invalid_argument`.
- **Closure lifetime >= call lifetime.** Every primitive captures the body via a 16-byte non-owning `FunctionRef`. The callable must outlive the synchronous call. Captures in the producer's stack frame satisfy this naturally.
- **Producer participates as slot 0.** Single-participant pools fall through to the inline path and never wake a worker.
- **`PoolGroup::global()` is one arena per CCD.** Cross-arena synchronous calls fall through to inline on the caller (a TLS participant token enforces the rule); they never deadlock.
- **`ThreadPool` is non-copyable, non-movable.** Workers hold interior pointers to per-instance state.
- **Cancellation is cooperative.** A stop request is observed at primitive-defined boundaries, not by preempting a running body.
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

## Quick start

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

The `<citor/cpos/parallel_for.h>` header surfaces the primitive as a customization-point object built on `tag_invoke`; each header under `include/citor/cpos/` exposes one primitive as a non-member callable so generic algorithms can dispatch through `parallelFor(pool, ...)` without depending on `pool.parallelFor(...)` being a member. Both spellings work.

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

## Public API shape

### Member calls (the normal spelling)

Most user code calls primitives as members of `ThreadPool`:

```cpp
pool.parallelFor<citor::HintsDefaults>(0, n, body);
const double sum = pool.parallelReduce<citor::FixedBlockReduceHints>(
    0, n, 0.0, map, combine);
pool.forkJoin<citor::CcdLocalForkJoinHints>(left, right);
```

Each member primitive templates on a hint type so the policy fully monomorphises into the call site.

### CPO calls (`tag_invoke` surface)

Each `<citor/cpos/...>` header exposes the primitive as a function object built on `tag_invoke`. The function object is not itself a function template, so supplying the hint type uses the templated-call-operator spelling:

```cpp
#include <citor/cpos/parallel_for.h>

citor::parallelFor.template operator()<citor::HintsDefaults>(
    pool, 0, n,
    [&](std::size_t lo, std::size_t hi) { /* ... */ });
```

Use the CPO surface for generic executor adapters and tests that need `tag_invoke` dispatch. Use the member surface in application code.

### Runtime hint siblings

When policy is decided at runtime (CLI flags, benchmark drivers), every primitive has a `*Runtime` sibling that takes a `Hints` POD by value:

```cpp
citor::Hints h;
h.balance = citor::Balance::DynamicChunked;
h.chunk = 1024;
h.minTaskUs = 25.0;

pool.parallelForRuntime(0, n, body, h);
pool.parallelReduceRuntime(0, n, init, map, combine, h);
pool.runPlexRuntime(phases, n, phaseBody, h);
pool.bulkForQueriesRuntime(q, queryBody, h);

citor::ChainHints ch;
pool.parallelChainRuntime(n, ch, citor::CancellationToken{}, stages...);
```

The runtime path uses the same engine; it trades compile-time monomorphisation for an in-register hint struct.

## ThreadPool lifecycle

```cpp
explicit ThreadPool(std::size_t participants);
```

Construction probes sysfs topology, prefers one logical CPU per physical core, clamps the requested count to the usable affinity mask, allocates per-slot worker state, creates one Chase-Lev deque per participant, and spawns `participants - 1` background pthreads with pre-bound affinity, raw futex parking, and a configured pthread stack size.

Lifecycle points worth knowing:

- `participants == 0` throws `std::invalid_argument`. Construction may also throw `std::system_error` when pthread setup fails.
- The pool is non-copyable and non-movable.
- Destruction signals shutdown, wakes parked workers, joins them, restores any producer auto-pin the pool owns, and waits for `submitDetached` work to retire.
- `pool.participants()` returns the effective count after topology clamping.
- `pool.kind()` distinguishes a user-constructed `Standalone` pool from a `PoolGroup::global()` `Arena` pool.
- `pool.bindProducerSlot()` returns an RAII guard pinning the caller to slot 0's CPU for a hot dispatch region.
- `pool.lowLatencyScope()` returns an RAII guard that keeps workers from parking between short bursts of dispatches.
- `pool.snapshotCounters()` reports worker counters always; pool-level counters require `CITOR_ENABLE_POOL_COUNTERS=ON` at build time.
- `pool.lastDetachedException()` returns the first exception captured from a detached body.

Standalone pools auto-pin the producer to slot 0 on Linux when the affinity mask permits. This aligns first-touch allocation with the slot-0 CCD; the auto-pin is reverted by the destructor.

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

**Nested same-pool calls** re-route through `forkJoinAll`: the inner blocks land on the calling worker's own deque so peers can steal them. No deadlock, no dispatch-mutex re-entry. Use this freely for tiled / blocked workloads.

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

**When to use it**: your work is a simple loop over a uniform range, you have meaningful work per chunk, and you want straight fan-out.

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

**Nesting**: invoked from inside another primitive on the same pool, the call falls through to inline-on-caller (single-threaded). For nested fan-out, fan out at the outer level and reduce serially per chunk.

### `parallelScan`

Two-pass Blelloch inclusive prefix scan over `[0, n)`. Returns the inclusive accumulator at the right edge.

```cpp
template <class HintsT, class T, class BodyFn, class PrefixFn>
[[nodiscard]] T parallelScan(std::size_t n, T identity,
                             BodyFn &&body, PrefixFn &&prefix,
                             CancellationToken tok = {});
```

`body(chunkId, lo, hi, initial, out)` is invoked **twice per slot** when there are at least two participants:
- Pass 1 with `initial == identity`: compute and return the chunk's partial.
- Pass 2 with `initial == exclusivePrefix[slot]`: write the per-element scan into `out[lo..hi)`.

The producer computes chunk-level exclusive prefixes serially in `O(participants)` between the two passes. Two passes avoid the `O(n^2/p)` sequential bottleneck of split-recombine; the body's per-element work runs once per pass, twice total. With `participants == 1` the call collapses to a single body invocation (single-pass shape).

**When to use it**: inclusive prefix sums or any associative scan over a contiguous buffer.

**Nesting**: same-pool reentrancy falls through to inline-on-caller; the inner scan is single-threaded.

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

**Hint knobs**: `pipelineSameChunk` (workers reuse their chunk across stages for cache locality, default true), `balance` (`StaticUniform` for same-chunk, `DynamicChunked` requires `pipelineSameChunk = false`), `chunk` (dynamic block grain when not same-chunk).

**When to use it**: your work has 2+ data-dependent stages over the same row range and you want one descriptor publish for the whole pipeline. Below that, a sequence of `parallelFor` calls is simpler and the chain has no advantage.

**Nesting**: same-pool reentrancy falls through to inline-on-caller (the chain runs single-threaded).

### `runPlex`

Persistent-worker phased loop. Workers stay live across all `nPhases` phases of one `runPlex` call instead of being woken and joined per phase. Inter-phase transitions stay in user-space spin-wait, so the per-phase rendezvous overhead is the rendezvous spin rather than a futex round-trip.

```cpp
template <class HintsT, class Phase>
void runPlex(std::size_t nPhases, std::size_t n, Phase &&phaseFn,
             CancellationToken tok = {});
```

`phaseFn(phaseIdx, slot, lo, hi)` is invoked exactly once per `(phase, slot)` pair, in stable phase-then-slot order.

**When to use it**: iterative numeric kernels (Jacobi / Gauss-Seidel / stencil sweeps) where the same partition is reused across many phases. For one-shot fan-outs `parallelFor` is cheaper because `runPlex` keeps workers spinning between phases.

**Nesting**: same-pool reentrancy falls through to inline-on-caller; the inner phased loop is single-threaded. `runPlex` is meant to be the outermost driver.

### `bulkForQueries`

Many independent queries fanned across the pool. Differs from `parallelFor` in semantics: the body must process **every** query index in the chunk, and per-query results must be written to a per-query slot keyed on the query index (chunk dispatch order varies across worker counts).

```cpp
template <class HintsT, class QueryFn>
void bulkForQueries(std::size_t q, QueryFn &&fn,
                    CancellationToken tok = {});
```

**When to use it**: spatial-index lookups, batched key/value gets, KD-tree or BVH ray queries -- workloads where per-query depth varies and a `Balance::DynamicChunked` policy amortises the skew. Use `parallelFor` when the per-item cost is uniform.

**Nesting**: same-pool reentrancy falls through to inline-on-caller. If a single query body itself wants fan-out, prefer `forkJoin` inside it (which has a first-class nested path) over another `bulkForQueries`.

### `forkJoin`

Recursive divide-and-conquer over per-worker Chase-Lev work-stealing deques. Tasks may call back into `forkJoin` from inside their bodies; nested fork-join is the headline use case. The nested call uses the Cilk-5 spawn-parent shape: children are pushed onto the calling worker's own deque (visible to peers via Chase-Lev), the last child runs inline, and the join is a per-frame `pendingTasks` counter.

```cpp
template <class HintsT, class... TaskFns>
void forkJoin(TaskFns &&...fns);

template <class HintsT, class... TaskFns>
void forkJoin(CancellationToken tok, TaskFns &&...fns);
```

The producer participates as slot 0 and steals from other workers' deques when its own drains. `Affinity::CcdLocal` (the default and the named preset `CcdLocalForkJoinHints`) biases steal probes to same-CCD victims first.

**Exception handling**: the first exception escaping any task body is captured and rethrown from the producer after the join. Subsequent throws drop. The remaining tasks are cancelled so the join doesn't block on quiescence.

**When to use it**: divide-and-conquer with non-uniform recursion (Strassen, cilksort, DAC matmul, UTS, BVH builds). For straight loops over uniform ranges, `parallelFor` has lower dispatch overhead and bigger blocks.

### `submitDetached`

Fire-and-forget. The pool's destructor blocks until every detached body has retired; until then, the pool's lifetime extends every in-flight body.

```cpp
template <class HintsT, class TaskFn>
void submitDetached(TaskFn fn, CancellationToken tok = {});
```

**Exception handling**: a throw from a detached body is captured into a per-pool slot and surfaced via `pool.lastDetachedException()`. Subsequent throws drop. The pool does not call `std::terminate` on a detached throw.

**When to use it**: tear-down work whose completion is not needed on the caller's join path, but whose retirement is needed before the pool itself can go away (logging flush, async finalisation). For anything the caller actually waits on, use a synchronous primitive.

---

## Nested calls

What happens when a synchronous primitive runs inside another primitive's body on the same pool:

| inner call (from a same-pool worker)  | behavior                                                                  |
|----------------------------------------|---------------------------------------------------------------------------|
| `parallelFor`                          | First-class nested path; inner chunks dispatch in parallel.               |
| `forkJoin`                             | First-class recursive path; children land on the calling worker's deque.  |
| `parallelReduce`                       | Same-pool reentrancy detected; inner call runs inline on the caller.      |
| `parallelScan`                         | Same as above.                                                            |
| `parallelChain`                        | Same as above.                                                            |
| `runPlex`                              | Same as above.                                                            |
| `bulkForQueries`                       | Same as above.                                                            |
| `submitDetached`                       | Always submits; not synchronous, so no reentrancy concern.                |

Cross-arena calls (worker on `PoolGroup` arena A invokes a synchronous primitive on arena B) fall through to inline-on-caller as well; the TLS participant token enforces this so cross-arena synchronous submissions cannot deadlock.

---

## Cookbook

One recipe per primitive plus two cross-cutting examples. Each shows what the primitive does that a generic thread-pool API cannot. All snippets assume `citor::ThreadPool pool;` is in scope.

### `parallelFor` -- nested fan-out for tiled work

Outer loop over row tiles, inner loop over column tiles, both via `parallelFor`. Same-pool nested `parallelFor` pushes inner chunks onto the calling worker's own deque, so peers steal them like any other work; no central dispatch lock, no participant double-count.

```cpp
void matmul(citor::ThreadPool &pool, const float *A, const float *B,
            float *C, std::size_t n) {
  constexpr std::size_t kRowTile = 32;
  constexpr std::size_t kColTile = 32;
  const std::size_t rowTiles = (n + kRowTile - 1) / kRowTile;
  const std::size_t colTiles = (n + kColTile - 1) / kColTile;

  pool.parallelFor<citor::HintsDefaults>(
      0, rowTiles,
      [&](std::size_t r0, std::size_t r1) {
        pool.parallelFor<citor::HintsDefaults>(
            0, colTiles,
            [&](std::size_t c0, std::size_t c1) {
              for (std::size_t rt = r0; rt < r1; ++rt) {
                for (std::size_t ct = c0; ct < c1; ++ct) {
                  const std::size_t i0 = rt * kRowTile;
                  const std::size_t i1 = std::min(i0 + kRowTile, n);
                  const std::size_t j0 = ct * kColTile;
                  const std::size_t j1 = std::min(j0 + kColTile, n);
                  for (std::size_t i = i0; i < i1; ++i) {
                    for (std::size_t j = j0; j < j1; ++j) {
                      float acc = 0.0f;
                      for (std::size_t k = 0; k < n; ++k) {
                        acc += A[i * n + k] * B[k * n + j];
                      }
                      C[i * n + j] = acc;
                    }
                  }
                }
              }
            });
      });
}
```

The same shape applies to image convolutions over `(rowTile, colTile)`, batched per-row transforms, and any 2D blocked workload. Pools without a first-class nested path force the user to flatten this into a 1D loop with manual `(rt, ct)` index math.

### `parallelReduce` -- bit-identical Kahan sum across worker counts

`KahanReduceHints` pins `balance = StaticUniform` and selects a chunk-id pairwise tree with Kahan/Neumaier compensation. The result is bit-identical regardless of `participants`.

```cpp
double kahanSum(citor::ThreadPool &pool, const std::vector<double> &xs) {
  return pool.parallelReduce<citor::KahanReduceHints>(
      0, xs.size(), /*init=*/0.0,
      [&](std::size_t lo, std::size_t hi) {
        double s = 0.0;
        for (std::size_t i = lo; i < hi; ++i) s += xs[i];
        return s;
      },
      [](double a, double b) { return a + b; });
}

// Contract: kahanSum on a 2-participant pool == kahanSum on a 16-participant
// pool, byte for byte, for the same input. The tree shape is keyed on chunk
// id, not on which worker happened to combine which partial.
```

Use this when you compare floating-point results across machines, between threaded and serial paths, or in a regression test that asserts exact equality. A naive `parallelReduce` with `Determinism::None` would lose those guarantees.

### `parallelScan` -- two-pass inclusive prefix scan

Real two-pass scan. Pass 1 produces per-chunk partials; the producer prefixes them in `O(participants)`; pass 2 writes the per-element scan with the chunk's exclusive prefix as `initial`.

```cpp
#include <numeric>

void inclusiveScan(citor::ThreadPool &pool,
                   const std::vector<std::int64_t> &in,
                   std::vector<std::int64_t> &out) {
  out.assign(in.size(), 0);

  if (pool.participants() == 1) {
    std::inclusive_scan(in.begin(), in.end(), out.begin());
    return;
  }

  std::atomic<std::size_t> calls{0};
  const std::size_t pn = pool.participants();

  (void)pool.parallelScan<citor::HintsDefaults>(
      in.size(), std::int64_t{0},
      [&](std::size_t /*chunkId*/, std::size_t lo, std::size_t hi,
          std::int64_t initial,
          std::int64_t * /*unusedOut*/) -> std::int64_t {
        const std::size_t call = calls.fetch_add(1, std::memory_order_acq_rel);
        if (call < pn) {
          // Pass 1: chunk partial.
          std::int64_t s = 0;
          for (std::size_t i = lo; i < hi; ++i) s += in[i];
          return s;
        }
        // Pass 2: per-element scan with the chunk's exclusive prefix.
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

The body sees `initial == identity` on pass 1 and `initial == exclusivePrefix[slot]` on pass 2; the call counter is the simplest way to tell them apart. The same shape covers stream compaction (mark, scan offsets, scatter) and output-offset allocation in batched parsing.

### `parallelChain` -- three-stage pipeline in one descriptor

Three sequential `parallelFor` calls would pay three separate dispatch round-trips. `parallelChain` publishes one descriptor and uses a per-slot done-epoch scan for the inter-stage rendezvous; workers stay live across stages. With `pipelineSameChunk = true` (the default) each worker keeps the same `[lo, hi)` across all stages, which keeps the cache hot for the row range it owns.

```cpp
pool.parallelChain<citor::ChainHintsDefaults>(
    rows,
    citor::globalStage("normalize",
        [&](std::size_t /*stage*/, std::uint32_t /*slot*/,
            std::size_t lo, std::size_t hi) {
          for (std::size_t y = lo; y < hi; ++y) normalizeRow(in, y, width);
        }),
    citor::globalStage("transform",
        [&](std::size_t /*stage*/, std::uint32_t /*slot*/,
            std::size_t lo, std::size_t hi) {
          for (std::size_t y = lo; y < hi; ++y) fftRow(in, tmp, y, width);
        }),
    citor::staticStage("emit",
        [&](std::size_t /*stage*/, std::uint32_t /*slot*/,
            std::size_t lo, std::size_t hi) {
          for (std::size_t y = lo; y < hi; ++y) emitRow(tmp, out, y, width);
        }));
```

`globalStage` rendezvous all participants before the next stage starts. `staticStage` skips the post-stage barrier when the next stage doesn't need it (for the emit stage above, there is no next stage). The descriptor lives on the producer's stack for the whole call.

### `runPlex` -- persistent-worker stencil iteration

Heat equation with ping-pong buffers. Phase parity selects source and destination, so workers never write to a buffer they're reading. Across thousands of phases, `runPlex` keeps workers in user-space rendezvous between phases instead of paying a wake/park round-trip per step.

```cpp
std::vector<float> a(n), b(n);
// init a[]

constexpr std::size_t kPhases = 10000;
pool.runPlex<citor::HintsDefaults>(
    kPhases, n - 2,
    [&](std::size_t phase, std::uint32_t /*slot*/,
        std::size_t lo, std::size_t hi) {
      const auto &src = (phase & 1U) ? b : a;
      auto &dst = (phase & 1U) ? a : b;
      for (std::size_t i = lo + 1; i < hi + 1; ++i) {
        dst[i] = 0.5f * src[i] + 0.25f * (src[i - 1] + src[i + 1]);
      }
    });

const auto &result = (kPhases & 1U) ? b : a;
```

A `parallelFor` loop with `kPhases` iterations would wake and park workers `kPhases` times. `runPlex` publishes one descriptor and keeps workers spinning between phases; the per-phase cost is a rendezvous spin, not a syscall. The same shape covers Game of Life, Gauss-Seidel sweeps, and any iterative kernel over a stable partition.

### `bulkForQueries` -- variable-cost batched ray queries

One independent query per index. Per-query traversal depth varies, so static partitioning leaves slow tails idle. `DynamicHints` selects the atomic-counter chunk policy: a worker that finishes its block fast keeps pulling more.

```cpp
pool.bulkForQueries<citor::DynamicHints>(
    rays.size(),
    [&](std::size_t lo, std::size_t hi) {
      for (std::size_t q = lo; q < hi; ++q) {
        out[q] = bvh.intersect(rays[q]);   // each query writes its own slot
      }
    });
```

Result must be keyed by query index (`out[q]`), not by chunk order: a fast worker can finish many adjacent queries while a slow worker is still in one. Same shape works for spatial-hash lookups, KD-tree nearest-neighbor, and per-row sparse matrix-vector products.

### `forkJoin` -- recursive cilksort with Chase-Lev steal

Each worker has its own Chase-Lev deque. Each `forkJoin` level pushes children onto the calling worker's deque, runs one inline, and lets peers steal the rest. `CcdLocalForkJoinHints` biases steal probes toward same-CCD victims so transferred work stays L3-local.

```cpp
template <class It>
void cilksort(citor::ThreadPool &pool, It first, It last) {
  const auto n = std::distance(first, last);
  if (n < 2048) {
    std::sort(first, last);
    return;
  }
  const auto mid = first + n / 2;
  pool.forkJoin<citor::CcdLocalForkJoinHints>(
      [&] { cilksort(pool, first, mid); },
      [&] { cilksort(pool, mid, last); });
  std::inplace_merge(first, mid, last);
}
```

The recursion is unbounded in shape; the steal protocol is what keeps an unbalanced partition from stalling the slowest leaf. The same shape covers Strassen multiplication, BVH builds, branch-and-bound search (pair with a `CancellationToken` to prune), and recursive divide-and-conquer matrix algorithms.

### `submitDetached` -- background work the destructor waits for

`submitDetached` returns immediately. The pool destructor blocks on the detached counter, so a detached body can outlive the calling scope but cannot outlive the pool. Exceptions are captured per-pool and surfaced via `lastDetachedException()` instead of calling `std::terminate`.

```cpp
class LogSink {
 public:
  void flushAsync(citor::ThreadPool &pool) {
    pool.submitDetached<citor::HintsDefaults>([this] {
      this->flushBufferToDisk();
    });
  }
};

// Somewhere before the pool goes out of scope:
if (auto eptr = pool.lastDetachedException()) {
  std::rethrow_exception(eptr);   // first exception captured from any detached body
}
```

Use this for tear-down work whose completion is not needed on the caller's join path: log flushes, metrics writes, async finalisation. For anything the caller actually waits on, use a synchronous primitive.

### Cross-cutting: per-CCD sharding via `PoolGroup`

`PoolGroup::global()` constructs one arena per CCD (shared-L3 cluster) at first use. Workers in arena `i` are pinned to CCD `i`'s cores; `arena(i).parallelFor(...)` keeps memory traffic on that CCD's L3 instead of crossing the inter-CCD fabric. Drive each arena from its own producer thread to run all CCDs in parallel.

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

Worker-on-arena-A calling into arena B falls through to inline-on-caller via the TLS participant token; cross-arena work never enqueues onto a queue the caller doesn't service.

### Cross-cutting: cancellation with deadline

Pass the same `CancellationToken` through nested calls and they all observe the stop signal at chunk boundaries. `Deadline` is a TSC-calibrated absolute threshold (single calibration per process), so `expired()` is a few cycles. `cancellationChecks = false` compiles out the per-chunk poll for tokens known never to stop.

```cpp
auto tok = citor::CancellationToken::makeOwned();
auto deadline = citor::Deadline::fromMillis(50);

pool.submitDetached<citor::HintsDefaults>([tok, deadline]() mutable {
  while (!deadline.expired()) std::this_thread::yield();
  tok.request_stop();
});

try {
  pool.parallelFor<citor::HintsDefaults>(
      0, n,
      [&](std::size_t lo, std::size_t hi) {
        // Inner forkJoin propagates the same token.
        pool.forkJoin<citor::CcdLocalForkJoinHints>(
            tok,
            [&] { searchHalf(lo, (lo + hi) / 2, tok); },
            [&] { searchHalf((lo + hi) / 2, hi, tok); });
      },
      tok);
} catch (const citor::cancelled_exception &) {
  // void primitives throw cancelled_exception on a stopped token.
}
```

Branch-and-bound search, deadline-bounded computation, and any "stop when good enough" pattern fits this shape. `parallelReduce` cancels by throwing `cancelled_value_exception<T>` carrying the deterministic combine of chunks completed before the stop.

---

## Hints

Each compile-time primitive call templates on a hint type. Start from a preset and override only what differs:

```cpp
struct MyHints : citor::HintsDefaults {
  static constexpr citor::Affinity affinity  = citor::Affinity::CcdLocal;
  static constexpr double          minTaskUs = 25.0;
  static constexpr std::size_t     chunk     = 4096;
};
```

### Hint fields

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

### Presets

| preset                    | what it changes                                           | use when |
|---------------------------|-----------------------------------------------------------|----------|
| `HintsDefaults`           | the defaults above                                        | every primitive's first cut. |
| `StaticHints`             | `balance = StaticUniform`                                 | uniform-cost loops that benefit from the typed monomorphised fast path. |
| `DynamicHints`            | `balance = DynamicChunked`                                | a stable name for the future-proof default. |
| `LatencyHints`            | `priority = Latency`                                      | short jobs that want fast first response over peak throughput. |
| `BulkHints`               | `minTaskUs = 25.0`, `cancellationChecks = false`          | hot uniform-cost loops with no cancellation. |
| `KahanReduceHints`        | `determinism = KahanCompensated`, `minTaskUs = 25.0`      | numerically sensitive sums (`parallelReduce`). |
| `FixedBlockReduceHints`   | `minTaskUs = 25.0`                                        | integer or order-insensitive reductions (`parallelReduce`). |
| `CcdLocalForkJoinHints`   | `affinity = CcdLocal`                                     | recursive fork-join workloads with cross-CCD locality. |
| `ChainHintsDefaults`      | chain shape: `balance = StaticUniform`, `pipelineSameChunk = true` | most chains. |
| `DynamicChainHints`       | chain shape: `balance = DynamicChunked`, `pipelineSameChunk = false` | stage packs with skewed bodies and only Global / DeterministicReduce barriers. |

Runtime hint structs (`citor::Hints`, `citor::ChainHints`) carry the same fields and feed the `*Runtime` siblings of every primitive. They use the same engine; the runtime path trades compile-time monomorphisation for an in-register hint struct.

## Cancellation and deadlines

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

Exception types:

- `cancelled_exception`: thrown by void-returning synchronous primitives that stop mid-flight.
- `cancelled_value_exception<T>`: thrown by value-returning primitives such as `parallelReduce`; carries `partial_value`, the deterministic combine of the chunks that completed before cancellation.

`forkJoin` cancels by clamping the outstanding count and returning normally; tasks that haven't started just don't run.

The `cancellationChecks` hint compiles out the per-chunk poll for tokens that are statically known not to stop. Pair it with the never-stopped sentinel for hot loops where cancellation is not a possibility.

`citor::Deadline` is a TSC-based absolute threshold for cooperative deadline checks:

```cpp
citor::Deadline d0;                                  // never expires
citor::Deadline d1 = citor::Deadline::fromMillis(5);
citor::Deadline d2 = citor::Deadline::fromMicros(250);
bool expired = d1.expired();
```

On x86_64, `Deadline` calibrates cycles per nanosecond once per process and uses `__rdtsc()` / `rdtscp` checks. On non-x86 builds, deadline factories collapse to the never-expires sentinel. The intended pattern is to set the deadline, hand the `CancellationToken` to the primitive, and let a watchdog (or the body itself) call `tok.request_stop()` once `deadline.expired()`.

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

`PoolGroup::global()` is lazy, thread-safe by C++ function-local-static initialization, and intentionally never destroyed. Use a stack `PoolGroup` when you want bounded worker lifetime.

## Diagnostics and counters

`CITOR_ENABLE_POOL_COUNTERS=ON` compiles in pool-level counters (dispatches, inline fallbacks, cancellation stops). With it OFF the hot-path increments compile out. Worker-level park/wake counters are always available through `snapshotCounters()`.

```cpp
const auto before = pool.snapshotCounters();
pool.parallelFor<citor::HintsDefaults>(0, n, body);
const auto after = pool.snapshotCounters();

const auto inlineFalls = after.inlineFallbacks - before.inlineFallbacks;
```

Use counters for regression tests, benchmark context, and diagnosing unexpected inline fallback. Pool-level counter drift between two snapshots is the cleanest signal that a `*Hints` change accidentally shifted work onto the producer.

## Build, test, and release workflow

### CMake options

| option                          | default      | effect |
|---------------------------------|--------------|--------|
| `CITOR_BUILD_TESTS`             | ON top-level | Build the GTest suite. |
| `CITOR_BUILD_BENCHMARK`         | ON top-level | Build the comparative bench (fetches ten peer pools via CPM; first cold configure is 5-10 minutes). |
| `CITOR_USE_AVX2`                | ON           | Compile with `-mavx2 -mfma`, define `CITOR_USE_AVX2`. Auto-detected via `check_cxx_compiler_flag`. |
| `CITOR_BUILD_WITH_SANITIZER`    | OFF          | Build with `-fsanitize=thread -fno-omit-frame-pointer`. |
| `CITOR_ENABLE_POOL_COUNTERS`    | OFF          | Compile in pool-level diagnostic counters. Hot path pays no extra atomics when OFF. |
| `CITOR_WORKER_STACK_KIB`        | `8192`       | Per-worker pthread stack size (KiB). |

clang-tidy is not a build option -- it runs as a pre-commit hook (over staged files) and as a dedicated CI job (over the whole tree). When citor is consumed via `add_subdirectory(...)` or CPM, all of the above default to OFF -- the consumer gets the public INTERFACE target only.

### Build and test locally

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

# Single test binary:
ctest --test-dir build --output-on-failure -R parallel_for_test

# Single GTest case:
./build/tests/parallel_for_test --gtest_filter='ParallelFor.SmallRange*'
```

### Formatting and linting

Top-level builds wire optional `format` and `check-format` targets when `clang-format` is available:

```bash
cmake --build build --target check-format
cmake --build build --target format
```

Pre-commit hooks (install via `uv run pre-commit install`) cover trailing whitespace, end-of-file fixes, YAML, large files, mixed line endings, merge-conflict markers, `clang-format`, the `gersemi` CMake formatter, and `commitizen` on `commit-msg`. CI runs the same hooks plus a separate clang-tidy job over the whole tree.

### Single-header generation

```bash
python3 tools/amalgamate.py
python3 tools/amalgamate.py --check
```

The release workflow regenerates the single header after a Commitizen bump and amends the release commit if `single_include/citor.hpp` changed.

### Versioning and release

Commitizen owns version bumps. `pyproject.toml` lists every file that must carry the current version string, including CMake, `version.h`, `CITATION.cff`, README install snippets, Conan, and vcpkg metadata. The release workflow has two paths: CI success on `main` triggers a Commitizen auto-bump, single-header refresh, push, and GitHub release; a manual `v*` tag push regenerates the single header and creates a release artifact from the tag.

## Benchmarks

The bench harness measures dispatch latency and per-primitive throughput against ten peer pools (BS, dp, task, riften, oneTBB, Taskflow, Eigen, OpenMP, Leopard, dispenso); two coroutine schedulers (libfork, TooManyCooks) join the recursive fork-join cells. Numbers are not embedded in this README -- they age out on the next compiler / microarchitecture bump.

The full sweep fetches ten peer pools and runs roughly 94 workloads; wall-clock is 40-50 minutes on a 16-core CCD. Use `python -m tools.bench isolated` to run each cell in its own process so a competitor's segfault does not kill the whole run.

```bash
cmake -S . -B build -G Ninja \
  -DCITOR_BUILD_BENCHMARK=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --target parallel_bench -j

# Full sweep, JSON export, taskset to one CCD:
taskset -c 0-15 ./build/benchmark/parallel_bench \
  --export bench_out/results.json

# Plot: SVG per workload family.
python -m tools.plot_bench \
  --input bench_out/results.json \
  --output charts/

# Or drive the same flow through tools.bench with host-tagged output:
python -m tools.bench run
```

The harness probes host invariants (governor, turbo, SMT, ASLR, libomp blocktime) at startup and flags any failed gate in the table output and the JSON `context` block. For serious latency numbers, set the cpufreq governor to `performance`, disable boost, disable ASLR (`/proc/sys/kernel/randomize_va_space=0`), and pin process affinity. The benchmark tree includes workloads for parallel-for dispatch / granularity / matmul / Pareto bodies, reductions, scans, chains, runPlex heat / stencil, fork-join recursive workloads, bulk queries, differential comparisons, and two-pool OpenMP coexistence.

## Supported targets

- **Validated and CI-backed**: Ubuntu 24.04, GCC 14, Clang 18, C++20, Linux x86_64.
- **Packaging metadata**: vcpkg overlay marks `linux & x64`; the Conan recipe warns when `os != Linux` or `arch != x86_64`.
- **Kernel**: Linux 6.x with `futex` and sysfs `cpu/cpuX/cache/index*` is the validated base.
- **AVX2**: auto-detected via `check_cxx_compiler_flag`; the pool builds without it but loses the AVX2-tuned code paths.

Implementation notes for non-validated paths:

- Linux uses raw futex parking and pthread affinity. Non-Linux compile fallbacks (std::thread + condition-variable parking) exist but are not the validated latency target.
- x86_64 uses TSC-based deadline / spin helpers and `_mm_pause`. Non-x86 collapses `Deadline` factories to the never-expires sentinel and uses a compiler-barrier spin hint.
- AVX2/FMA flags are added by CMake when `CITOR_USE_AVX2=ON` and the compiler accepts them; the flag is an optimization toggle, not part of the support contract.

## License

MIT. See [`LICENSE`](LICENSE).
