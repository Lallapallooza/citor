# `citor_warnings` INTERFACE target. Kept separate from the public `citor`
# target so consumers do not inherit our warning levels.
#
# Wired only when this is the top-level project; tests and bench targets
# link against it explicitly.

if(NOT CITOR_IS_TOP_LEVEL)
    return()
endif()

add_library(citor_warnings INTERFACE)
if(MSVC)
    # /W4 plus conformance flags. /WX is omitted because cl emits warnings
    # the GCC/clang preset does not, and turning them into errors would
    # block the build before the Windows-specific diagnostics can be
    # triaged.
    target_compile_options(
        citor_warnings
        INTERFACE
            /W4
            /permissive-
            /Zc:__cplusplus
            /Zc:preprocessor
            # The headers carry [[gnu::cold]] / [[gnu::always_inline]] / ...
            # attribute names. The standard says unknown namespaced
            # attributes are ignored; MSVC's C5030 ("attribute is not
            # recognized") fires anyway. Silence it; the GCC/clang build
            # is the source of truth for inlining decisions.
            /wd5030
    )
else()
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
endif()
