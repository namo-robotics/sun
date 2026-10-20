#!/usr/bin/env bash
# Builds the paired ABI generics examples.
set -euo pipefail
source "$(dirname "$0")/../common.sh"
mkdir -p "$ABI_ROOT/tmp/abi-examples/generics"
"$ABI_SUN" --compile "$ABI_EXAMPLES/generics/sun-config.json"
