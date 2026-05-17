cmake_minimum_required(VERSION 3.20)
project(citor_cpm_smoke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# CITOR_SOURCE_DIR points at the citor repo checkout. The flow exercises the
# `CPMAddPackage` consumption path against the vendored `cmake/CPM.cmake`
# without depending on a public tag.
if(NOT DEFINED CITOR_SOURCE_DIR)
    message(FATAL_ERROR "CITOR_SOURCE_DIR must point at the citor repo root.")
endif()

include("${CITOR_SOURCE_DIR}/cmake/CPM.cmake")
cpmaddpackage(NAME citor SOURCE_DIR "${CITOR_SOURCE_DIR}")

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE citor::citor)
