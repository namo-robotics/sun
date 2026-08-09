#!/usr/bin/env bash
# Starts the server, checks the 200 and 404 paths, and shuts down.
# Exit 0 = pass.
set -euo pipefail
DIR=$(dirname "$0")
cd "$DIR"

out=$(mktemp)
cleanup() { kill "$SPID" 2>/dev/null || true; wait "$SPID" 2>/dev/null || true; rm -f "$out"; }
trap cleanup EXIT

./server > "$out" 2>&1 &
SPID=$!

# Wait for the server to bind before connecting.
for _ in $(seq 1 50); do
  grep -q "Serving" "$out" && break
  sleep 0.1
done

body=$(curl -s --max-time 5 http://127.0.0.1:8080/)
echo "$body" | grep -q "Hello from Sun!"

status=$(curl -s -o /dev/null -w "%{http_code}" --max-time 5 http://127.0.0.1:8080/missing)
[ "$status" = "404" ]

echo "PASS"
