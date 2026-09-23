#!/usr/bin/env bash
# Run the library-backed executable and verify its compiler inspection artifacts.
set -euo pipefail
example_dir=$(dirname "$0")

"$example_dir/main"

for output in life main; do
  for artifact in ast.dot scope_tree.html ir.ll; do
    test -s "$example_dir/${output}_debug/$artifact"
  done
done
test -s "$example_dir/life_debug/moon.json"
