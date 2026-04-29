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
    GIT_TAG bd4533f1f70c2b975cbd5769a60d8eaaea1d2233
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
    GIT_TAG 2417b702026b56303486dc1442fed9658990185f
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
    GIT_TAG 44f1d6ee2ba8bed4873cb5b24b3cd1d8bf825b48
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
    GIT_TAG 3a0746bf5f601dfed05330aefcb6854354fce07d
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
    GIT_TAG 41884b536a53166775fd20918aba4fa066983815
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
    GIT_TAG ea1a1f4d5804643b48dcd352196bbe45016c5022
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
    GIT_TAG a5a50c2b33eea2ff1fcb355cacdface43eb42b25
)

cpmaddpackage(
    NAME oneTBB
    GITHUB_REPOSITORY oneapi-src/oneTBB
    GIT_TAG 0c0ff192a2304e114bc9e6557582dfba101360ff
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
    GIT_TAG 7d9e85b6b2e9bf501021f857f2f3cbe43bc37c85
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
    GIT_TAG 3147391d946bb4b6c68edd901f2add6ac1f31f8c
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

# HdrHistogram_c is a mandatory bench dependency. The cold-dispatch
# tail-latency TU requires it; without it, parallel_bench cannot configure.
cpmaddpackage(
    NAME HdrHistogram_c
    GITHUB_REPOSITORY HdrHistogram/HdrHistogram_c
    GIT_TAG be60a9987ee48d0abf0d7b6a175bad8d6c1585d1
    OPTIONS
        "HDR_LOG_REQUIRED OFF"
        "HDR_HISTOGRAM_BUILD_SHARED OFF"
        "HDR_HISTOGRAM_BUILD_STATIC ON"
        "HDR_HISTOGRAM_INSTALL_SHARED OFF"
        "HDR_HISTOGRAM_INSTALL_STATIC OFF"
        "HDR_HISTOGRAM_BUILD_PROGRAMS OFF"
)

# Leopard (hosseinmoein) -- header-only C++20 work-stealing pool with
# parallel_loop + dispatch. Fills a gap between BS-style submit pools and
# full work-stealing runtimes.
cpmaddpackage(
    NAME Leopard
    GITHUB_REPOSITORY hosseinmoein/Leopard
    GIT_TAG main
    DOWNLOAD_ONLY YES
)
if(Leopard_ADDED AND NOT TARGET leopard_threadpool)
    # Leopard's `ThreadPool.tcc` defines several non-template member functions
    # (e.g. `ThreadPool::ThreadPool`, `Conditioner::execute`) without the
    # `inline` keyword. Including the header in multiple TUs produces ODR
    # link errors. Patch the file post-fetch to mark those definitions
    # `inline` so the header is genuinely header-only across TUs.
    set(_leopard_tcc ${Leopard_SOURCE_DIR}/include/Leopard/ThreadPool.tcc)
    if(EXISTS ${_leopard_tcc})
        file(READ ${_leopard_tcc} _leopard_tcc_contents)
        # Idempotent guard: only patch when our marker is absent.
        string(
            FIND
            "${_leopard_tcc_contents}"
            "// citor-patched-inline"
            _leopard_marker_pos
        )
        if(_leopard_marker_pos EQUAL -1)
            string(
                REGEX
                REPLACE
                "(\nvoid Conditioner::|\nThreadPool::|\nvoid ThreadPool::|\nbool ThreadPool::|\n[A-Za-z_:]+::size_type ThreadPool::)"
                "\ninline\\1"
                _leopard_tcc_contents
                "${_leopard_tcc_contents}"
            )
            file(
                WRITE ${_leopard_tcc}
                "// citor-patched-inline\n${_leopard_tcc_contents}"
            )
        endif()
    endif()
    add_library(leopard_threadpool INTERFACE)
    target_include_directories(
        leopard_threadpool
        SYSTEM
        INTERFACE ${Leopard_SOURCE_DIR}/include
    )
endif()

# Dispenso (Meta/Facebook) -- C++17 work-stealing pool with parallel_for,
# futures, task graphs, concurrent containers. Closest design philosophy
# match to citor among the surveyed competitors.
cpmaddpackage(
    NAME dispenso
    GITHUB_REPOSITORY facebookincubator/dispenso
    GIT_TAG main
    OPTIONS
        "DISPENSO_SHARED_LIB OFF"
        "DISPENSO_BUILD_TESTS OFF"
        "DISPENSO_BUILD_BENCHMARKS OFF"
)

find_package(OpenMP QUIET)
if(OpenMP_CXX_FOUND)
    add_library(parallel_bench_openmp INTERFACE)
    target_link_libraries(parallel_bench_openmp INTERFACE OpenMP::OpenMP_CXX)
endif()

set(_citor_bench_sources
    affinity_sweep_bench.cpp
    balance_sweep_bench.cpp
    bench_main.cpp
    bulk_for_queries_bench.cpp
    chain_bench.cpp
    cold_path_bench.cpp
    cross_ccd_parallel_for_bench.cpp
    differential_bench.cpp
    empty_fanout_bench.cpp
    fork_join_bench.cpp
    forkjoin_cilksort_bench.cpp
    forkjoin_knapsack_cancel_bench.cpp
    forkjoin_strassen_bench.cpp
    forkjoin_uts_bench.cpp
    granularity_bench.cpp
    matmul_bench.cpp
    parallel_for_cold_dispatch_bench.cpp
    parallel_reduce_bench.cpp
    parallel_reduce_pareto_bench.cpp
    pareto_body_bench.cpp
    plex_bench.cpp
    runplex_heat_bench.cpp
    runplex_stencil_bench.cpp
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
        hdr_histogram_static
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
    get_target_property(
        _taskflow_includes
        Taskflow
        INTERFACE_INCLUDE_DIRECTORIES
    )
    if(_taskflow_includes)
        target_include_directories(
            parallel_bench
            SYSTEM
            PRIVATE ${_taskflow_includes}
        )
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
if(TARGET leopard_threadpool)
    target_link_libraries(parallel_bench PRIVATE leopard_threadpool)
    target_compile_definitions(parallel_bench PRIVATE CITOR_BENCH_HAS_LEOPARD)
endif()
if(TARGET dispenso)
    # Dispenso ships its own warning surface (sign conversions, unused
    # parameters in its template-heavy headers) that trips the bench's
    # `-Wsign-conversion` / `-Wextra` policy. Re-export its include
    # directories as SYSTEM to silence the third-party noise while keeping
    # our own code under the strict policy.
    get_target_property(
        _dispenso_includes
        dispenso
        INTERFACE_INCLUDE_DIRECTORIES
    )
    if(_dispenso_includes)
        target_include_directories(
            parallel_bench
            SYSTEM
            PRIVATE ${_dispenso_includes}
        )
    endif()
    target_link_libraries(parallel_bench PRIVATE dispenso)
    target_compile_definitions(parallel_bench PRIVATE CITOR_BENCH_HAS_DISPENSO)
endif()

if(UNIX AND NOT APPLE)
    target_compile_definitions(
        parallel_bench
        PRIVATE CITOR_BENCH_HAS_PERF_EVENT
    )
endif()

citor_enable_tidy(parallel_bench)

# Two-pool BLAS-coexistence bench. Lives in a SEPARATE executable target
# because libomp's `KMP_BLOCKTIME` and `omp_set_num_threads(...)` are
# process-wide globals; sharing a process with the standard `parallel_bench`
# cells would let the two-pool cells' blocktime overrides leak into other
# workloads' timing windows. The target only builds when OpenMP is available
# (the workload itself depends on `omp.h` plus the libomp `kmp_set_blocktime`
# extension).
if(TARGET parallel_bench_openmp)
    set(_citor_bench_two_pool_sources
        two_pool/bench_main_two_pool.cpp
        two_pool/blas_coexistence_bench.cpp
    )
    add_executable(parallel_bench_two_pool ${_citor_bench_two_pool_sources})
    target_include_directories(
        parallel_bench_two_pool
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(
        parallel_bench_two_pool
        PRIVATE citor::citor nanobench parallel_bench_openmp
    )
    target_compile_definitions(
        parallel_bench_two_pool
        PRIVATE CITOR_BENCH_HAS_OPENMP
    )
    if(CITOR_IS_TOP_LEVEL)
        target_link_libraries(parallel_bench_two_pool PRIVATE citor_warnings)
    endif()
    citor_enable_tidy(parallel_bench_two_pool)
endif()
