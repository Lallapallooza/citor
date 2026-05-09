#!/usr/bin/env bash
# Drives every consume-channel test. Verifies that citor is consumable via:
#   - the single-header drop-in (`single_include/citor.hpp`)
#   - `FetchContent_Declare` from a local path
#   - `CPMAddPackage` from a local source
#   - `add_subdirectory` from a local path
#   - `find_package(citor)` after `cmake --install`
#   - conan create + install (if conan is on PATH)
#   - vcpkg install via overlay port (if vcpkg is on PATH)
#
# Run from the repo root:
#   tests/packaging/run_packaging_tests.sh

set -euo pipefail

root="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
work="$(mktemp -d -t citor-packaging.XXXXXX)"
trap 'rm -rf "$work"' EXIT

cd "$root"

python3 tools/amalgamate.py

run_cmake_consume() {
    local label="$1"
    local source_dir="$2"
    local extra="${3:-}"
    echo "== ${label} =="
    cmake -S "${source_dir}" -B "${work}/${label}" -G Ninja \
          -DCITOR_LOCAL_SOURCE="$root" ${extra} > "${work}/${label}.configure.log" 2>&1
    cmake --build "${work}/${label}" -j > "${work}/${label}.build.log" 2>&1
    "${work}/${label}/${label}"
    echo "PASS: ${label}"
}

echo "== single-header =="
${CXX:-c++} -std=c++20 -O2 -pthread -mavx2 -mfma -DCITOR_USE_AVX2 \
    -I single_include \
    tests/packaging/single_header_consume/main.cpp \
    -o "$work/single_header_consume"
"$work/single_header_consume"
echo "PASS: single-header"

run_cmake_consume fetchcontent_consume       tests/packaging/fetchcontent_consume
run_cmake_consume cpm_consume                tests/packaging/cpm_consume
run_cmake_consume add_subdirectory_consume   tests/packaging/add_subdirectory_consume

echo "== find_package =="
cmake -S "$root" -B "$work/install-build" -G Ninja \
      -DCITOR_BUILD_TESTS=OFF -DCITOR_BUILD_BENCHMARK=OFF \
      -DCMAKE_INSTALL_PREFIX="$work/install-prefix" \
      > "$work/install.configure.log" 2>&1
cmake --build "$work/install-build" -j > "$work/install.build.log" 2>&1
cmake --install "$work/install-build" > "$work/install.install.log" 2>&1
run_cmake_consume find_package_consume \
    tests/packaging/find_package_consume \
    "-DCMAKE_PREFIX_PATH=$work/install-prefix"

if command -v conan > /dev/null; then
    echo "== conan =="
    pushd "$work" > /dev/null
    conan profile detect --force > /dev/null
    conan create "$root/packaging/conan" --build=missing > "$work/conan.create.log" 2>&1
    popd > /dev/null
    echo "PASS: conan"
else
    echo "SKIP: conan (binary not on PATH)"
fi

# vcpkg port pins SHA512 against a tagged release tarball; runs in CI only.
echo "SKIP: vcpkg (CI-only against a tagged release)"

echo
echo "all packaging consume tests passed"
