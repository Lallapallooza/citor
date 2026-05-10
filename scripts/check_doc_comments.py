"""Require `///` doc comments on declarations under `include/citor/`.

Walks the C++ AST via `tree-sitter-cpp` and inspects every namespace-
or class-scope declaration. The comment block immediately above the
declaration must contain at least one line starting with `///`. Two
distinct failure modes:

  - `missing doc`     -- no comment block above the declaration.
  - `wrong-style doc` -- comment block above is `//` only; should be `///`.

Forward declarations, friend declarations, `static_assert`, and
declarations inside function/lambda bodies are skipped.
"""

from __future__ import annotations

import sys
from collections.abc import Iterable
from pathlib import Path

try:
    import tree_sitter_cpp
    from tree_sitter import Language, Node, Parser
except ModuleNotFoundError as exc:
    sys.stderr.write(f"check_doc_comments: missing module: {exc.name}\n")
    sys.stderr.write("install via: pip install tree_sitter tree_sitter_cpp\n")
    sys.exit(2)


REPO_ROOT = Path(__file__).resolve().parent.parent
INCLUDE_ROOT = REPO_ROOT / "include" / "citor"

CPP_LANG = Language(tree_sitter_cpp.language())
PARSER = Parser(CPP_LANG)

# Node kinds that represent a declaration the rule applies to. The walk
# enters a node of this kind, evaluates its preceding doc, then either
# stops (function bodies, simple decls) or recurses into a body that
# itself contains more declarations (class / struct / enum).
DECL_KINDS = frozenset(
    {
        "alias_declaration",  # `using Foo = ...;`
        "class_specifier",
        "declaration",  # variable / free-function prototype
        "enum_specifier",
        "enumerator",
        "field_declaration",  # method or data member inside a class
        "function_definition",
        "struct_specifier",
        "template_declaration",
        "type_definition",  # `typedef ...;`
        "union_specifier",
    }
)

# When `template_declaration` wraps an inner declaration, the inner
# declaration is a child of the template. We check the template node and
# then suppress the recursive check on its inner declaration -- the doc
# `///` lives above the `template<>` keyword, not above the inner decl.
INNER_DECL_KINDS_UNDER_TEMPLATE = frozenset(
    {
        "class_specifier",
        "function_definition",
        "declaration",
        "struct_specifier",
        "union_specifier",
        "alias_declaration",
        "type_definition",
    }
)


def decl_text(node: Node, source: bytes) -> str:
    """First line of the declaration source, trimmed for diagnostics."""
    raw = source[node.start_byte : node.end_byte].decode("utf-8", errors="replace")
    first = raw.splitlines()[0] if raw else ""
    return first[:80]


def enclosing_record_name(node: Node) -> str | None:
    """Return the name of the nearest enclosing class/struct/union, or
    None if `node` is not inside one. Walks up `parent` pointers."""
    cur = node.parent
    while cur is not None:
        if cur.type in {"class_specifier", "struct_specifier", "union_specifier"}:
            name_node = cur.child_by_field_name("name")
            if name_node is not None and name_node.text is not None:
                return str(name_node.text.decode("utf-8", errors="replace"))
        cur = cur.parent
    return None


def is_skippable(node: Node, source: bytes) -> bool:
    """Skip declarations exempt from the `///` rule.

    The exempt set covers the cases where the name is self-explanatory
    or the semantics are inherited verbatim from a documented parent:

      - Forward / friend / static_assert.
      - `= delete` / `= default` special members and destructors.
      - Operator overloads (any `operator` keyword as function name).
      - `tag_invoke` overloads (CPO routers; tag type + signature
        already encode the routing target).
      - Field overrides inside a `*Hints` preset (the override
        inherits its meaning from the corresponding field on the
        parent `Hints` struct, where it is documented once).
    """
    # Forward decl: no body field.
    if (
        node.type in {"class_specifier", "struct_specifier", "union_specifier", "enum_specifier"}
        and node.child_by_field_name("body") is None
    ):
        return True
    text = source[node.start_byte : node.end_byte].decode("utf-8", errors="replace").lstrip()
    if (
        text.startswith(("friend ", "static_assert", "~"))
        or "= delete" in text
        or "= default" in text
    ):
        return True
    head = text[:120]
    if "operator" in head:
        import re

        if re.search(
            r"\boperator\s*([\(\[\)\]<>=!+\-*/%^&|~,]|new\b|delete\b|bool\b|\w+\s*\()",
            head,
        ):
            return True
    if "tag_invoke" in head:
        return True
    if node.type == "field_declaration":
        record = enclosing_record_name(node)
        if record is not None and "Hints" in record:
            return True
    return False


