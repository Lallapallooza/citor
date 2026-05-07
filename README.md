# citor

> Header-only C++20 thread pool tuned for sub-microsecond dispatch on Linux x86_64 + AVX2. Eight cooperating primitives, decentralized per-slot done-epoch barriers, Chase-Lev work-stealing, per-CCD arenas. MIT.

[![ci](https://github.com/Lallapallooza/citor/actions/workflows/ci.yml/badge.svg)](https://github.com/Lallapallooza/citor/actions/workflows/ci.yml)
[![nightly](https://github.com/Lallapallooza/citor/actions/workflows/nightly.yml/badge.svg)](https://github.com/Lallapallooza/citor/actions/workflows/nightly.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)

The name comes from Latin *cito* -- "swiftly, quickly".

---

## Table of contents

- [Contract](#contract)
- [Install](#install)
- [Hello world](#hello-world)
- [Performance](#performance)
- [Primitives](#primitives)
  - [`parallelFor`](#parallelfor)
  - [`parallelReduce`](#parallelreduce)
  - [`parallelScan`](#parallelscan)
  - [`parallelChain`](#parallelchain)
  - [`runPlex`](#runplex)
  - [`bulkForQueries`](#bulkforqueries)
  - [`forkJoin`](#forkjoin)
  - [`submitDetached`](#submitdetached)
- [Hints reference](#hints-reference)
- [PoolGroup and per-CCD arenas](#poolgroup-and-per-ccd-arenas)
- [Cancellation](#cancellation)
- [Build options](#build-options)
- [Supported compilers](#supported-compilers)
- [Non-goals](#non-goals)
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

## Performance

Per-family geomean speedups vs eight competitor pools (BS, dp, task, riften, oneTBB, Taskflow, Eigen, OpenMP) on the citor bench harness. Charts are regenerated by `scripts/plot.py`; the underlying numbers live in `bench_out/<host>/<sha>/results.json`.

| family             | chart                                                        |
|--------------------|--------------------------------------------------------------|
| `parallelFor`      | [parallelFor_geomean.svg](docs/bench/parallelFor_geomean.svg) |
| `parallelReduce`   | [parallelReduce_geomean.svg](docs/bench/parallelReduce_geomean.svg) |
| `forkJoin`         | [forkJoin_geomean.svg](docs/bench/forkJoin_geomean.svg)       |
| `parallelChain`    | [parallelChain_geomean.svg](docs/bench/parallelChain_geomean.svg) |
| `runPlex`          | [runPlex_geomean.svg](docs/bench/runPlex_geomean.svg)         |

Per-workload scatters and the "where citor loses" table live in [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md). The README does not embed numbers -- they age out on the next compiler / microarchitecture bump and the commit message that introduces the change is the right home for them.

Reproduce on your hardware:

```bash
cmake -S . -B build -G Ninja -DCITOR_BUILD_BENCHMARK=ON
cmake --build build --target parallel_bench -j

scripts/quickbench.sh                                # ~90s smoke check
scripts/run_bench.sh --out bench_out/$(hostname -s)  # full sweep, ~2-3h
scripts/plot.py bench_out/.../results.json --out docs/bench
```

The bench scripts read host invariants (governor, turbo, SMT, ASLR, taskset mask) into a `host.json` next to the results so two runs from different machines stay comparable. See [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md) for the full methodology and host-disclosure block.

---

