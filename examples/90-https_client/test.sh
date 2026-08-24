#!/usr/bin/env bash
# Fetches https://example.com and checks the status line. Needs network
# access; skips (exit 0) when there is none. Exit 0 = pass.
set -euo pipefail
DIR=$(dirname "$0")
cd "$DIR"

if ! curl -fsS --max-time 10 -o /dev/null https://example.com/ 2>/dev/null; then
  echo "SKIP: no network access"
  exit 0
fi

./build.sh > /dev/null

out=$(./client)
echo "$out"

grep -q "status: 200" <<< "$out" || { echo "FAIL: expected status 200"; exit 1; }
grep -qE "body bytes: [0-9]{3,}" <<< "$out" || { echo "FAIL: empty body"; exit 1; }

echo "PASS"
