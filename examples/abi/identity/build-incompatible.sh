#!/usr/bin/env bash
# Compiles the v2 consumer. Run build.sh first to build its dependencies.
set -euo pipefail
cd "$(dirname "$0")"
output=$(sun describe --field build_root)
sun --compile --lib-path "$output" -o "$output/rejected" main_v2.sun
