#!/usr/bin/env bash
set -euo pipefail

# Build distinct GNU and musl AArch64 Linux bundles using the workspace's shared configuration.
# Stage the bundles beneath the output directory for CMake and packaging.
# Usage: scripts/build-cross-bundles.sh <sun> <output-dir> [compiler flags...]

if [ "$#" -lt 2 ]; then
    echo "Usage: $0 <sun> <output-dir> [compiler flags...]" >&2
    exit 1
fi

SUN_COMPILER="$1"
OUTPUT_DIR="$2"
shift 2
SOURCE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

VERSION_NAME="$(bash "$SOURCE_ROOT/scripts/release-version-name.sh")"

for target in aarch64-linux-gnu aarch64-linux-musl; do
    "$SUN_COMPILER" -c --version-name "$VERSION_NAME" --target "$target" --no-test \
        "$@" "$SOURCE_ROOT/sun-config.json"
    build_root=$("$SUN_COMPILER" describe --config "$SOURCE_ROOT/sun-config.json" \
        --target "$target" "$@" --field build_root)
    mkdir -p "$OUTPUT_DIR/$target"
    for moon in stdlib tls; do
        cp "$build_root/$moon.moon" "$OUTPUT_DIR/$target/$moon.moon"
    done
done
