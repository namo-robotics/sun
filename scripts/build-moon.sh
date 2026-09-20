#!/usr/bin/env bash
set -euo pipefail

# Compile a bundle next to the bundles it imports. Safe to run on every
# build: the compiler leaves a bundle alone when its inputs are unchanged.
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
    -o "$OUTPUT" "$@" "$ENTRYPOINT"
