set(CITOR_IS_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(CITOR_IS_TOP_LEVEL ON)
endif()

if(CITOR_ENABLE_CLANG_TIDY)
    find_program(
        CLANG_TIDY_EXE
        NAMES clang-tidy clang-tidy-21 clang-tidy-20 clang-tidy-19 clang-tidy-18
        REQUIRED
    )
    # Route every TU's tidy invocation through a wrapper that consults the
    # build-time `CITOR_TIDY_FILES` env var (newline- or colon-separated
    # repo-relative source paths). The wrapper exits 0 for TUs not in the
    # list, which lets CI run tidy only on changed files without splitting
    # the build into a separate compile-then-tidy pass. When
    # `CITOR_TIDY_FILES` is unset (the local-dev default), tidy runs on
    # every TU. The wrapper picks the real clang-tidy via
    # `CITOR_CLANG_TIDY_BIN` (defaults to the first `clang-tidy` on PATH)
    # and the repo root via `CITOR_REPO_ROOT` (defaults to `git rev-parse`
    # of the wrapper's cwd).
    set(CMAKE_CXX_CLANG_TIDY
        "${CMAKE_SOURCE_DIR}/scripts/clang-tidy-diff-gate.sh"
    )
endif()

if(CITOR_IS_TOP_LEVEL)
    find_program(CLANG_FORMAT_EXE NAMES clang-format)
    if(CLANG_FORMAT_EXE)
        file(
            GLOB_RECURSE CITOR_FORMAT_SOURCES
            CONFIGURE_DEPENDS
            "${CMAKE_SOURCE_DIR}/include/citor/*.h"
            "${CMAKE_SOURCE_DIR}/benchmark/*.cpp"
            "${CMAKE_SOURCE_DIR}/benchmark/*.h"
            "${CMAKE_SOURCE_DIR}/tests/*.cpp"
        )

        add_custom_target(
            format
            COMMAND ${CLANG_FORMAT_EXE} -i ${CITOR_FORMAT_SOURCES}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Running clang-format on project sources"
        )

        add_custom_target(
            check-format
            COMMAND
                ${CLANG_FORMAT_EXE} --dry-run --Werror ${CITOR_FORMAT_SOURCES}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Checking clang-format compliance"
        )
    endif()
endif()
