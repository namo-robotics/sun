#!/usr/bin/env bash
# Builds the compatible dependency revision example.
set -euo pipefail
cd "$(dirname "$0")"
output=$(sun describe --field build_root)
mkdir -p "$output"
# Compile snapshots of one library with the same ID and distinct output paths.
for revision in v1 v1.1 v2; do
  sun --emit-moon --id sun.examples/shapes \
    -o "$output/shapes_$revision.moon" "shapes_$revision.sun"
done
sun --compile sun-config.json
