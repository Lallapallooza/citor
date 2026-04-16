cmake_minimum_required(VERSION 3.20)
project(
    citor
    VERSION 0.1.0
    LANGUAGES CXX
)

if(PROJECT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(CITOR_IS_TOP_LEVEL ON)
else()
    set(CITOR_IS_TOP_LEVEL OFF)
endif()

option(CITOR_BUILD_TESTS "Build the citor test suite" ${CITOR_IS_TOP_LEVEL})
option(CITOR_BUILD_BENCHMARK "Build the comparative bench harness" ${CITOR_IS_TOP_LEVEL})
option(CITOR_ENABLE_CLANG_TIDY "Wire clang-tidy into per-target builds" ${CITOR_IS_TOP_LEVEL})
option(CITOR_USE_AVX2 "Compile with AVX2 + FMA when supported" ON)
option(CITOR_BUILD_WITH_SANITIZER "Build with ThreadSanitizer" OFF)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/CPM.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/Tooling.cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CheckCXXCompilerFlag)
if(CITOR_USE_AVX2)
    check_cxx_compiler_flag("-mavx2" CITOR_HAS_AVX2)
endif()

add_library(citor INTERFACE)
add_library(citor::citor ALIAS citor)

target_compile_features(citor INTERFACE cxx_std_20)
target_include_directories(citor INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

find_package(Threads REQUIRED)
target_link_libraries(citor INTERFACE Threads::Threads)

if(CITOR_USE_AVX2 AND CITOR_HAS_AVX2)
    target_compile_options(citor INTERFACE -mavx2 -mfma)
    target_compile_definitions(citor INTERFACE CITOR_USE_AVX2)
endif()

if(CITOR_BUILD_WITH_SANITIZER)
    target_compile_options(citor INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(citor INTERFACE -fsanitize=thread)
endif()

if(CITOR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(CITOR_BUILD_BENCHMARK)
    add_subdirectory(benchmark)
endif()
