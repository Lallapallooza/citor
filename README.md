# citor

> Header-only C++20 thread pool with sub-microsecond dispatch on Linux x86_64. Eight cooperating primitives, decentralized per-slot done-epoch barriers, Chase-Lev work-stealing, per-CCD arenas. MIT.

[![ci](https://github.com/Lallapallooza/citor/actions/workflows/ci.yml/badge.svg)](https://github.com/Lallapallooza/citor/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)

| | |
|---|---|
| Version | `0.1.0` |
| Distribution | header-only |
| CMake target | `citor::citor` (INTERFACE) |
| Validated target | Linux x86_64 + AVX2 |
| Compilers | GCC 14, Clang 18 (CI-backed) |
| C++ standard | C++20 |
| Runtime deps | `Threads::Threads` / pthread |
| License | MIT |

The name comes from Latin *cito* -- "swiftly, quickly".

---

## Table of contents

- [What citor is](#what-citor-is)
- [Hard contract](#hard-contract)
- [vs other thread pools](#vs-other-thread-pools)
- [Performance shape](#performance-shape)
- [Install](#install)
- [Quick start](#quick-start)
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
- [Repository layout](#repository-layout)
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

## Hard contract

These points are API contract, not implementation trivia.

- **Header-only.** Including modular headers under `include/citor/` or the generated `single_include/citor.hpp` is enough; there is no library binary to link. Linked C++ runtime + `pthread` are the only runtime dependencies.
- **CPU-bound and synchronous.** No `co_await` surface, no future, no I/O reactor. Bodies that block on I/O, sleeps, or external locks defeat the latency contract.
- **`ThreadPool(participants)` is the total participant count, including the calling thread.** A pool of `8` runs the caller plus `7` background pthreads, subject to topology and affinity-mask clamping. Query the effective count with `pool.participants()`. `participants == 0` throws `std::invalid_argument`.
- **Closure lifetime >= call lifetime.** Every primitive captures the body via a 16-byte non-owning `FunctionRef`. The callable must outlive the synchronous call. Captures in the producer's stack frame satisfy this naturally.
- **Producer participates as slot 0.** Single-participant pools fall through to the inline path and never wake a worker.
- **`PoolGroup::global()` is one arena per CCD.** Cross-arena synchronous calls fall through to inline on the caller (a TLS participant token enforces the rule); they never deadlock.
- **`ThreadPool` is non-copyable, non-movable.** Workers hold interior pointers to per-instance state.
- **Empty ranges are silent no-ops.** `parallelFor(0, 0, body)`, `parallelReduce(0, 0, init, ...)`, `parallelScan(0, ...)`, `runPlex(0, ...)`, `bulkForQueries(0, ...)` all return without invoking the body. Inverted ranges (`first > last`) collapse the same way.
- **Concurrent producers are safe.** Two threads calling primitives on the same pool serialize through the dispatch lease. `Hints::priority` arbitrates: `Latency` jumps the queue, `Background` yields. Single-producer pools never see priority effects.
- **Cancellation is cooperative.** A stop request is observed at primitive-defined boundaries, not by preempting a running body. Void-returning primitives early-return on stop; only `parallelReduce` throws (`cancelled_value_exception<T>` carrying the deterministic partial).
- **Nested parallelism is safe everywhere.** `parallelFor` and `forkJoin` have first-class same-pool nested paths (children land on the calling worker's deque). Other synchronous primitives detect same-pool reentrancy and fall through to inline-on-caller -- safe, but the inner call is not parallel.

## vs other thread pools

| capability                                            | citor | BS::thread_pool | oneTBB         | Taskflow        | folly Executor | OpenMP   |
|-------------------------------------------------------|:-----:|:---------------:|:--------------:|:---------------:|:--------------:|:--------:|
| Recursive fork-join over per-worker work-stealing deques | yes | no | yes (`task_group`) | yes (`Subflow`) | no | tied tasks |
| Multi-stage pipeline in **one** dispatch descriptor   | yes (`parallelChain`) | no | yes (`parallel_pipeline`) | yes (`Pipeline`) | no | no |
| Workers persistent across N phases (no wake/park)     | yes (`runPlex`) | no | partial | partial | no | partial |
| Per-CCD / shared-L3 arenas with TLS guard             | yes (`PoolGroup`) | no | task arenas (no L3 grouping) | no | no | no |
| Bit-identical reduce across worker counts             | yes (`Determinism::FixedBlockOrder`) | no | partial | no | no | no |
| Sub-microsecond fan-out floor on Linux x86_64         | yes | no | yes | yes | no | yes |
| Header-only                                           | yes | yes | no | yes | no | runtime |
| Producer participates as slot 0 (no caller wake)      | yes | no | no | no | no | no |

`citor` is not aiming to replace any single peer; it's a different shape. If your workload is one-shot throughput fan-out over uniform ranges, `BS::thread_pool` and `OpenMP` are simpler. citor's surface earns its keep on workloads that combine **short phases**, **deterministic reductions**, **recursive irregular work**, and **CCD-aware locality** in the same library, behind a header-only INTERFACE target.

## Performance shape

Numbers age out on every microarchitecture and compiler bump. The shape is what's stable:

- **Empty fan-out floor.** Hundreds of nanoseconds for a `parallelFor` call where the body has nothing to do; the pool reaches that floor by keeping the descriptor on the producer's stack and avoiding any allocation on the hot path.
- **Single-CCD vs cross-CCD.** Within one CCD (16 cores on a Zen 5 chiplet), wake-up to first body invocation stays under a microsecond. Cross-CCD adds the inter-fabric latency once at the start; workers don't cross CCDs during a call.
- **Persistent-worker amortisation.** `runPlex` collapses N phases into one dispatch; per-phase overhead drops from a futex round-trip to a user-space rendezvous spin (typically a few hundred nanoseconds depending on participant count and contention).
- **Inline fallback.** When `n * estimatedItemNs * 1e-3 < minTaskUs * participants`, the pool runs the call inline on the producer with zero wake. Set `minTaskUs > 0` and a non-zero `estimatedItemNs` on hot paths where the dispatch floor matters.

`benchmark/parallel_bench` measures the actual numbers on your hardware against ten peer pools and exports JSON suitable for `tools.plot_bench`. See [Benchmarks](#benchmarks) for the recipe.

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

The producer is slot 0; with one participant the call collapses to an inline loop and never wakes a worker. The body lives on the producer's stack for the call.

Both `pool.parallelFor(...)` and the free CPO `citor::parallelFor` are public surfaces -- see [Public API shape](#public-api-shape) for when each is the right spelling.

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

Each `<citor/cpos/...>` header exposes the primitive as a function-object value built on `tag_invoke`. The function object is not itself a function template, so supplying the hint type uses the templated-call-operator spelling:

```cpp
#include <citor/cpos/parallel_for.h>

citor::parallelFor.template operator()<citor::HintsDefaults>(
    pool, 0, n,
    [&](std::size_t lo, std::size_t hi) { /* ... */ });
```

The shorter form `citor::parallelFor<citor::HintsDefaults>(pool, ...)` does NOT compile -- `parallelFor` is a value, not a template. Use the member surface in application code; reach for the CPO surface for generic executor adapters and tests that need `tag_invoke` dispatch.

## ThreadPool lifecycle

```cpp
explicit ThreadPool(std::size_t participants);
```

Construction probes sysfs topology, prefers one logical CPU per physical core, clamps the requested count to the usable affinity mask, allocates per-slot worker state, creates one Chase-Lev deque per participant, and spawns `participants - 1` background pthreads with pre-bound affinity, raw futex parking, and a configured pthread stack size.

Lifecycle points worth knowing:

- `participants == 0` throws `std::invalid_argument`. Construction may also throw `std::system_error` when pthread setup fails.
- The pool is non-copyable and non-movable.
- Destruction first waits for `submitDetached` work to retire (so detached bodies can still touch pool state), then signals shutdown, wakes parked workers, joins them, and finally restores any producer auto-pin the pool owns.
- `pool.participants()` returns the effective count after topology clamping.
- `pool.kind()` distinguishes a user-constructed `Standalone` pool from a `PoolGroup::global()` `Arena` pool.
- `pool.bindProducerSlot()` returns an RAII guard pinning the caller to slot 0's CPU for a hot dispatch region.
- `pool.lowLatencyScope()` returns an RAII guard that keeps workers from parking between short bursts of dispatches.
- `pool.snapshotCounters()` reports worker counters always; pool-level counters require `CITOR_ENABLE_POOL_COUNTERS=ON` at build time.
- `pool.lastDetachedException()` returns the first exception captured from a detached body. The destructor blocks on the in-flight counter but does **not** rethrow -- call this proactively to observe detached failures.
- `pool.producerCpu()`, `pool.ccdCount()`, `pool.arenaIndex()`, and the static `ThreadPool::workerIndex()` / `ThreadPool::insidePoolWorker()` / `ThreadPool::currentArenaIndexHint()` expose topology and TLS state for libraries layering on top.

Standalone pools auto-pin the producer to slot 0 on Linux when the affinity mask permits. This aligns first-touch allocation with the slot-0 CCD; the auto-pin is reverted by the destructor.

## Primitives

Every primitive is reachable as a member of `citor::ThreadPool` and as a free-standing CPO that dispatches through `tag_invoke`. Both spellings dispatch into the same engine. The compile-time hint type carries the policy (balance, determinism, affinity, priority, cost gates, chunk grain) so each call site picks its own policy without runtime branching.

### `parallelFor`

Bulk fan-out over a uniform `[first, last)` range. The most-used primitive; covers contiguous-range bulk work.

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
void scaleVector(citor::ThreadPool &pool, std::vector<float> &v, float k) {
  pool.parallelFor<citor::HintsDefaults>(
      0, v.size(),
      [&](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) v[i] *= k;
      });
}
```

For runtime hints (build a `Hints` POD at run time):

```cpp
citor::Hints h;
h.balance = citor::Balance::DynamicChunked;
h.minTaskUs = 25.0;
pool.parallelForRuntime(0, v.size(), body, h);
```

**When to use it**: your work is a simple loop over a uniform range and you want straight fan-out.

**When to use something else**: deep recursion -> `forkJoin`. Multi-stage pipeline -> `parallelChain`. Iterated phases over the same partition -> `runPlex`.

### `parallelReduce`

Parallel reduction over `[first, last)` with deterministic combine semantics.

```cpp
template <class HintsT, class T, class Map, class Combine>
[[nodiscard]] T parallelReduce(std::size_t first, std::size_t last, T init,
                               Map &&map, Combine &&combine,
                               CancellationToken tok = {});
```

`map(lo, hi)` produces the partial value for one chunk; `combine(a, b)` combines two partials. The reduce engine internally forces `Balance::StaticUniform` regardless of `HintsT::balance` so chunk-id-to-rank mapping is stable; the combine runs as a chunk-id pairwise tree, so the result is bit-identical across worker counts when `Determinism::FixedBlockOrder` or `Determinism::KahanCompensated` is requested.

**Determinism shapes** (selected on `HintsT::determinism`):
- `Determinism::FixedBlockOrder` -- chunk-id pairwise tree combine.
- `Determinism::KahanCompensated` -- Kahan/Neumaier compensated FP sum on top of the fixed-block tree.

The reduce-side hint presets (`KahanReduceHints`, `FixedBlockReduceHints`) wire the determinism field plus a `minTaskUs = 25.0` floor; they leave `balance` at its default since the engine overrides it anyway.

**Cancellation**: a stopped token throws `cancelled_value_exception<T>` carrying the deterministic combine of the chunks that completed before the cancellation was observed. This is the **only** primitive that throws on cancellation -- void primitives early-return.

**Nesting**: invoked from inside another primitive on the same pool, the call falls through to inline-on-caller (single-threaded). For nested fan-out, fan out at the outer level and reduce serially per chunk.

### `parallelScan`

Two-pass Blelloch inclusive prefix scan over `[0, n)`. Returns the inclusive accumulator at the right edge.

```cpp
template <class HintsT, class T, class BodyFn, class PrefixFn>
[[nodiscard]] T parallelScan(std::size_t n, T identity,
                             BodyFn &&body, PrefixFn &&prefix,
                             CancellationToken tok = {});
```

The body has signature `T body(std::size_t chunkId, std::size_t lo, std::size_t hi, T initial, T *reserved)`. The trailing `reserved` parameter is **always `nullptr`** -- it exists only to keep the signature stable; the body owns its output buffer through capture. The body is invoked **twice per slot** when there are at least two participants:

- Pass 1 with `initial == identity`: compute and return the chunk's partial.
- Pass 2 with `initial == exclusivePrefix[slot]`: write the per-element scan into the user's captured buffer; the return value is the chunk contribution.

Distinguish passes via a simple atomic call counter (the canonical idiom; see the Cookbook). The producer computes chunk-level exclusive prefixes serially in `O(participants)` between the two passes. With `participants == 1` or `n` below the inline threshold, the call collapses to a single body invocation.

**Nesting**: same-pool reentrancy falls through to inline-on-caller; the inner scan is single-threaded.

### `parallelChain`

Multi-stage pipeline. One descriptor publish covers the entire chain; per-stage rendezvous is fully decentralized (per-slot done-epoch scan).

```cpp
template <class ChainHintsT, class... Stages>
void parallelChain(std::size_t n, Stages &&...stages);

template <class ChainHintsT, class... Stages>
void parallelChainWithToken(std::size_t n, const CancellationToken &tok,
                            Stages &&...stages);
```

The cancellation overload is named `parallelChainWithToken` because the variadic `Stages` pack would otherwise absorb the leading token argument.

Each stage is built with one of the helpers from `<citor/chain.h>`:
- `staticStage(name, fn)`         -- `BarrierKind::None` (no rendezvous after).
- `globalStage(name, fn)`         -- `BarrierKind::Global` (rendezvous across all slots).
- `reduceStage(name, fn)`         -- `BarrierKind::DeterministicReduce`.
- `serialStage(name, fn)`         -- `BarrierKind::ProducerSerial` (rank 0 runs serially while others spin).
- `makeStage<BarrierKind::X>(fn)` -- explicit barrier kind without a name; underlying type is `Stage<F, BarrierKind>`.

The stage body signature is `void(stageIdx, slot, lo, hi)`. Empty stage packs and `n == 0` are no-ops.

**Hint knobs**: `pipelineSameChunk` (workers reuse their chunk across stages for cache locality, default `true`), `balance`, `chunk`. With `pipelineSameChunk = false`, chains made entirely of `Global` / `DeterministicReduce` stages opt into per-stage chunk claiming via `Balance::DynamicChunked`; chains containing any `None` or `ProducerSerial` stage silently fall back to the same-chunk engine.

**When to use it**: 2+ data-dependent stages over the same row range where you want one descriptor publish. A sequence of separate `parallelFor` calls is simpler and the chain has no advantage when the per-stage body is large.

**Nesting**: same-pool reentrancy falls through to inline-on-caller (the chain runs single-threaded).

### `runPlex`

Persistent-worker phased loop. Workers stay live across all `nPhases` phases of one `runPlex` call; inter-phase transitions stay in user-space rendezvous.

```cpp
template <class HintsT, class Phase>
void runPlex(std::size_t nPhases, std::size_t n, Phase &&phaseFn,
             CancellationToken tok = {});
```

`phaseFn(phaseIdx, slot, lo, hi)` is invoked exactly once per `(phase, slot)` pair, in stable phase-then-slot order.

**When to use it**: iterative numeric kernels (Jacobi / Gauss-Seidel / stencil sweeps), simulation tick loops, cellular automata -- workloads where the same partition is reused across many phases. For one-shot fan-outs `parallelFor` is cheaper because `runPlex` keeps workers spinning between phases.

**Nesting**: same-pool reentrancy falls through to inline-on-caller; the inner phased loop is single-threaded. `runPlex` is meant to be the outermost driver.

### `bulkForQueries`

Many independent queries fanned across the pool. Differs from `parallelFor` in semantics: the body must process **every** query index in the chunk, and per-query results must be written to a per-query slot keyed on the query index (chunk dispatch order varies across worker counts).

```cpp
template <class HintsT, class QueryFn>
void bulkForQueries(std::size_t q, QueryFn &&fn,
                    CancellationToken tok = {});
```

**When to use it**: spatial-index lookups, batched key/value gets, KD-tree or BVH ray queries -- workloads where per-query depth varies and `Balance::DynamicChunked` (the default for `bulkForQueries`) amortises the skew. Use `parallelFor` when the per-item cost is uniform.

**Nesting**: same-pool reentrancy falls through to inline-on-caller. If a single query body itself wants fan-out, prefer `forkJoin` inside it over another `bulkForQueries`.

### `forkJoin`

Recursive divide-and-conquer over per-worker Chase-Lev work-stealing deques. Tasks may call back into `forkJoin` from inside their bodies; nested fork-join is the central use case. The nested call uses the Cilk-5 spawn-parent shape: children are pushed onto the calling worker's own deque (visible to peers via Chase-Lev), the last child runs inline, and the join is a per-frame `pendingTasks` counter.

```cpp
template <class HintsT, class... TaskFns>
void forkJoin(TaskFns &&...fns);

template <class HintsT, class... TaskFns>
void forkJoin(CancellationToken tok, TaskFns &&...fns);
```

The producer participates as slot 0 and steals from other workers' deques when its own drains. `Affinity::CcdLocal` (the default and the named preset `CcdLocalForkJoinHints`) biases steal probes to same-CCD victims first.

**Exception handling**: the first exception escaping any task body is captured and rethrown from the producer after the join. Subsequent throws drop. The remaining tasks are cancelled so the join doesn't block on quiescence.

**When to use it**: divide-and-conquer with non-uniform recursion (Strassen, cilksort, BVH builds, branch-and-bound, octree splits). For straight loops over uniform ranges, `parallelFor` has lower dispatch overhead and bigger blocks.

### `submitDetached`

Fire-and-forget. The pool's destructor blocks until every detached body has retired; until then, the pool's lifetime extends every in-flight body.

```cpp
template <class HintsT, class TaskFn>
void submitDetached(TaskFn fn, CancellationToken tok = {});
```

**Exception handling**: a throw from a detached body is captured into a per-pool slot and surfaced via `pool.lastDetachedException()`. The first throw latches; subsequent throws are silently dropped. The destructor blocks on the in-flight counter but does **not** rethrow -- a caller that cares must call `lastDetachedException()` proactively. The pool does not call `std::terminate` on a detached throw.

**When to use it**: tear-down work whose completion is not on the caller's join path: log flushes, metrics writes, async finalisation. For anything the caller actually waits on, use a synchronous primitive.

### Nested calls

What happens when a synchronous primitive runs inside another primitive's body on the same pool:

| inner call (from a same-pool worker)              | behavior                                                                  |
|---------------------------------------------------|---------------------------------------------------------------------------|
| `parallelFor`                                     | First-class nested path; inner chunks dispatch in parallel.               |
| `forkJoin`                                        | First-class recursive path; children land on the calling worker's deque.  |
| `parallelFor` inside a `forkJoin` body            | Same first-class nested path; inner blocks become deque entries.          |
| `parallelReduce`                                  | Same-pool reentrancy detected; inner call runs inline on the caller.      |
| `parallelScan`                                    | Same as above.                                                            |
| `parallelChain`                                   | Same as above.                                                            |
| `runPlex`                                         | Same as above.                                                            |
| `bulkForQueries`                                  | Same as above.                                                            |
| `submitDetached`                                  | Always submits; not synchronous, so no reentrancy concern.                |

Cross-arena calls (worker on `PoolGroup` arena A invokes a synchronous primitive on arena B) fall through to inline-on-caller as well; the TLS participant token enforces this so cross-arena synchronous submissions cannot deadlock.

---

## Cookbook

Each recipe is a real-world scenario, not a primitive demo. The "Why this primitive" line at the end of each tells you what citor does that a generic thread-pool API cannot. All snippets assume `citor::ThreadPool pool;` is in scope.

### Audio buffer per-sample gain (`parallelFor`)

An audio engine applies per-sample gain or limiter to a 48 kHz interleaved stereo buffer, called per block from the audio callback. Uniform cost per sample, no recursion.

```cpp
void applyGain(citor::ThreadPool &pool, float *interleaved,
               std::size_t frames, std::size_t channels, float gain) {
  pool.parallelFor<citor::HintsDefaults>(
      0, frames,
      [&](std::size_t lo, std::size_t hi) {
        for (std::size_t f = lo; f < hi; ++f)
          for (std::size_t c = 0; c < channels; ++c)
            interleaved[f * channels + c] *= gain;
      });
}
```

For tiled 2D workloads (image kernel over `(rowTile, colTile)`, spatial filter, batched per-row transform), nest two `parallelFor` calls -- same-pool nested calls push inner chunks onto the calling worker's deque so peers steal them, no central dispatch lock, no participant double-count, no flatten-into-1D index math:

```cpp
pool.parallelFor<citor::HintsDefaults>(
    0, rowTiles,
    [&](std::size_t r0, std::size_t r1) {
      pool.parallelFor<citor::HintsDefaults>(
          0, colTiles,
          [&](std::size_t c0, std::size_t c1) {
            for (std::size_t rt = r0; rt < r1; ++rt)
              for (std::size_t ct = c0; ct < c1; ++ct)
                applyTileKernel(image, rt, ct);  // your micro-kernel
          });
    });
```

**Why this primitive.** `parallelFor` over a uniform range is the most common shape; what citor adds is a first-class same-pool nested path so 2D tile loops stay readable.

### Bit-identical portfolio NPV (`parallelReduce`)

End-of-day risk system aggregates discounted cashflows across N instruments. The number must reproduce byte-for-byte across runs **and** across worker counts (audit / regression / cross-environment comparison).

```cpp
double portfolioNpv(citor::ThreadPool &pool,
                    std::span<const Instrument> book, double rate) {
  return pool.parallelReduce<citor::KahanReduceHints>(
      0, book.size(), 0.0,
      [&](std::size_t lo, std::size_t hi) {
        double s = 0.0;
        for (std::size_t i = lo; i < hi; ++i)
          s += discountedCashflow(book[i], rate);
        return s;
      },
      [](double a, double b) { return a + b; });
}

// Contract: portfolioNpv on a 2-participant pool == portfolioNpv on a
// 16-participant pool, byte for byte, for the same input. The combine
// tree is keyed on chunk id, not on which worker ran which chunk.
```

**Why this primitive.** `KahanReduceHints` selects `Determinism::KahanCompensated` on a chunk-id pairwise tree; the engine internally pins `StaticUniform` so chunk-id-to-rank mapping is stable. Most pools do not promise byte-equal results across worker counts.

### Output offsets for event filtering (`parallelScan`)

A telemetry consumer ingests N events per batch, filters by predicate, and writes survivors compactly into an output buffer. `parallelScan` computes the exclusive prefix of "kept" flags so each thread knows where to write.

```cpp
std::size_t filterEvents(citor::ThreadPool &pool,
                         std::span<const Event> in,
                         std::span<Event> out, EventFilter pred) {
  std::vector<std::int64_t> mark(in.size());
  pool.parallelFor<citor::HintsDefaults>(
      0, in.size(),
      [&](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) mark[i] = pred(in[i]) ? 1 : 0;
      });

  std::vector<std::int64_t> off(in.size());
  if (pool.participants() == 1) {
    std::exclusive_scan(mark.begin(), mark.end(), off.begin(),
                        std::int64_t{0});
  } else {
    // Canonical two-pass idiom: atomic call counter distinguishes pass 1
    // (chunk total) from pass 2 (per-element exclusive prefix). The body's
    // trailing pointer parameter is reserved (always nullptr); the body
    // owns its output buffer through capture.
    std::atomic<std::size_t> calls{0};
    const std::size_t pn = pool.participants();
    (void)pool.parallelScan<citor::HintsDefaults>(
        in.size(), std::int64_t{0},
        [&](std::size_t /*chunkId*/, std::size_t lo, std::size_t hi,
            std::int64_t initial,
            std::int64_t * /*reserved*/) -> std::int64_t {
          const std::size_t call =
              calls.fetch_add(1, std::memory_order_acq_rel);
          if (call < pn) {
            std::int64_t s = 0;
            for (std::size_t i = lo; i < hi; ++i) s += mark[i];
            return s;
          }
          std::int64_t running = initial;
          for (std::size_t i = lo; i < hi; ++i) {
            off[i] = running;
            running += mark[i];
          }
          return running - initial;
        },
        [](std::int64_t a, std::int64_t b) { return a + b; });
  }

  std::int64_t kept = 0;
  pool.parallelFor<citor::HintsDefaults>(
      0, in.size(),
      [&](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i)
          if (mark[i]) out[static_cast<std::size_t>(off[i])] = in[i];
      });

  if (!mark.empty()) kept = off.back() + mark.back();
  return static_cast<std::size_t>(kept);
}
```

**Why this primitive.** Two-pass scan: pass 1 produces per-chunk totals, the producer prefixes them in `O(participants)`, pass 2 writes per-element offsets with the chunk's exclusive prefix as `initial`. Same shape covers stream compaction, CSV row-offset computation, and output-slot allocation in batched parsers.

### ML inference preprocessing pipeline (`parallelChain`)

An ML inference frontend takes a batch of decoded RGB images and runs three stages: bilinear resize, per-channel normalize to `[-1, 1]`, write into a packed float tensor.

```cpp
void preprocessBatch(citor::ThreadPool &pool,
                     std::span<const RgbImage> in,
                     std::vector<RgbImage> &resized,
                     float *outTensor,
                     std::size_t outH, std::size_t outW) {
  resized.resize(in.size());

  pool.parallelChain<citor::ChainHintsDefaults>(
      in.size(),
      citor::globalStage("resize",
          [&](std::size_t /*stage*/, std::uint32_t /*slot*/,
              std::size_t lo, std::size_t hi) {
            for (std::size_t i = lo; i < hi; ++i)
              resized[i] = bilinearResize(in[i], outH, outW);
          }),
      citor::globalStage("normalize",
          [&](std::size_t /*stage*/, std::uint32_t /*slot*/,
              std::size_t lo, std::size_t hi) {
            for (std::size_t i = lo; i < hi; ++i) normalizeInPlace(resized[i]);
          }),
      citor::staticStage("emit",
          [&](std::size_t /*stage*/, std::uint32_t /*slot*/,
              std::size_t lo, std::size_t hi) {
            for (std::size_t i = lo; i < hi; ++i)
              writeToTensor(resized[i], outTensor, i, outH, outW);
          }));
}
```

**Why this primitive.** Three sequential `parallelFor` calls pay three separate dispatch round-trips. `parallelChain` publishes one descriptor and uses a per-slot done-epoch scan for the inter-stage rendezvous. With `pipelineSameChunk = true` (the default) each worker keeps the same `[lo, hi)` across all stages, so the L1/L2 stays warm for the image range it owns.

### Cloth simulation tick loop (`runPlex`)

A 2D mass-spring cloth simulation runs N substeps per render frame. Phase parity selects source and destination so workers never write to the buffer they're reading; positions and velocities live in two arrays that swap roles each step.

```cpp
void simulateCloth(citor::ThreadPool &pool,
                   std::vector<Particle> &a, std::vector<Particle> &b,
                   std::size_t substeps, float dt) {
  pool.runPlex<citor::HintsDefaults>(
      substeps, a.size(),
      [&](std::size_t step, std::uint32_t /*slot*/,
          std::size_t lo, std::size_t hi) {
        const auto &src = (step & 1U) ? b : a;
        auto &dst = (step & 1U) ? a : b;
        for (std::size_t i = lo; i < hi; ++i)
          dst[i] = integrateVerlet(src, i, dt);
      });
}
```

**Why this primitive.** A `parallelFor` loop with `substeps` iterations would wake and park workers `substeps` times. `runPlex` publishes one descriptor and keeps workers in user-space rendezvous between phases -- per-phase cost is a rendezvous spin, not a syscall. Same shape covers Jacobi solvers, Gauss-Seidel sweeps, Game of Life, fluid simulations, and any iterative kernel over a stable partition.

### Game broad-phase collision queries (`bulkForQueries`)

A physics engine has N moving bodies per frame and queries each against a BVH for potential collision pairs. Per-query cost varies wildly with traversal depth -- some bodies are in cluttered regions, others are alone in space.

```cpp
void broadPhase(citor::ThreadPool &pool, const Bvh &bvh,
                std::span<const Body> bodies,
                std::vector<HitList> &hits) {
  hits.resize(bodies.size());
  pool.bulkForQueries<citor::DynamicHints>(
      bodies.size(),
      [&](std::size_t lo, std::size_t hi) {
        for (std::size_t q = lo; q < hi; ++q)
          hits[q] = bvh.queryAabb(bodies[q].aabb);
      });
}
```

**Why this primitive.** `parallelFor` is for uniform per-item cost; `bulkForQueries` defaults to `Balance::DynamicChunked` for variable per-query cost. A worker that finishes its block fast keeps pulling more, so a single deep-tree query doesn't stall the slowest leaf. Result must be keyed by query index (`hits[q]`), not chunk order: chunk dispatch order varies across worker counts. Same shape works for ray-batch intersection, spatial-hash lookups, KD-tree nearest-neighbor, and per-row sparse matrix-vector products.

### BVH build by recursive partition (`forkJoin`)

A graphics engine builds a BVH from a triangle list by recursively partitioning along the longest-axis median. Each subtree is independent; partition imbalance is absorbed by Chase-Lev steal.

```cpp
struct BvhNode {
  Aabb bounds;
  std::unique_ptr<BvhNode> left, right;
  std::span<Triangle> leaf;  // empty for internal nodes
};

void buildBvh(citor::ThreadPool &pool,
              std::span<Triangle> tris, BvhNode &out) {
  if (tris.size() <= 8) {
    out.leaf = tris;
    out.bounds = boundsOf(tris);
    return;
  }
  const std::size_t mid = partitionAlongLongestAxis(tris);
  out.left  = std::make_unique<BvhNode>();
  out.right = std::make_unique<BvhNode>();
  pool.forkJoin<citor::CcdLocalForkJoinHints>(
      [&] { buildBvh(pool, tris.subspan(0, mid), *out.left);  },
      [&] { buildBvh(pool, tris.subspan(mid),    *out.right); });
  out.bounds = unionAabb(out.left->bounds, out.right->bounds);
}
```

**Why this primitive.** Each worker has its own Chase-Lev deque. Each `forkJoin` level pushes children onto the calling worker's deque, runs one inline, and lets peers steal the rest. There is no central submission queue, so the steal protocol scales with participant count. `CcdLocalForkJoinHints` biases steal probes to same-CCD victims so transferred work stays L3-local. Same shape covers KD-tree builds, octree splits, parallel sorts (cilksort, mergesort), Strassen multiplication, and branch-and-bound search (pair with a `CancellationToken` to prune).

### Post-frame metrics flush (`submitDetached`)

A game or telemetry-heavy server wants to flush per-frame timing metrics to a sink (UDP, file, Prometheus exporter) without blocking the main loop. The pool destructor still waits for every detached body to retire, so metrics from the final frame land before shutdown.

```cpp
struct FrameMetrics { std::uint64_t cpuNs, gpuNs, framesQueued; };

void flushMetricsAsync(citor::ThreadPool &pool, FrameMetrics m) {
  pool.submitDetached<citor::HintsDefaults>(
      [m] { writeMetricsToSink(m); });
}

// Periodically (or before pool teardown), observe any captured throw.
// The destructor blocks on the in-flight counter but does NOT rethrow.
if (auto eptr = pool.lastDetachedException()) {
  std::rethrow_exception(eptr);   // first throw only; later ones drop
}
```

**Why this primitive.** `submitDetached` is the only primitive that does not block the caller. The pool's destructor blocks on the detached counter, so the body can outlive the calling scope but cannot outlive the pool. Exceptions are captured and surfaced via `lastDetachedException()` instead of calling `std::terminate` -- the user picks when to observe them.

### Per-CCD column-store aggregation (`PoolGroup`)

A column-store query engine has a 100M-row dataset partitioned to roughly fit per-CCD L3. The aggregation runs on each CCD against its local partition; the producer combines partial results.

```cpp
double aggregateByShard(const ColumnStore &store) {
  auto &group = citor::PoolGroup::global();
  const std::size_t shards = group.ccdCount();
  std::vector<double> partials(shards, 0.0);

  std::vector<std::thread> drivers;
  for (std::size_t ccd = 0; ccd < shards; ++ccd) {
    drivers.emplace_back([&, ccd] {
      auto &arena = group.arena(ccd);
      partials[ccd] = arena.parallelReduce<citor::KahanReduceHints>(
          0, store.shardSize(ccd), 0.0,
          [&](std::size_t lo, std::size_t hi) {
            double s = 0.0;
            for (std::size_t i = lo; i < hi; ++i)
              s += store.value(ccd, i);
            return s;
          },
          [](double a, double b) { return a + b; });
    });
  }
  for (auto &t : drivers) t.join();

  double total = 0.0;
  for (double p : partials) total += p;
  return total;
}
```

**Why this primitive.** Workers in arena `i` are pinned to CCD `i`'s cores at construction; `arena(i).parallelReduce(...)` keeps memory traffic on that CCD's L3 instead of crossing the inter-CCD fabric. Cross-arena synchronous calls (worker on arena A submitting to arena B) fall through to inline-on-caller via the TLS participant token, so cross-arena work never enqueues onto a queue the caller doesn't service. Same shape applies to per-NUMA-node partition processing, per-CCD ML inference batch routing, and any large array transformation where partition locality matters.

### Deadline-bounded chess search (cancellation + `Deadline`)

A chess engine runs iterative deepening with an external time budget. The search must abort cooperatively when the budget expires; the partial result from the deepest fully-completed iteration is returned.

```cpp
Move searchWithDeadline(citor::ThreadPool &pool, const Board &b,
                        std::chrono::milliseconds budget) {
  auto tok = citor::CancellationToken::makeOwned();
  Move best;

  // Watchdog runs on its own std::thread, not a pool worker, so it does not
  // burn one of the participants for the duration of the call.
  std::thread watchdog([tok, budget]() mutable {
    std::this_thread::sleep_for(budget);
    tok.request_stop();
  });

  for (int depth = 1; depth <= 20 && !tok.stop_requested(); ++depth) {
    std::vector<MoveScore> scored(b.legalMoveCount());
    pool.parallelFor<citor::HintsDefaults>(
        0, scored.size(),
        [&](std::size_t lo, std::size_t hi) {
          for (std::size_t i = lo; i < hi; ++i)
            scored[i] = alphaBeta(b, i, depth, tok);
        },
        tok);
    if (!tok.stop_requested()) best = pickBest(scored);
  }

  watchdog.join();
  return best;
}
```

**Why this combination.** Pass the same token to every primitive in the call tree; each polls at chunk boundaries and stops admitting new work. `Deadline` is also available (`citor::Deadline::fromMillis(50).expired()` is a few cycles via cached TSC), but `Deadline` does not stop a primitive by itself -- wire it through a watchdog that calls `tok.request_stop()` once `expired()`. For tokens statically known never to fire, `cancellationChecks = false` compiles out the per-chunk poll. Void primitives (like `parallelFor` here) early-return on stop -- they do **not** throw; only `parallelReduce` throws (`cancelled_value_exception<T>` carrying the deterministic partial).

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
| `balance`            | `Balance`       | `DynamicChunked`   | `StaticUniform` (worker-strided block partition, deterministic block->rank) vs `DynamicChunked` (atomic counter, straggler-tolerant). Reduce primitives override this internally to `StaticUniform`. |
| `determinism`        | `Determinism`   | `FixedBlockOrder`  | `parallelReduce` only. `FixedBlockOrder` = chunk-id pairwise tree. `KahanCompensated` = Kahan/Neumaier on top. |
| `affinity`           | `Affinity`      | `CcdLocal`         | `forkJoin` steal-victim direction. `CcdLocal` biases same-CCD victims first. |
| `priority`           | `Priority`      | `Throughput`       | Two-bucket gate when concurrent producers contend. `Latency` jumps the gate; `Background` yields. Single-producer pools see no priority effect. |
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
| `DynamicChainHints`       | chain shape: `balance = DynamicChunked`, `pipelineSameChunk = false` | stage packs with skewed bodies and only `Global` / `DeterministicReduce` barriers. |

### Runtime hint siblings

Every primitive has a `*Runtime` sibling that takes a `Hints` (or `ChainHints`) POD by value. Use these when policy is decided at runtime (CLI flags, benchmark drivers):

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

Two caveats:

- The runtime `Hints` POD defaults to `Balance::StaticUniform`, while the compile-time `HintsDefaults` defaults to `DynamicChunked`. Set the field explicitly on the POD when you want parity with a specific compile-time preset.
- For `parallelChainRuntime`, only `hints.priority` is consumed by the engine today; `balance`, `chunk`, `pipelineSameChunk`, and `cancellationChecks` are accepted but currently fall back to engine defaults. Use `parallelChain<ChainHintsT>(...)` when those fields must be honored.

## Cancellation and deadlines

`citor::CancellationToken` is a copy-cheap handle wrapping a heap-allocated atomic. The default-constructed sentinel is allocation-free and `stop_requested()` always returns `false`; obtain a token whose flag can actually be set via `CancellationToken::makeOwned()`. `tok.canStop()` distinguishes a real token from the sentinel.

```cpp
citor::CancellationToken tok = citor::CancellationToken::makeOwned();

std::thread killer([tok]() mutable {
  using namespace std::chrono_literals;
  std::this_thread::sleep_for(50ms);
  tok.request_stop();
});

pool.parallelFor<citor::HintsDefaults>(
    0, 1'000'000'000,
    [&](std::size_t lo, std::size_t hi) {
      for (std::size_t i = lo; i < hi; ++i) { /* heavy work */ }
    },
    tok);
// On a stop, parallelFor early-returns; it does NOT throw.

killer.join();
```

Cancellation behavior by primitive:

- **`parallelFor`, `parallelChain`, `parallelChainWithToken`, `runPlex`, `bulkForQueries`, `submitDetached`**: void return; on a stopped token, the primitive early-returns without invoking further bodies. **No exception is thrown.**
- **`forkJoin`**: void return; cancellation clamps the outstanding-task count and returns normally. Tasks that haven't started simply don't run.
- **`parallelReduce`**: throws `cancelled_value_exception<T>` carrying `partial_value`, the deterministic combine of the chunks that completed before the stop was observed.
- **`parallelScan`**: returns `identity` on a pre-stopped token; mid-flight stops still complete the in-flight passes.

The `cancellationChecks` hint compiles out the per-chunk poll for tokens that are statically known not to stop. Pair it with the never-stopped sentinel for hot loops where cancellation is not a possibility.

`citor::Deadline` is a TSC-based absolute threshold for cooperative deadline checks:

```cpp
citor::Deadline d0;                                  // never expires (UINT64_MAX threshold)
citor::Deadline d1 = citor::Deadline::fromMillis(5);
citor::Deadline d2 = citor::Deadline::fromMicros(250);
bool expired = d1.expired();
```

On x86_64, `Deadline` calibrates cycles per nanosecond once per process and uses `__rdtsc()` / `rdtscp` checks; `expired()` is a few cycles. On non-x86 builds, deadline factories collapse to the never-expires sentinel. **`Deadline` does not stop a primitive by itself.** The intended pattern is to set the deadline, hand the `CancellationToken` to the primitive, and let a watchdog (or the body itself) call `tok.request_stop()` once `deadline.expired()`. A `Deadline` that has already elapsed before construction reports expired immediately.

## PoolGroup and per-CCD arenas

`PoolGroup::global()` lazily constructs one `ThreadPool` arena per CCD detected by sysfs. Each arena's `pool.kind()` is `PoolKind::Arena`. Cross-arena synchronous calls fall through to the inline path on the caller -- the TLS participant token guards against a worker on arena A submitting work to arena B and blocking on a queue arena A doesn't service.

```cpp
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

`PoolGroup::global()` is lazy, thread-safe by C++ function-local-static initialization, and intentionally never destroyed. Detached tasks submitted to a `PoolGroup::global()` arena will outlive most user objects -- if you need bounded worker lifetime, construct a stack `citor::PoolGroup group;` instead and let it go out of scope normally.

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

The bench harness measures dispatch latency and per-primitive throughput against ten peer pools (BS, dp, task, riften, oneTBB, Taskflow, Eigen, OpenMP, Leopard, dispenso); two coroutine schedulers (libfork, TooManyCooks) join the recursive fork-join workloads. Numbers are not embedded in this README -- they age out on the next compiler / microarchitecture bump.

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

The harness probes host invariants (governor, turbo, SMT, ASLR, libomp blocktime) at startup and flags any failed gate in the table output and the JSON `context` block. For serious latency numbers, set the cpufreq governor to `performance`, disable boost, disable ASLR (`/proc/sys/kernel/randomize_va_space=0`), and pin process affinity. The benchmark tree includes workloads for parallel-for dispatch, granularity, matmul, Pareto-distributed bodies, reductions, scans, chains, runPlex heat / stencil sweeps, fork-join recursive workloads, bulk queries, differential comparisons, and two-pool OpenMP coexistence.

## Supported targets

- **Validated and CI-backed**: Ubuntu 24.04, GCC 14, Clang 18, C++20, Linux x86_64.
- **Packaging metadata**: vcpkg overlay marks `linux & x64`; the Conan recipe warns when `os != Linux` or `arch != x86_64`.
- **Kernel**: Linux 6.x with `futex` and sysfs `cpu/cpuX/cache/index*` is the validated base.
- **AVX2**: auto-detected via `check_cxx_compiler_flag`; the pool builds without it but loses the AVX2-tuned code paths.

Implementation notes for non-validated paths:

- Linux uses raw futex parking and pthread affinity. Non-Linux compile fallbacks (`std::thread` + condition-variable parking) exist but are not the validated latency target.
- x86_64 uses TSC-based deadline checks and `_mm_pause` for spin hints. Non-x86 collapses `Deadline` factories to the never-expires sentinel and uses a compiler-barrier spin hint.
- AVX2/FMA flags are added by CMake when `CITOR_USE_AVX2=ON` and the compiler accepts them; the flag is an optimization toggle, not part of the support contract.

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

## License

MIT. See [`LICENSE`](LICENSE).
