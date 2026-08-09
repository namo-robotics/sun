#!/usr/bin/env bash
# Runs the built example and asserts expected output. Exit 0 = pass.
set -euo pipefail
DIR=$(dirname "$0")
cd "$DIR"

out=$(./main)
echo "$out"
echo "$out" | grep -q "^5$"
echo "$out" | grep -q "caught division by zero"
