cmake_minimum_required(VERSION 3.20)
project(find_package_consume LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(citor 0.1 REQUIRED)

add_executable(find_package_consume main.cpp)
target_link_libraries(find_package_consume PRIVATE citor::citor)
