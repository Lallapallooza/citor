# Overlay port for citor. Use as:
#   vcpkg install citor --overlay-ports=path/to/citor/packaging/vcpkg/ports
#
# When the upstream microsoft/vcpkg PR merges, the overlay flag goes away
# and the canonical `vcpkg install citor` works.
#
# To bump the version:
#   1. Update REF below to the new tag.
#   2. Run `vcpkg install citor --overlay-ports=...` once locally; vcpkg
#      will print the expected SHA512 on first failure. Paste it into the
#      SHA512 line.
#   3. Update vcpkg.json `version`.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO Lallapallooza/citor
    REF "v${VERSION}"
    SHA512 0
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
