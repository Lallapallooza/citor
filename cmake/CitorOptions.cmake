# Project-wide options. Sourced by the top-level CMakeLists.txt.

set(CITOR_IS_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(CITOR_IS_TOP_LEVEL ON)
endif()

option(CITOR_BUILD_TESTS "Build the GTest suite" ${CITOR_IS_TOP_LEVEL})
option(
    CITOR_BUILD_BENCHMARK
    "Build the comparative bench"
    ${CITOR_IS_TOP_LEVEL}
)
option(CITOR_BUILD_WITH_SANITIZER "Compile and link with -fsanitize=thread" OFF)
option(CITOR_USE_AVX2 "Add -mavx2 -mfma to the public INTERFACE target" ON)
option(
    CITOR_ENABLE_CLANG_TIDY
    "Run clang-tidy on every TU during the build (CMAKE_CXX_CLANG_TIDY)"
    OFF
)

# Pool diagnostic counters (dispatches, inlineFallbacks, cancellationStops).
# OFF leaves the hot-path increment sites as no-ops.
option(
    CITOR_ENABLE_POOL_COUNTERS
    "Compile in pool-level diagnostic counters"
    OFF
)

# Per-worker pthread stack size. The default matches glibc's default thread
# stack so deeply recursive forkJoin bodies have headroom; production builds
# with shallow recursion may drop this to save committed pages.
set(CITOR_WORKER_STACK_KIB
    "8192"
    CACHE STRING
    "Per-worker pthread stack size, KiB"
)
