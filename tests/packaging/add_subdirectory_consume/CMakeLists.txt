cmake_minimum_required(VERSION 3.24)
project(add_subdirectory_consume LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# add_subdirectory consume against a local source path. The packaging test
# runner sets CITOR_LOCAL_SOURCE on the cmake command line.
if(NOT DEFINED CITOR_LOCAL_SOURCE)
    message(
        FATAL_ERROR
        "Set -DCITOR_LOCAL_SOURCE=<repo-root> to run this test."
    )
endif()

# add_subdirectory consume does not set CITOR_IS_TOP_LEVEL, so optional
# subdirectories (tests, benchmark, warnings) stay disabled.
add_subdirectory(${CITOR_LOCAL_SOURCE} citor_build)

add_executable(add_subdirectory_consume main.cpp)
target_link_libraries(add_subdirectory_consume PRIVATE citor::citor)
