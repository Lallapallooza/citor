cmake_minimum_required(VERSION 3.24)
project(cpm_consume LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# CPM consume against a local source path. The packaging test runner sets
# CITOR_LOCAL_SOURCE on the cmake command line.
if(NOT DEFINED CITOR_LOCAL_SOURCE)
    message(FATAL_ERROR "Set -DCITOR_LOCAL_SOURCE=<repo-root> to run this test.")
endif()

include(${CITOR_LOCAL_SOURCE}/cmake/CPM.cmake)
CPMAddPackage(NAME citor SOURCE_DIR ${CITOR_LOCAL_SOURCE})

add_executable(cpm_consume main.cpp)
target_link_libraries(cpm_consume PRIVATE citor::citor)
