#!/usr/bin/env bash
set -euo pipefail

# Build AArch64 Linux bundles using the workspace's shared configuration.
# Usage: scripts/build-cross-bundles.sh <sun> [compiler flags...]

if [ "$#" -lt 1 ]; then
    echo "Usage: $0 <sun> [compiler flags...]" >&2
    exit 1
fi

SUN_COMPILER="$1"
shift
SOURCE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

exec "$SUN_COMPILER" -c --target aarch64-linux-gnu --no-test \
    --depfile "$SOURCE_ROOT/build/aarch64-linux-gnu/sun-config.d" \
    "$@" "$SOURCE_ROOT/sun-config.json"
