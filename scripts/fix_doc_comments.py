"""Convert `//`-style declaration docs to `///` for `wrong-style` sites.

Reads the violation list from `check_doc_comments.py`, then for each
`wrong-style (use ///) doc:` site walks the contiguous `//` comment
block immediately above the declaration and prepends a slash on every
line that starts with `//` but not `///`. Idempotent.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

_DECL_LINE_RE = re.compile(r"^([^:]+):(\d+):\s*wrong-style \(use `///`\) doc:")
_LEADING_DOUBLE_SLASH_RE = re.compile(r"^(\s*)//(\s|$)")


def parse_violations() -> dict[Path, set[int]]:
    """Run the checker and parse `wrong-style` lines into {file: {decl_line}}."""
    proc = subprocess.run(
        [sys.executable, str(REPO_ROOT / "scripts" / "check_doc_comments.py")],
        capture_output=True,
        text=True,
        check=False,
    )
    sites: dict[Path, set[int]] = {}
    for line in proc.stderr.splitlines():
        match = _DECL_LINE_RE.match(line)
        if not match:
            continue
        rel = Path(match.group(1))
        decl_line = int(match.group(2))
        sites.setdefault(REPO_ROOT / rel, set()).add(decl_line)
    return sites


def fix_file(path: Path, decl_lines: set[int]) -> int:
    """Prepend `/` to every `//` line in the comment block above each
    decl line. Returns count of lines rewritten."""
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines()
    rewritten = 0
    for decl_line in decl_lines:
        idx = decl_line - 2  # decl_line is 1-based; walk above it
        # Skip past attribute / template lines.
        while idx >= 0 and (lines[idx].lstrip().startswith(("[[", "template", "alignas"))):
            idx -= 1
        # Walk back over contiguous // comment block (not ///).
        while idx >= 0:
            stripped = lines[idx].lstrip()
            if stripped.startswith("///"):
                idx -= 1
                continue
            if stripped.startswith("//"):
                lines[idx] = _LEADING_DOUBLE_SLASH_RE.sub(r"\1///\2", lines[idx])
                rewritten += 1
                idx -= 1
                continue
            break
    if rewritten:
        path.write_text("\n".join(lines) + ("\n" if text.endswith("\n") else ""), encoding="utf-8")
    return rewritten


def main() -> int:
    sites = parse_violations()
    if not sites:
        print("no wrong-style sites found")
        return 0
    total = 0
    for path, decl_lines in sorted(sites.items()):
        rewritten = fix_file(path, decl_lines)
        rel = path.relative_to(REPO_ROOT)
        print(f"{rel}: rewrote {rewritten} comment line(s) above {len(decl_lines)} decl(s)")
        total += rewritten
    print(f"total: {total} comment lines converted")
    return 0


if __name__ == "__main__":
    sys.exit(main())
