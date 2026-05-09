# Install rules and package config. Generates citor-config.cmake and
# citor-config-version.cmake (SameMajorVersion compat) so consumers can
# call `find_package(citor X.Y REQUIRED)`.

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(
    FILES "${PROJECT_SOURCE_DIR}/single_include/citor.hpp"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    OPTIONAL
)
install(TARGETS citor EXPORT citorTargets)
install(
    EXPORT citorTargets
    FILE citorTargets.cmake
    NAMESPACE citor::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/citor
)

configure_package_config_file(
    ${PROJECT_SOURCE_DIR}/cmake/citorConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/citor-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/citor
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/citor-config-version.cmake"
    COMPATIBILITY SameMajorVersion
)
install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/citor-config.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/citor-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/citor
)
