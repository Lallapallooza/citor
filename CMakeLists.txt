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

include(${PROJECT_SOURCE_DIR}/cmake/CPM.cmake)
include(${PROJECT_SOURCE_DIR}/cmake/CitorOptions.cmake)
include(${PROJECT_SOURCE_DIR}/cmake/CitorTarget.cmake)
include(${PROJECT_SOURCE_DIR}/cmake/CitorWarnings.cmake)
include(${PROJECT_SOURCE_DIR}/cmake/Tooling.cmake)

# Bench is added before tests because the bench subdir fetches BS::thread_pool
# via CPM, and the differential tests under tests/ check `TARGET
# bshoshany_thread_pool` to decide which competitors to compile against.
if(CITOR_BUILD_BENCHMARK AND CITOR_IS_TOP_LEVEL)
    add_subdirectory(benchmark)
endif()

if(CITOR_BUILD_TESTS AND CITOR_IS_TOP_LEVEL)
    enable_testing()
    add_subdirectory(tests)
endif()

include(${PROJECT_SOURCE_DIR}/cmake/CitorInstall.cmake)
