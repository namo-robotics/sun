#!/usr/bin/env bash
# Starts the listener, runs the talker, and asserts the message arrived.
# Exit 0 = pass.
set -euo pipefail
DIR=$(dirname "$0")
cd "$DIR"

out=$(mktemp)
cleanup() { kill "$LPID" 2>/dev/null || true; wait "$LPID" 2>/dev/null || true; rm -f "$out"; }
trap cleanup EXIT

./listener > "$out" 2>&1 &
LPID=$!

# Wait for the listener to bind before connecting.
for _ in $(seq 1 50); do
  grep -q "Listening" "$out" && break
  sleep 0.1
done

./talker
sleep 0.3

echo "--- listener output ---"
cat "$out"
grep -q "Hello from talker!" "$out"
