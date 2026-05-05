#!/usr/bin/env bash
# Drives the per-channel consume tests. Verifies that citor is consumable
# via:
#   - the single-header drop-in (`single_include/citor.hpp`)
#   - `FetchContent_Declare` from a local path
#   - `find_package(citor)` after `cmake --install`
#
# Run from the repo root:
#   tests/packaging/run_packaging_tests.sh
#
# Wired into nightly CI; not in the per-push fast lane because the install
# step is slow.

set -euo pipefail

root="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
work="$(mktemp -d -t citor-packaging.XXXXXX)"
trap 'rm -rf "$work"' EXIT

cd "$root"

# Make sure single_include/ is fresh.
python3 tools/amalgamate.py

# 1. Single-header consume.
echo "== single-header =="
${CXX:-c++} -std=c++20 -O2 -pthread -mavx2 -mfma -DCITOR_USE_AVX2 \
    -I single_include \
    tests/packaging/single_header_consume/main.cpp \
    -o "$work/single_header_consume"
"$work/single_header_consume"
echo "PASS: single-header"

# 2. FetchContent consume.
echo "== fetchcontent =="
cmake -S tests/packaging/fetchcontent_consume \
      -B "$work/fetchcontent" \
      -G Ninja \
      -DCITOR_LOCAL_SOURCE="$root" \
      > "$work/fetchcontent.configure.log" 2>&1
cmake --build "$work/fetchcontent" -j > "$work/fetchcontent.build.log" 2>&1
"$work/fetchcontent/fetchcontent_consume"
echo "PASS: fetchcontent"

# 3. find_package consume after install.
echo "== find_package =="
cmake -S "$root" -B "$work/install-build" -G Ninja \
      -DCITOR_BUILD_TESTS=OFF -DCITOR_BUILD_BENCHMARK=OFF \
      -DCMAKE_INSTALL_PREFIX="$work/install-prefix" \
      > "$work/install.configure.log" 2>&1
cmake --build "$work/install-build" -j > "$work/install.build.log" 2>&1
cmake --install "$work/install-build" > "$work/install.install.log" 2>&1

cmake -S tests/packaging/find_package_consume \
      -B "$work/find_package" \
      -G Ninja \
      -DCMAKE_PREFIX_PATH="$work/install-prefix" \
      > "$work/find_package.configure.log" 2>&1
cmake --build "$work/find_package" -j > "$work/find_package.build.log" 2>&1
"$work/find_package/find_package_consume"
echo "PASS: find_package"

echo
echo "all packaging consume tests passed"
