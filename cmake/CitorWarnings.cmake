# `citor_warnings` INTERFACE target. Kept separate from the public `citor`
# target so consumers do not inherit our warning levels.
#
# Wired only when this is the top-level project; tests and bench targets
# link against it explicitly.

if(NOT CITOR_IS_TOP_LEVEL)
    return()
endif()

add_library(citor_warnings INTERFACE)
target_compile_options(
    citor_warnings
    INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Woverloaded-virtual
        -Wconversion
        -Wsign-conversion
        -Wnull-dereference
        -Wdouble-promotion
        -Wformat=2
)
