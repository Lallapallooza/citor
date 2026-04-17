include(${PROJECT_SOURCE_DIR}/cmake/GTestDep.cmake)
include(GoogleTest)

set(_citor_tests
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