def preceding_doc_kind(node: Node, source: bytes) -> str:
    """Inspect the comment block immediately above `node`.

    Walks back through `prev_named_sibling` over comment nodes (and
    `attribute_specifier` / template parameter list noise). Returns
    `triple` if at least one `///` line is present, `double` if only
    `//` lines are present, or `none` if no comment block precedes the
    declaration.
    """
    cursor = node.prev_named_sibling
    saw_comment = False
    saw_triple = False
    while cursor is not None:
        if cursor.type == "comment":
            saw_comment = True
            text = source[cursor.start_byte : cursor.end_byte].decode("utf-8", errors="replace")
            if text.lstrip().startswith("///"):
                saw_triple = True
            cursor = cursor.prev_named_sibling
            continue
        if cursor.type in {"attribute_specifier", "ms_declspec_modifier"}:
            cursor = cursor.prev_named_sibling
            continue
        break
    if saw_triple:
        return "triple"
    return "double" if saw_comment else "none"


def is_inner_decl_of_template(node: Node) -> bool:
    parent = node.parent
    return (
        parent is not None
        and parent.type == "template_declaration"
        and node.type in INNER_DECL_KINDS_UNDER_TEMPLATE
    )


# A nested record type under a `field_declaration` (e.g. `struct Inner { ... }`
# inside a class body) is a child of the field_declaration, not a sibling of
# the surrounding declarations. The doc comment lives above the field_declaration,
# so the inner specifier's `prev_named_sibling` is `None`. Skip the inner
# specifier and rely on the outer field_declaration's doc check.
INNER_RECORD_KINDS = frozenset(
    {"class_specifier", "struct_specifier", "union_specifier", "enum_specifier"}
)


def is_inner_record_of_field_declaration(node: Node) -> bool:
    parent = node.parent
    return (
        parent is not None
        and parent.type == "field_declaration"
        and node.type in INNER_RECORD_KINDS
    )


def container_body(node: Node) -> Node | None:
    """Body node to recurse into for a class / struct / enum / namespace."""
    if node.type == "namespace_definition":
        return node.child_by_field_name("body")
    if node.type in {"class_specifier", "struct_specifier", "union_specifier"}:
        return node.child_by_field_name("body")
    if node.type == "enum_specifier":
        return node.child_by_field_name("body")
    return None


def walk(
    node: Node,
    source: bytes,
    in_namespace: int,
    in_function_body: int,
    issues: list[tuple[int, str, str]],
) -> None:
    # Function body (or lambda body) shields its contents from doc checks.
    if node.type == "compound_statement":
        for child in node.named_children:
            walk(child, source, in_namespace, in_function_body + 1, issues)
        return

    if node.type == "namespace_definition":
        body = container_body(node)
        if body is not None:
            for child in body.named_children:
                walk(child, source, in_namespace + 1, in_function_body, issues)
        return

    if (
        node.type in DECL_KINDS
        and in_namespace > 0
        and in_function_body == 0
        and not is_skippable(node, source)
        and not is_inner_decl_of_template(node)
        and not is_inner_record_of_field_declaration(node)
    ):
        kind = preceding_doc_kind(node, source)
        if kind != "triple":
            issues.append(
                (
                    node.start_point[0] + 1,
                    "wrong-style (use `///`)" if kind == "double" else "missing",
                    decl_text(node, source),
                )
            )

    body = container_body(node)
    if body is not None:
        for child in body.named_children:
            walk(child, source, in_namespace, in_function_body, issues)
        return

    for child in node.named_children:
        walk(child, source, in_namespace, in_function_body, issues)


def scan(path: Path) -> list[tuple[int, str, str]]:
    source = path.read_bytes()
    tree = PARSER.parse(source)
    issues: list[tuple[int, str, str]] = []
    walk(tree.root_node, source, 0, 0, issues)
    return issues


def collect_files(argv: Iterable[str]) -> list[Path]:
    args = [Path(p) for p in argv if p.endswith(".h")]
    if args:
        return args
    return sorted(INCLUDE_ROOT.rglob("*.h"))


def main(argv: list[str]) -> int:
    files = collect_files(argv[1:])
    if not files:
        return 0
    fail = 0
    for path in files:
        try:
            display = path.relative_to(REPO_ROOT)
        except ValueError:
            display = path
        for line, kind, snippet in scan(path):
            sys.stderr.write(f"{display}:{line}: {kind} doc: {snippet}\n")
            fail += 1
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
