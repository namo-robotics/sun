#!/usr/bin/env bash
# Starts the receiver, runs the sender, and asserts the datagram arrived.
# Exit 0 = pass.
set -euo pipefail
DIR=$(dirname "$0")
cd "$DIR"

out=$(mktemp)
cleanup() { kill "$RPID" 2>/dev/null || true; wait "$RPID" 2>/dev/null || true; rm -f "$out"; }
trap cleanup EXIT

./receiver > "$out" 2>&1 &
RPID=$!

# Wait for the receiver to bind before sending.
for _ in $(seq 1 50); do
  grep -q "Waiting" "$out" && break
  sleep 0.1
done

./sender
sleep 0.3

echo "--- receiver output ---"
cat "$out"
grep -q "Hello from sender!" "$out"
grep -q "127.0.0.1" "$out"
