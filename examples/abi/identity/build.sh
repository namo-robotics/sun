#!/usr/bin/env bash
# Builds the paired ABI identity examples.
set -euo pipefail
source "$(dirname "$0")/../common.sh"
mkdir -p "$ABI_ROOT/tmp/abi-examples/identity"
"$ABI_SUN" --compile "$ABI_EXAMPLES/identity/sun-config.json"
