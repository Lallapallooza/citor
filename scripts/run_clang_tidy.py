#!/usr/bin/env python3
"""Run clang-tidy on the C++ files passed as positional arguments.

Looks for `compile_commands.json` under any `build*/` directory. Exits 0
without running if none is found (so a fresh checkout does not block
commits before the developer has configured a build)."""

import shutil
import subprocess
import sys
from pathlib import Path

CLANG_TIDY_NAMES = ("clang-tidy", "clang-tidy-21", "clang-tidy-20", "clang-tidy-19", "clang-tidy-18")


def find_clang_tidy() -> str:
    for name in CLANG_TIDY_NAMES:
        path = shutil.which(name)
        if path:
            return path
    sys.stderr.write("clang-tidy not found on PATH; install one of: " + ", ".join(CLANG_TIDY_NAMES) + "\n")
    sys.exit(2)


def find_compile_db() -> Path | None:
    repo = Path(__file__).resolve().parent.parent
    for candidate in sorted(repo.glob("build*")):
        db = candidate / "compile_commands.json"
        if db.is_file():
            return candidate
    return None


def main(argv: list[str]) -> int:
    files = [Path(p) for p in argv[1:] if p.endswith((".cpp", ".cc", ".cxx", ".h", ".hpp", ".hxx"))]
    if not files:
        return 0
    tidy = find_clang_tidy()
    db_dir = find_compile_db()
    if db_dir is None:
        sys.stderr.write(
            "clang-tidy: no build/compile_commands.json found; configure once with `cmake -S . -B build` to enable the pre-commit check\n"
        )
        return 0
    failures = 0
    for f in files:
        try:
            f.resolve().relative_to(Path(__file__).resolve().parent.parent)
        except ValueError:
            continue
        rc = subprocess.call([tidy, "-p", str(db_dir), "--quiet", str(f)])
        if rc != 0:
            failures += 1
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
