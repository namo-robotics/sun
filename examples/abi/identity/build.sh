#!/usr/bin/env bash
# Builds the paired ABI identity examples.
set -euo pipefail
source "$(dirname "$0")/../common.sh"
cd "$ABI_EXAMPLES/identity"
"$ABI_SUN" --compile sun-config.json
