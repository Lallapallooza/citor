cmake_minimum_required(VERSION 3.24)
project(fetchcontent_consume LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# Consumes citor from a local path (the repo this file lives in). The
# packaging test runner sets CITOR_LOCAL_SOURCE on the cmake command line.
if(NOT DEFINED CITOR_LOCAL_SOURCE)
    message(
        FATAL_ERROR
        "Set -DCITOR_LOCAL_SOURCE=<repo-root> to run this test."
    )
endif()

FetchContent_Declare(citor SOURCE_DIR ${CITOR_LOCAL_SOURCE})
FetchContent_MakeAvailable(citor)

add_executable(fetchcontent_consume main.cpp)
target_link_libraries(fetchcontent_consume PRIVATE citor::citor)
