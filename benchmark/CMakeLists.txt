# Comparative bench harness.
#
# Adds the `parallel_bench` executable that runs `citor::ThreadPool` head-to-
# head against a curated set of competitor pools (BS::thread_pool,
# dp::thread_pool, task_thread_pool, riften::Thiefpool, oneTBB, Taskflow,
# Eigen NonBlockingThreadPool, OpenMP) on the same workload with the same
# seeds. Measurement uses a `__rdtscp`-based cycle stamp; on Linux the harness
# can also cross-check against `perf_event_open(PERF_COUNT_HW_CPU_CYCLES)`.
include(${PROJECT_SOURCE_DIR}/cmake/CPM.cmake)

cpmaddpackage(
    NAME bshoshany_thread_pool
    GITHUB_REPOSITORY bshoshany/thread-pool
    GIT_TAG v5.1.0
    DOWNLOAD_ONLY YES
)
if(bshoshany_thread_pool_ADDED AND NOT TARGET bshoshany_thread_pool)
    add_library(bshoshany_thread_pool INTERFACE)
    target_include_directories(
        bshoshany_thread_pool
        SYSTEM
        INTERFACE ${bshoshany_thread_pool_SOURCE_DIR}/include
    )
endif()

cpmaddpackage(
    NAME dp_thread_pool
    GITHUB_REPOSITORY DeveloperPaul123/thread-pool
    GIT_TAG 0.7.0
    DOWNLOAD_ONLY YES
)
if(dp_thread_pool_ADDED AND NOT TARGET dp_thread_pool)
    add_library(dp_thread_pool INTERFACE)
    target_include_directories(
        dp_thread_pool
        SYSTEM
        INTERFACE ${dp_thread_pool_SOURCE_DIR}/include
    )
endif()

cpmaddpackage(
    NAME task_thread_pool
    GITHUB_REPOSITORY alugowski/task-thread-pool
    GIT_TAG v1.0.10
    DOWNLOAD_ONLY YES
)
if(task_thread_pool_ADDED AND NOT TARGET task_thread_pool)
    add_library(task_thread_pool INTERFACE)
    target_include_directories(
        task_thread_pool
        SYSTEM
        INTERFACE ${task_thread_pool_SOURCE_DIR}/include
    )
endif()

cpmaddpackage(
    NAME function2
    GITHUB_REPOSITORY Naios/function2
    GIT_TAG 4.1.0
    DOWNLOAD_ONLY YES
)
if(function2_ADDED AND NOT TARGET function2)
    add_library(function2 INTERFACE)
    target_include_directories(
        function2
        SYSTEM
        INTERFACE ${function2_SOURCE_DIR}/include
    )
endif()

cpmaddpackage(
    NAME riften_deque
    GITHUB_REPOSITORY ConorWilliams/ConcurrentDeque
    GIT_TAG v1.1.0
    DOWNLOAD_ONLY YES
)
if(riften_deque_ADDED AND NOT TARGET riften_deque)
    add_library(riften_deque INTERFACE)
    target_include_directories(
        riften_deque
        SYSTEM
        INTERFACE ${riften_deque_SOURCE_DIR}/include
    )
endif()

cpmaddpackage(
    NAME riften_threadpool
    GITHUB_REPOSITORY ConorWilliams/Threadpool
    GIT_TAG v2.1.1
    DOWNLOAD_ONLY YES
)
if(riften_threadpool_ADDED AND NOT TARGET riften_threadpool)
    add_library(riften_threadpool INTERFACE)
    target_include_directories(
        riften_threadpool
        SYSTEM
        INTERFACE ${riften_threadpool_SOURCE_DIR}/include
    )
    target_link_libraries(riften_threadpool INTERFACE function2 riften_deque)
endif()

cpmaddpackage(
    NAME nanobench
    GITHUB_REPOSITORY martinus/nanobench
    GIT_TAG v4.3.11
)

