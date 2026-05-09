"""Require a `///` doc comment above every declaration in `include/citor/`.

Tracks brace depth, so only declarations at namespace, class, struct, or
template-body scope get flagged. Function bodies are skipped.

Args: paths to scan, or empty for the whole `include/` tree."""

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
INCLUDE_ROOT = REPO_ROOT / "include" / "citor"

DECL_RE = re.compile(
    r"^\s*"
    r"(?:template\s*<[^>]*>\s*)?"
    r"(?:\[\[[^\]]+\]\]\s*)*"
    r"(?:friend\s+)?"
    r"(?:explicit\s+)?"
    r"(?:inline\s+|static\s+|constexpr\s+|virtual\s+)*"
    r"(?:"
    r"(class|struct|enum\s+class|enum|union)\s+\w"
    r"|"
    r"(?:typename\s+\w[\w:<>, ]*\s+)?(?:\w[\w:<>, &\*]*)\s+\w+\s*[\(\{=]"
    r")"
)
SKIP_RE = re.compile(r"^\s*(//|#|using|namespace\s|extern|friend|return|//===)")
ANON_NS_RE = re.compile(r"^\s*namespace\s*\{")
NAMED_NS_RE = re.compile(r"^\s*namespace\s+\w[\w:]*\s*\{")
NS_CLOSE_RE = re.compile(r"^\s*\}\s*//\s*namespace\b")


def has_preceding_doc(lines: list[str], idx: int) -> bool:
    """True when `///` precedes the declaration, ignoring `[[attr]]`,
    `template<>`, and blank lines."""
    j = idx - 1
    while j >= 0:
        s = lines[j].strip()
        if not s:
            j -= 1
            continue
        if s.startswith("///"):
            return True
        if s.startswith(("[[", "template", "//")):
            j -= 1
            continue
        return False
    return False


def scan(path: Path) -> list[tuple[int, str]]:
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines()
    issues: list[tuple[int, str]] = []
    brace_depth = 0  # function-body depth; top-of-namespace is 0
    in_anon = 0
    for idx, line in enumerate(lines):
        if ANON_NS_RE.match(line):
            in_anon += 1
            brace_depth += 1
            continue
        if NAMED_NS_RE.match(line):
            brace_depth += 1
            continue
        if NS_CLOSE_RE.match(line):
            if in_anon > 0:
                in_anon -= 1
            brace_depth = max(0, brace_depth - 1)
            continue

        if (
            brace_depth == 0
            and not in_anon
            and not SKIP_RE.match(line)
            and DECL_RE.match(line)
            and not has_preceding_doc(lines, idx)
        ):
            issues.append((idx + 1, line.rstrip()))

        brace_depth += line.count("{") - line.count("}")
        brace_depth = max(brace_depth, 0)

    return issues


def main(argv: list[str]) -> int:
    if argv[1:]:
        files = [Path(p) for p in argv[1:] if p.endswith(".h")]
    else:
        files = sorted(INCLUDE_ROOT.rglob("*.h"))
    if not files:
        return 0
    fail = 0
    for f in files:
        for line_no, snippet in scan(f):
            try:
                rel = f.relative_to(REPO_ROOT)
            except ValueError:
                rel = f
            sys.stderr.write(f"{rel}:{line_no}: missing /// doc: {snippet}\n")
            fail += 1
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
