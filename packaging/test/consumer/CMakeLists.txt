cmake_minimum_required(VERSION 3.20)
project(citor_packaging_smoke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(citor 0.1.0 REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE citor::citor)
