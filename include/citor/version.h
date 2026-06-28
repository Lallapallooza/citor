#pragma once

// Compile-time version string. Bumped automatically by `cz bump` per the
// `version_files` list in `pyproject.toml`. The amalgamated header reads the
// same string from `CMakeLists.txt`'s `project(... VERSION ...)` line.
//
// Integer comparison is omitted here: CMake consumers use
// `find_package(citor X.Y REQUIRED)` for version gating, and runtime callers
// can split this string. Inventing `CITOR_VERSION_MAJOR/MINOR/PATCH` macros
// is one more place commitizen would have to keep in sync, and lines like
// `#define CITOR_VERSION_PATCH 0` lose the version literal a regex can pin
// against.

#define CITOR_VERSION_STRING "0.6.0"
