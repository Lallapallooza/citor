include(${PROJECT_SOURCE_DIR}/cmake/GTestDep.cmake)
include(GoogleTest)

set(_citor_tests
    bench_format_test
    bulk_for_queries_exception_test
    bulk_for_queries_test
    chase_lev_deque_test
    fork_join_test
    parallel_chain_test
    parallel_for_test
    parallel_for_tsan_test
    parallel_pool_deadlock_fuzz_test
    parallel_pool_tsan_stress_test
    parallel_reduce_cross_run_determinism_test
    parallel_reduce_determinism_test
    parallel_reduce_test
    parallel_scan_test
    pool_group_test
    priority_queue_test
    run_plex_test
    submit_detached_test
    thread_pool_lifecycle_test
    value_types_test
)

foreach(_t IN LISTS _citor_tests)
    add_executable(${_t} ${_t}.cpp)
    target_link_libraries(${_t} PRIVATE citor::citor GTest::gtest_main)
    if(CITOR_IS_TOP_LEVEL)
        target_link_libraries(${_t} PRIVATE citor_warnings)
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(${_t} PRIVATE pthread)
    endif()
    citor_enable_tidy(${_t})
    gtest_discover_tests(${_t})
endforeach()
