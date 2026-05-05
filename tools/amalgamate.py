#!/usr/bin/env python3
# Generates `single_include/citor.hpp` from the modular tree under `include/`.
#
# Algorithm (nlohmann/json model):
#   1. Walk every public entry header (`include/citor/*.h` plus
#      `include/citor/cpos/*.h`) in dependency-respecting DFS order.
#   2. Collect the union of `#include <...>` system headers the tree uses.
#   3. Emit a single prologue (project name, version, license, generation
#      stamp), one `#pragma once`, the deduplicated system include block, and
#      the merged project bodies in DFS post-order. Per-file `#pragma once`
#      and project `#include "citor/..."` lines are stripped during the merge
#      since they are subsumed by the single output.
#
# Usage:
#   tools/amalgamate.py                  -- write single_include/citor.hpp
#   tools/amalgamate.py --check          -- exit nonzero if the on-disk
#                                           single_include/citor.hpp differs
#                                           from the freshly generated content
#   tools/amalgamate.py --output PATH    -- write to a custom path
#
# Run with the repo root as CWD or pass `--repo-root` to override.

from __future__ import annotations

import argparse
import datetime
import os
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT_DEFAULT = Path(__file__).resolve().parent.parent

# Public entry-point headers. Anything reachable through the include graph from
# this set is pulled into the amalgamation; private detail/ headers are reached
# transitively through the public surface.
ENTRY_RELATIVE = [
    "citor/version.h",
    "citor/always_assert.h",
    "citor/function_ref.h",
    "citor/cancellation.h",
    "citor/hints.h",
    "citor/chain.h",
    "citor/thread_pool.h",
    "citor/pool_group.h",
    "citor/cpos/parallel_for.h",
    "citor/cpos/parallel_reduce.h",
    "citor/cpos/parallel_scan.h",
    "citor/cpos/parallel_chain.h",
    "citor/cpos/run_plex.h",
    "citor/cpos/bulk_for_queries.h",
    "citor/cpos/fork_join.h",
    "citor/cpos/submit_detached.h",
]

# Lines starting with these patterns are dropped during the merge (system
# includes are emitted in the prologue, project includes have already been
# merged inline, and `#pragma once` is emitted once at the top).
RE_PROJECT_INCLUDE = re.compile(r'^\s*#\s*include\s*"((?:citor)/[^"]+)"\s*$')
RE_SYSTEM_INCLUDE = re.compile(r'^\s*#\s*include\s*<([^>]+)>\s*$')
RE_PRAGMA_ONCE = re.compile(r"^\s*#\s*pragma\s+once\s*$")


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def parse_includes(source: str) -> tuple[list[str], list[str]]:
    # Returns (project_relpaths, system_relpaths) in source order. Comments
    # and string literals can shadow `#include` lines in pathological cases;
    # the citor source style does not produce those, so a line-level regex is
    # sufficient. If that ever changes, the comment-stripping pass goes here.
    project: list[str] = []
    system: list[str] = []
    for line in source.splitlines():
        m = RE_PROJECT_INCLUDE.match(line)
        if m:
            project.append(m.group(1))
            continue
        m = RE_SYSTEM_INCLUDE.match(line)
        if m:
            system.append(m.group(1))
            continue
    return project, system


def strip_lines(source: str) -> str:
    # Removes `#pragma once`, project `#include "citor/..."`, and system
    # `#include <...>` lines from a per-file body. The amalgamation emits each
    # exactly once at the top of the output.
    out: list[str] = []
    for line in source.splitlines():
        if RE_PRAGMA_ONCE.match(line):
            continue
        if RE_PROJECT_INCLUDE.match(line):
            continue
        if RE_SYSTEM_INCLUDE.match(line):
            continue
        out.append(line)
    return "\n".join(out).rstrip() + "\n"


