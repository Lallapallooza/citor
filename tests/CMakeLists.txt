include(${PROJECT_SOURCE_DIR}/cmake/GTestDep.cmake)
include(GoogleTest)

# Third-party GoogleTest sources do not honour our `.clang-tidy` rules;
# clear the inherited `CXX_CLANG_TIDY` property so they compile cleanly
# when `CITOR_ENABLE_CLANG_TIDY=ON` is set on the top-level build.
if(CITOR_ENABLE_CLANG_TIDY)
    foreach(_gtest_target IN ITEMS gtest gtest_main gmock gmock_main)
        if(TARGET ${_gtest_target})
            set_target_properties(${_gtest_target} PROPERTIES CXX_CLANG_TIDY "")
        endif()
    endforeach()
endif()

set(_citor_tests
    bench_format_test
    bulk_for_queries_cancellation_test
    bulk_for_queries_cpo_test
    bulk_for_queries_exception_test
    bulk_for_queries_partition_test
    cancellation_deadline_test
    cancellation_exception_test
    cancellation_token_test
    chase_lev_deque_concurrent_test
    chase_lev_deque_growth_test
    chase_lev_deque_single_thread_test
    coherence_probe_test
    fork_join_affinity_test
    fork_join_cancellation_test
    fork_join_determinism_test
    fork_join_edge_test
    fork_join_errors_test
    fork_join_spawn_sync_test
    function_ref_test
    hints_test
    inclusive_scan_cpo_test
    parallel_chain_barriers_test
    parallel_chain_basic_test
    parallel_chain_cancellation_test
    parallel_chain_cpo_test
    parallel_chain_exceptions_test
    parallel_chain_inline_fallback_test
    parallel_chain_partition_test
    parallel_for_blocking_test
    parallel_for_cancellation_test
    parallel_for_cpo_test
    parallel_for_exceptions_test
    parallel_for_inline_fallback_test
    parallel_for_nesting_test
    parallel_for_partition_test
    parallel_for_reuse_test
    parallel_for_tsan_test
    parallel_pool_deadlock_fuzz_test
    parallel_pool_tsan_stress_dispatch_serialization_test
    parallel_pool_stress_primitives_test
    parallel_reduce_basic_test
    parallel_reduce_cancellation_test
    parallel_reduce_cpo_test
    parallel_reduce_cross_run_determinism_test
    parallel_reduce_determinism_main_test
    parallel_reduce_determinism_test
    parallel_reduce_exceptions_test
    parallel_scan_cancellation_test
    parallel_scan_correctness_test
    parallel_scan_edge_cases_test
    parallel_scan_exception_safety_test
    pool_group_arena_test
    pool_group_construction_test
    pool_group_reentrancy_test
    pool_group_topology_test
    priority_queue_test
    regression_auto_pin_test
    regression_back_to_back_dispatch_test
    regression_cross_pool_dispatch_test
    regression_descriptor_reuse_test
    regression_detached_lifecycle_test
    regression_fast_range_test
    regression_inline_fallback_test
    regression_partition_overflow_test
    regression_runtime_hints_test
    regression_tls_context_test
    run_plex_basic_test
    run_plex_cancellation_test
    run_plex_cpo_test
    run_plex_exceptions_test
    run_plex_inline_fallback_test
    run_plex_partition_test
    submit_detached_dispatch_test
    submit_detached_lifecycle_test
    thread_pool_auto_pin_test
    thread_pool_construction_test
    thread_pool_counters_test
    thread_pool_idle_test
    thread_pool_low_latency_scope_test
    thread_pool_tls_test
    thread_pool_tsan_stress_test
    worker_mailbox_protocol_test
)

set(_citor_test_extra_sources)
if(CITOR_BUILD_WITH_SANITIZER)
    list(APPEND _citor_test_extra_sources tsan_default_options.cpp)
endif()

# Linux-only tests: they probe sysfs / sched_getaffinity, drive
# pthread_getaffinity_np against the pool's auto-pin, or fork() through
# test_support_subprocess.h. Exclude on platforms without those interfaces
# so the Windows build can still compile + run the remaining suite.
set(_citor_tests_linux_only
    regression_auto_pin_test
    regression_cross_pool_dispatch_test
    regression_detached_lifecycle_test
    thread_pool_auto_pin_test
    thread_pool_construction_test
    thread_pool_idle_test
)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(REMOVE_ITEM _citor_tests ${_citor_tests_linux_only})
endif()

foreach(_t IN LISTS _citor_tests)
    add_executable(${_t} ${_t}.cpp ${_citor_test_extra_sources})
    target_link_libraries(${_t} PRIVATE citor::citor GTest::gtest_main)
    if(CITOR_IS_TOP_LEVEL)
        target_link_libraries(${_t} PRIVATE citor_warnings)
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(${_t} PRIVATE pthread)
    endif()
    gtest_discover_tests(${_t})
endforeach()

# `TSAN_OPTIONS` applied to every TSan-built test:
#   * `second_deadlock_stack=1` emits the second mutex-acquisition frame
#     on lock-order diagnostics.
#   * `print_full_thread_history=1` shows thread-creation stacks for every
#     thread named in a report.
#   * `atexit_sleep_ms=0` skips the runtime's unused post-exit pause.
#   * `external_symbolizer_path` points at the resolved `llvm-symbolizer-*`
#     binary so frames resolve to `file:line` under ctest's sanitized
#     environment.
#
# `FAIL_REGULAR_EXPRESSION` matches every TSan summary header (data race,
# lock-order inversion, thread leak) so a TSan report fails the test even
# when the process exits 0.
if(CITOR_BUILD_WITH_SANITIZER)
    find_program(
        LLVM_SYMBOLIZER_EXE
        NAMES
            llvm-symbolizer-21
            llvm-symbolizer-20
            llvm-symbolizer-19
            llvm-symbolizer-18
            llvm-symbolizer
    )
    set(_citor_tsan_options
        "halt_on_error=0:second_deadlock_stack=1:print_full_thread_history=1:atexit_sleep_ms=0"
    )
    if(LLVM_SYMBOLIZER_EXE)
        set(_citor_tsan_options
            "${_citor_tsan_options}:external_symbolizer_path=${LLVM_SYMBOLIZER_EXE}"
        )
    endif()
    set(_citor_tsan_fail_regex
        "WARNING: ThreadSanitizer;ThreadSanitizer: data race;ThreadSanitizer: lock-order-inversion;ThreadSanitizer: thread leak"
    )
    get_property(_citor_all_tests DIRECTORY PROPERTY TESTS)
    foreach(_test IN LISTS _citor_all_tests)
        set_tests_properties(
            ${_test}
            PROPERTIES
                ENVIRONMENT "TSAN_OPTIONS=${_citor_tsan_options}"
                FAIL_REGULAR_EXPRESSION "${_citor_tsan_fail_regex}"
                TIMEOUT 120
        )
    endforeach()
endif()
