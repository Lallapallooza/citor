#!/usr/bin/env python3
"""Pre-push hook. Builds the project and runs ctest.

Picks the first `build*/` containing a CMakeCache. If none exists,
configures `build/` with `-DCITOR_BUILD_BENCHMARK=OFF`."""

import os
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


def find_build_dir() -> Path:
    candidates = sorted(p for p in REPO_ROOT.glob("build*") if (p / "CMakeCache.txt").is_file())
    if candidates:
        return candidates[0]
    # No configured build tree; configure a minimal test-only one.
    build = REPO_ROOT / "build"
    build.mkdir(exist_ok=True)
    rc = subprocess.call(
        [
            "cmake",
            "-S",
            str(REPO_ROOT),
            "-B",
            str(build),
            "-G",
            "Ninja",
            "-DCITOR_BUILD_BENCHMARK=OFF",
        ],
    )
    if rc != 0:
        sys.exit(rc)
    return build


def main() -> int:
    build = find_build_dir()
    rc = subprocess.call(["cmake", "--build", str(build), "-j", str(os.cpu_count() or 4)])
    if rc != 0:
        return rc
    return subprocess.call(
        ["ctest", "--test-dir", str(build), "--output-on-failure", "-j", str(os.cpu_count() or 4)],
    )


if __name__ == "__main__":
    sys.exit(main())
