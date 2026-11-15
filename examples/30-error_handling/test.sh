#!/usr/bin/env bash
# Checks that main catches A after Bar handles B during cleanup.
set -euo pipefail
DIR=$(dirname "$0")
cd "$DIR"

status=0
out=$(./main 2>&1) || status=$?
if [ "$status" -ne 1 ] || [ -n "$out" ]; then
  printf 'Expected exit status 1 and no output; got status %s.\n%s\n' "$status" "$out" >&2
  exit 1
fi
