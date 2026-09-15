#!/usr/bin/env bash
set -euo pipefail

# Compile a bundle and record its inputs for CMake.
# Usage: scripts/build-moon.sh <sun> <entrypoint> <output.moon> [compiler flags...]

if [ "$#" -lt 3 ]; then
    echo "Usage: $0 <sun> <entrypoint> <output.moon> [compiler flags...]" >&2
    exit 1
fi

SUN_COMPILER="$1"
ENTRYPOINT="$2"
OUTPUT="$3"
shift 3

BUNDLE_DIR="$(dirname "$OUTPUT")"
mkdir -p "$BUNDLE_DIR"

exec "$SUN_COMPILER" --emit-moon \
    --lib-path "$BUNDLE_DIR" \
    --depfile "$OUTPUT.d" \
    -o "$OUTPUT" "$@" "$ENTRYPOINT"
