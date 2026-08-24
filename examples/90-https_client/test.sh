#!/usr/bin/env bash
# Fetches https://example.com and checks the status line. Skips (exit 0) when
# the TLS bundle has not been built or there is no network access.
# Exit 0 = pass.
set -euo pipefail
DIR=$(dirname "$0")
cd "$DIR"

if [ ! -f ../../build/tls.moon ]; then
  echo "SKIP: ../../build/tls.moon not built — run ./scripts/fetch-openssl.sh"
  exit 0
fi

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
