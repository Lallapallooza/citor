cmake_minimum_required(VERSION 3.20)
project(
    citor
    VERSION 0.1.0
    DESCRIPTION "Header-only C++20 thread pool for sub-microsecond dispatch"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CITOR_IS_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(CITOR_IS_TOP_LEVEL ON)
endif()

option(CITOR_BUILD_TESTS "Build the tests" ${CITOR_IS_TOP_LEVEL})
option(CITOR_BUILD_BENCHMARK "Build the bench harness" ${CITOR_IS_TOP_LEVEL})
option(
    CITOR_BUILD_WITH_SANITIZER
    "Build with ThreadSanitizer (-fsanitize=thread)"
    OFF
)
option(CITOR_USE_AVX2 "Enable AVX2 / FMA on the public include set" ON)

# Pool diagnostic counters (dispatches, inlineFallbacks, cancellationStops). When OFF, the
# hot-path increment sites compile to no-ops and `ThreadPool::snapshotCounters()` returns zero
# for the pool-level fields (worker-aggregated fields still reflect `WorkerState` counters,
# which are pre-existing). Default OFF so the dispatch hot path pays no extra atomics.
option(CITOR_ENABLE_POOL_COUNTERS "Enable pool-level diagnostic counters (default OFF)" OFF)

# Per-worker pthread stack size. Default 2048 KiB accommodates recursive forkJoin bodies under
# TSan + ASan shadow stacks (UTS-style benchmarks overflow at 256 KiB under sanitizer builds)
# AND the cilksort_j16_n16m bench (overflows at 1024 KiB on Release: log2(16M / 8K) = 11 levels
# of recursion combined with the per-frame 32-bucket merge scratch and per-iteration tmp/data
# pointers exceeds the smaller stack on j=16). Production builds with no sanitizer and shallow
# recursion can drop this to 256 (saves ~28 MiB committed per j=16 pool); verify with the
# heaviest forkJoin workload on hand before doing so.
set(CITOR_WORKER_STACK_KIB "2048" CACHE STRING "Per-worker pthread stack size, KiB")

include(${PROJECT_SOURCE_DIR}/cmake/CPM.cmake)
include(${PROJECT_SOURCE_DIR}/cmake/Tooling.cmake)

# Public header-only target. Consumers either CPMAddPackage us or add this as a
# subdirectory; in both cases they link against `citor::citor` and pick up the
# include path plus the AVX2 compile defines (when enabled).
add_library(citor INTERFACE)
add_library(citor::citor ALIAS citor)
target_include_directories(
    citor
    INTERFACE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_compile_features(citor INTERFACE cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(citor INTERFACE Threads::Threads)

if(CITOR_USE_AVX2)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-mavx2" CITOR_HAS_MAVX2)
    if(CITOR_HAS_MAVX2)
        target_compile_options(citor INTERFACE -mavx2 -mfma)
        target_compile_definitions(citor INTERFACE CITOR_USE_AVX2)
    endif()
endif()

target_compile_definitions(
    citor
    INTERFACE
        CITOR_WORKER_STACK_KIB=${CITOR_WORKER_STACK_KIB}
)

if(CITOR_ENABLE_POOL_COUNTERS)
    target_compile_definitions(citor INTERFACE CITOR_ENABLE_POOL_COUNTERS)
endif()

# Strict warnings only when we are the top-level project. Consumers should not
# inherit our warning levels through the INTERFACE target.
if(CITOR_IS_TOP_LEVEL)
    add_library(citor_warnings INTERFACE)
    target_compile_options(
        citor_warnings
        INTERFACE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wnon-virtual-dtor
            -Wold-style-cast
            -Wcast-align
            -Woverloaded-virtual
            -Wconversion
            -Wsign-conversion
            -Wnull-dereference
            -Wdouble-promotion
            -Wformat=2
    )
endif()

if(CITOR_BUILD_WITH_SANITIZER)
    target_compile_options(
        citor
        INTERFACE -fsanitize=thread -fno-omit-frame-pointer
    )
    target_link_options(citor INTERFACE -fsanitize=thread)
endif()

# Order matters: bench fetches BS::thread_pool which the differential tests
# depend on. Bench subdir runs first so the target is in scope when tests/
# evaluates `if(TARGET bshoshany_thread_pool)`.
if(CITOR_BUILD_BENCHMARK AND CITOR_IS_TOP_LEVEL)
    add_subdirectory(benchmark)
endif()

if(CITOR_BUILD_TESTS AND CITOR_IS_TOP_LEVEL)
    enable_testing()
    add_subdirectory(tests)
endif()

include(GNUInstallDirs)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS citor EXPORT citor-targets)
install(
    EXPORT citor-targets
    FILE citor-config.cmake
    NAMESPACE citor::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/citor
)
