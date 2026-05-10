#!/usr/bin/env python3
"""Run clang-tidy on the project.

Default mode tidies the C++ files passed as positional arguments and backs the
`clang-tidy` pre-commit hook; it looks for `compile_commands.json` under any
`build*/` directory and exits 0 if none is present so a fresh checkout does not
block commits.

`--full` configures and builds `build-tidy/` with `CITOR_ENABLE_CLANG_TIDY=ON`
to match the CI `clang-tidy` job; it backs the `clang-tidy-full` pre-push hook.
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
FULL_BUILD_DIR = REPO_ROOT / "build-tidy"

CLANG_TIDY_NAMES = (
    "clang-tidy",
    "clang-tidy-21",
    "clang-tidy-20",
    "clang-tidy-19",
    "clang-tidy-18",
)


def find_clang_tidy() -> str:
    for name in CLANG_TIDY_NAMES:
        path = shutil.which(name)
        if path:
            return path
    names = ", ".join(CLANG_TIDY_NAMES)
    sys.stderr.write(f"clang-tidy not found on PATH; install one of: {names}\n")
    sys.exit(2)


def find_compile_db() -> Path | None:
    for candidate in sorted(REPO_ROOT.glob("build*")):
        db = candidate / "compile_commands.json"
        if db.is_file():
            return candidate
    return None


def run_per_file(files: list[Path]) -> int:
    if not files:
        return 0
    tidy = find_clang_tidy()
    db_dir = find_compile_db()
    if db_dir is None:
        sys.stderr.write(
            "clang-tidy: no build/compile_commands.json found; "
            "configure once with `cmake -S . -B build` to enable the pre-commit check\n"
        )
        return 0
    failures = 0
    for f in files:
        try:
            f.resolve().relative_to(REPO_ROOT)
        except ValueError:
            continue
        rc = subprocess.call([tidy, "-p", str(db_dir), "--quiet", str(f)])
        if rc != 0:
            failures += 1
    return 1 if failures else 0


def run_full() -> int:
    # Wires `CMAKE_CXX_CLANG_TIDY` so tidy runs against every translation unit.
    # Uses a dedicated `build-tidy/` tree so the developer's primary build
    # directory is untouched.
    find_clang_tidy()
    if not (FULL_BUILD_DIR / "CMakeCache.txt").is_file():
        FULL_BUILD_DIR.mkdir(exist_ok=True)
        rc = subprocess.call(
            [
                "cmake",
                "-S",
                str(REPO_ROOT),
                "-B",
                str(FULL_BUILD_DIR),
                "-G",
                "Ninja",
                "-DCITOR_BUILD_BENCHMARK=OFF",
                "-DCITOR_ENABLE_CLANG_TIDY=ON",
            ],
        )
        if rc != 0:
            return rc
    return subprocess.call(
        ["cmake", "--build", str(FULL_BUILD_DIR), "-j", str(os.cpu_count() or 4)],
    )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(allow_abbrev=False)
    parser.add_argument("--full", action="store_true", help="run the full CI-parity build")
    parser.add_argument("files", nargs="*", help="C++ files (per-file mode)")
    args = parser.parse_args(argv[1:])
    if args.full:
        return run_full()
    cxx_suffixes = (".cpp", ".cc", ".cxx", ".h", ".hpp", ".hxx")
    files = [Path(p) for p in args.files if p.endswith(cxx_suffixes)]
    return run_per_file(files)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
