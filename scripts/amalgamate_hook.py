#!/usr/bin/env python3
"""Regenerate `single_include/citor.hpp` and `git add` it.

Wired to fire when `include/citor/**.h` or `tools/amalgamate.py` is
staged."""

import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SINGLE_INCLUDE = REPO_ROOT / "single_include" / "citor.hpp"


def main() -> int:
    rc = subprocess.call([sys.executable, str(REPO_ROOT / "tools" / "amalgamate.py")])
    if rc != 0:
        sys.stderr.write("amalgamate: regeneration failed\n")
        return rc
    if not SINGLE_INCLUDE.is_file():
        sys.stderr.write(f"amalgamate: expected {SINGLE_INCLUDE} after regeneration\n")
        return 2
    rc = subprocess.call(["git", "add", str(SINGLE_INCLUDE)])
    if rc != 0:
        sys.stderr.write("amalgamate: failed to git-add the regenerated file\n")
        return rc
    return 0


if __name__ == "__main__":
    sys.exit(main())
