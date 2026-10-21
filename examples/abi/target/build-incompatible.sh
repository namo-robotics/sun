#!/usr/bin/env bash
# Compiles the incompatible consumer and preserves the expected compiler error.
# Run build.sh first to create the libraries it imports.
set -euo pipefail
source "$(dirname "$0")/../common.sh"
output="$ABI_ROOT/tmp/abi-examples/target"
mkdir -p "$output"
rm -f "$output/rejected"
exec "$ABI_SUN" --compile -o "$output/rejected" \
  "$ABI_EXAMPLES/target/incompatible.sun"
