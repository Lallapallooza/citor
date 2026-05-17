#!/usr/bin/env bash
# CMAKE_CXX_CLANG_TIDY wrapper. Runs clang-tidy only on source files listed
# in CITOR_TIDY_FILES (newline- or colon-separated, repo-relative paths).
# When CITOR_TIDY_FILES is unset or empty, every TU is tidied (the
# preserved-default behaviour for local builds).
#
# cmake invokes the wrapper as:
#   <wrapper> <args> ... <source-file>
# where the source file is always the last positional arg. We pluck it,
# normalise to repo-relative, and check membership before exec'ing real
# clang-tidy.

set -euo pipefail

# The repo-relative path of the source file is the final argument cmake
# passes to clang-tidy. Earlier args are tidy flags (`-checks=`, `-p`,
# `--extra-arg-before=`, ...).
src="${!#}"

# Resolve to a repo-relative path. Source files may be absolute (when
# CMake was configured with an absolute source dir) or already relative.
repo_root="${CITOR_REPO_ROOT:-$(git rev-parse --show-toplevel 2>/dev/null || echo "")}"
if [ -n "$repo_root" ] && [[ "$src" == "$repo_root"/* ]]; then
  src="${src#"$repo_root"/}"
fi

# CITOR_TIDY_FILES semantics:
#   unset       -> tidy every TU (local-dev default).
#   __ALL__     -> tidy every TU (explicit full-tree request from CI).
#   empty       -> skip every TU (CI saw no diff worth tidying).
#   path1:path2 -> tidy only matching TUs.
if [ "${CITOR_TIDY_FILES+isset}" = "isset" ]; then
  if [ -z "$CITOR_TIDY_FILES" ]; then
    exit 0
  fi
  if [ "$CITOR_TIDY_FILES" != "__ALL__" ]; then
    match=0
    while IFS= read -r want; do
      [ -z "$want" ] && continue
      if [ "$want" = "$src" ]; then
        match=1
        break
      fi
    done < <(printf '%s\n' "$CITOR_TIDY_FILES" | tr ':' '\n')
    if [ "$match" -eq 0 ]; then
      exit 0
    fi
  fi
fi

real_tidy="${CITOR_CLANG_TIDY_BIN:-clang-tidy}"
exec "$real_tidy" "$@"
