cmake_minimum_required(VERSION 3.20)
project(citor_fetchcontent_smoke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# CITOR_SOURCE_DIR points at the citor repo checkout. The flow exercises the
# `FetchContent_MakeAvailable` consumption path without depending on a public
# tag.
if(NOT DEFINED CITOR_SOURCE_DIR)
    message(FATAL_ERROR "CITOR_SOURCE_DIR must point at the citor repo root.")
endif()

include(FetchContent)
FetchContent_Declare(citor SOURCE_DIR "${CITOR_SOURCE_DIR}")
FetchContent_MakeAvailable(citor)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE citor::citor)
