#!/bin/bash
set -euo pipefail

# Format the C++ sources. Only src/, include/ and tests/ — clang-format
# mangles the root files, which are not C++.

cd "$(dirname "${BASH_SOURCE[0]}")/.."

formatter="${CLANG_FORMAT:-}"
if [[ -z "$formatter" ]]; then
  formatter="$(command -v clang-format-20 || true)"
  if [[ -z "$formatter" ]] && command -v brew >/dev/null 2>&1; then
    llvm_prefix="$(brew --prefix llvm@20 2>/dev/null || true)"
    if [[ -n "$llvm_prefix" && -x "$llvm_prefix/bin/clang-format" ]]; then
      formatter="$llvm_prefix/bin/clang-format"
    fi
  fi
  if [[ -z "$formatter" ]]; then
    formatter="$(command -v clang-format || true)"
  fi
fi

if [[ -z "$formatter" ]] || ! version="$("$formatter" --version 2>/dev/null)"; then
  echo "error: clang-format 20 not found; install clang-format-20 (Linux) or llvm@20 (Homebrew), or set CLANG_FORMAT to its executable." >&2
  exit 1
fi
if [[ ! "$version" =~ clang-format\ version\ 20\. ]]; then
  echo "error: expected clang-format 20, got: $version" >&2
  exit 1
fi

find src include tests \( -name '*.cpp' -o -name '*.h' \) -print0 |
  xargs -0 "$formatter" -i
