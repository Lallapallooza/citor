# Defines the public `citor` INTERFACE target and its `citor::citor` alias.
#
# Picks up include paths, C++20 feature requirement, pthread, optional AVX2
# compile options, the worker stack-size define, and the optional pool
# diagnostic counters define.

add_library(citor INTERFACE)
add_library(citor::citor ALIAS citor)

target_include_directories(
    citor
    INTERFACE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_compile_features(citor INTERFACE cxx_std_20)

find_package(Threads REQUIRED)
target_link_libraries(citor INTERFACE Threads::Threads)

if(CITOR_USE_AVX2)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-mavx2" CITOR_HAS_MAVX2)
    if(CITOR_HAS_MAVX2)
        target_compile_options(citor INTERFACE -mavx2 -mfma)
        target_compile_definitions(citor INTERFACE CITOR_USE_AVX2)
    endif()
endif()

target_compile_definitions(
    citor
    INTERFACE CITOR_WORKER_STACK_KIB=${CITOR_WORKER_STACK_KIB}
)

if(CITOR_ENABLE_POOL_COUNTERS)
    target_compile_definitions(citor INTERFACE CITOR_ENABLE_POOL_COUNTERS)
endif()

if(CITOR_BUILD_WITH_SANITIZER)
    target_compile_options(
        citor
        INTERFACE -fsanitize=thread -fno-omit-frame-pointer
    )
    target_link_options(citor INTERFACE -fsanitize=thread)
endif()