cpmaddpackage(
    NAME oneTBB
    GITHUB_REPOSITORY oneapi-src/oneTBB
    GIT_TAG v2022.0.0
    OPTIONS
        "TBB_TEST OFF"
        "TBB_EXAMPLES OFF"
        "TBB_STRICT OFF"
        "TBB_INSTALL OFF"
        "BUILD_SHARED_LIBS OFF"
)

cpmaddpackage(
    NAME Taskflow
    GITHUB_REPOSITORY taskflow/taskflow
    GIT_TAG v3.7.0
    OPTIONS
        "TF_BUILD_TESTS OFF"
        "TF_BUILD_EXAMPLES OFF"
        "TF_BUILD_BENCHMARKS OFF"
        "TF_BUILD_CUDA OFF"
        "TF_BUILD_SYCL OFF"
        "TF_BUILD_PROFILER OFF"
)

cpmaddpackage(
    NAME Eigen
    GITLAB_REPOSITORY libeigen/eigen
    GIT_TAG 3.4.0
    DOWNLOAD_ONLY YES
)
if(Eigen_ADDED AND NOT TARGET eigen_threadpool)
    add_library(eigen_threadpool INTERFACE)
    target_include_directories(
        eigen_threadpool
        SYSTEM
        INTERFACE ${Eigen_SOURCE_DIR}
    )
endif()

find_package(OpenMP QUIET)
if(OpenMP_CXX_FOUND)
    add_library(parallel_bench_openmp INTERFACE)
    target_link_libraries(parallel_bench_openmp INTERFACE OpenMP::OpenMP_CXX)
endif()

set(_citor_bench_sources
    bench_main.cpp
    bulk_for_queries_bench.cpp
    chain_bench.cpp
    cold_path_bench.cpp
    differential_bench.cpp
    empty_fanout_bench.cpp
    fork_join_bench.cpp
    granularity_bench.cpp
    matmul_bench.cpp
    parallel_reduce_bench.cpp
    plex_bench.cpp
    scan_bench.cpp
)

add_executable(parallel_bench ${_citor_bench_sources})
target_include_directories(parallel_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(
    parallel_bench
    PRIVATE
        citor::citor
        bshoshany_thread_pool
        dp_thread_pool
        task_thread_pool
        riften_threadpool
        nanobench
)
if(CITOR_IS_TOP_LEVEL)
    target_link_libraries(parallel_bench PRIVATE citor_warnings)
endif()

if(TARGET TBB::tbb)
    target_link_libraries(parallel_bench PRIVATE TBB::tbb)
    target_compile_definitions(parallel_bench PRIVATE CITOR_BENCH_HAS_TBB)
endif()
if(TARGET Taskflow)
    # Taskflow ships its own warnings (signedness conversions, unused parameters)
    # that trip the bench's `-Wsign-conversion`/`-Wextra` policy. Re-export its
    # include directories as SYSTEM so its internal noise does not block the
    # build, while keeping our own warning policy strict.
    get_target_property(_taskflow_includes Taskflow INTERFACE_INCLUDE_DIRECTORIES)
    if(_taskflow_includes)
        target_include_directories(parallel_bench SYSTEM PRIVATE ${_taskflow_includes})
    endif()
    target_link_libraries(parallel_bench PRIVATE Taskflow)
    target_compile_definitions(parallel_bench PRIVATE CITOR_BENCH_HAS_TASKFLOW)
endif()
if(TARGET eigen_threadpool)
    target_link_libraries(parallel_bench PRIVATE eigen_threadpool)
    target_compile_definitions(
        parallel_bench
        PRIVATE CITOR_BENCH_HAS_EIGEN_THREADPOOL
    )
endif()
if(TARGET parallel_bench_openmp)
    target_link_libraries(parallel_bench PRIVATE parallel_bench_openmp)
    target_compile_definitions(parallel_bench PRIVATE CITOR_BENCH_HAS_OPENMP)
endif()

if(UNIX AND NOT APPLE)
    target_compile_definitions(
        parallel_bench
        PRIVATE CITOR_BENCH_HAS_PERF_EVENT
    )
endif()

citor_enable_tidy(parallel_bench)
