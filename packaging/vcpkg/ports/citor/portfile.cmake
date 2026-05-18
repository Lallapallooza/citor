# Overlay port for citor. Use as:
#   vcpkg install citor --overlay-ports=path/to/citor/packaging/vcpkg/ports
#
# When the upstream microsoft/vcpkg PR merges, the overlay flag goes away
# and the canonical `vcpkg install citor` works.
#
# Uses `vcpkg_from_git` so the git commit SHA is the verification: there is
# no tarball hash to recompute per release. To bump the version, update
# `vcpkg.json` `version`; `${VERSION}` flows from there.

vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL https://github.com/Lallapallooza/citor.git
    REF "v${VERSION}"
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DCITOR_BUILD_TESTS=OFF
        -DCITOR_BUILD_BENCHMARK=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME citor CONFIG_PATH lib/cmake/citor)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
