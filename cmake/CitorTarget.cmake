# Defines the public `citor` INTERFACE target and its `citor::citor` alias.
#
# Picks up include paths, C++20 feature requirement, pthread, the worker
# stack-size define, and the optional pool diagnostic counters define.

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

# MSVC's C4324 ("structure was padded due to alignment specifier") is the
# expected outcome of every `alignas(kCacheLine)` member in the engine.
# Suppressing it on the public INTERFACE target so consumers see no false-
# alarm noise when including citor headers. GCC/Clang do not emit this
# warning at all.
if(MSVC)
    target_compile_options(citor INTERFACE /wd4324)
endif()

target_compile_definitions(
    citor
    INTERFACE CITOR_WORKER_STACK_KIB=${CITOR_WORKER_STACK_KIB}
)

if(CITOR_ENABLE_POOL_COUNTERS)
    target_compile_definitions(citor INTERFACE CITOR_ENABLE_POOL_COUNTERS)
endif()

if(CITOR_BUILD_WITH_SANITIZER)
    if(MSVC)
        # MSVC has no ThreadSanitizer. Surface the mismatch instead of
        # silently dropping the user-visible toggle.
        message(
            WARNING
            "CITOR_BUILD_WITH_SANITIZER is ON but MSVC does not ship ThreadSanitizer; sanitizer flags are not applied. Build with clang or gcc to exercise the TSan path."
        )
    else()
        # `-g` is required even in Release: without DWARF debug info, TSan
        # stack traces resolve to `<null>` instead of `file:line` and the
        # report is impossible to read.
        target_compile_options(
            citor
            INTERFACE -fsanitize=thread -fno-omit-frame-pointer -g
        )
        target_link_options(citor INTERFACE -fsanitize=thread)
    endif()
endif()
