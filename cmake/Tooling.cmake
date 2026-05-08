set(CITOR_IS_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(CITOR_IS_TOP_LEVEL ON)
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
