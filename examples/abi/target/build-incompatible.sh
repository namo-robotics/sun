#!/usr/bin/env bash
# Compiles the incompatible consumer and preserves the expected compiler error.
# Run build.sh first to create the libraries it imports.
set -euo pipefail
cd "$(dirname "$0")"
output="../../../tmp/abi-examples/target"
mkdir -p "$output"
rm -f "$output/rejected"
exec sun --compile -o "$output/rejected" \
  "./incompatible.sun"
