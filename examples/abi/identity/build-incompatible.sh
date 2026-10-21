#!/usr/bin/env bash
# Compiles the v2 consumer. Run build.sh first to build its dependencies.
set -euo pipefail
source "$(dirname "$0")/../common.sh"
"$ABI_SUN" --compile -o "$ABI_ROOT/tmp/abi-examples/identity/rejected" \
  "$ABI_EXAMPLES/identity/main_incompatible.sun"