def dfs(
    rel: str,
    include_dir: Path,
    visited: set[str],
    order: list[str],
    system_set: set[str],
) -> None:
    if rel in visited:
        return
    visited.add(rel)
    full = include_dir / rel
    src = read_text(full)
    project, system = parse_includes(src)
    for s in system:
        system_set.add(s)
    for p in project:
        dfs(p, include_dir, visited, order, system_set)
    order.append(rel)


def project_version(repo_root: Path) -> str:
    text = read_text(repo_root / "CMakeLists.txt")
    m = re.search(r"VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)", text)
    if m is None:
        return "0.0.0"
    return m.group(1)


def git_sha(repo_root: Path) -> str:
    try:
        out = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=repo_root, stderr=subprocess.DEVNULL
        )
        return out.decode("ascii").strip()
    except (subprocess.CalledProcessError, FileNotFoundError, OSError):
        return "unknown"


def sort_system_includes(items: list[str]) -> list[str]:
    # Two groups, matching the project's IncludeCategories:
    #   Group A: C-style headers (`*.h`) -> sorted, emitted first.
    #   Group B: C++ standard headers   -> sorted, emitted second.
    c_headers = sorted({s for s in items if s.endswith(".h")})
    cxx_headers = sorted({s for s in items if not s.endswith(".h")})
    return c_headers, cxx_headers  # type: ignore[return-value]


def render(
    order: list[str],
    include_dir: Path,
    system_set: set[str],
    version: str,
    sha: str,
) -> str:
    today = datetime.date.today().isoformat()
    parts: list[str] = []
    parts.append("// SPDX-License-Identifier: MIT")
    parts.append("//")
    parts.append("// citor -- header-only C++20 thread pool")
    parts.append(f"// version: {version}")
    parts.append(f"// commit:  {sha}")
    parts.append(f"// generated: {today}")
    parts.append("//")
    parts.append("// GENERATED FILE -- DO NOT EDIT.")
    parts.append("// Run `python tools/amalgamate.py` to regenerate.")
    parts.append("// Modular sources live under `include/citor/`.")
    parts.append("")
    parts.append("#pragma once")
    parts.append("")

    c_headers, cxx_headers = sort_system_includes(sorted(system_set))
    if c_headers:
        for h in c_headers:
            parts.append(f"#include <{h}>")
        parts.append("")
    if cxx_headers:
        for h in cxx_headers:
            parts.append(f"#include <{h}>")
        parts.append("")

    for rel in order:
        full = include_dir / rel
        body = strip_lines(read_text(full))
        parts.append(f"// ===== {rel} =====")
        parts.append(body.rstrip())
        parts.append("")

    return "\n".join(parts).rstrip() + "\n"


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--repo-root", default=str(REPO_ROOT_DEFAULT))
    p.add_argument("--output", default=None,
                   help="output path (default: <repo>/single_include/citor.hpp)")
    p.add_argument("--check", action="store_true",
                   help="exit nonzero if on-disk file differs from generated")
    args = p.parse_args()

    repo_root = Path(args.repo_root).resolve()
    include_dir = repo_root / "include"
    output_path = Path(args.output) if args.output else (
        repo_root / "single_include" / "citor.hpp"
    )

    visited: set[str] = set()
    order: list[str] = []
    system_set: set[str] = set()
    for rel in ENTRY_RELATIVE:
        dfs(rel, include_dir, visited, order, system_set)

    rendered = render(
        order, include_dir, system_set,
        project_version(repo_root),
        git_sha(repo_root),
    )

    if args.check:
        if not output_path.exists():
            print(f"amalgamate: {output_path} does not exist", file=sys.stderr)
            return 2
        on_disk = read_text(output_path)
        if on_disk != rendered:
            print(
                f"amalgamate: {output_path} is out of sync with include/. "
                "Run `python tools/amalgamate.py` and commit the result.",
                file=sys.stderr,
            )
            return 1
        return 0

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(rendered, encoding="utf-8")
    rel_out = os.path.relpath(output_path, repo_root)
    print(f"amalgamate: wrote {rel_out} ({output_path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
