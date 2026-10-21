#!/usr/bin/env bash
# Locates Sun and output directories for the example scripts.
set -euo pipefail

ABI_EXAMPLES=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ABI_ROOT=$(cd "$ABI_EXAMPLES/../.." && pwd)
ABI_SUN=${SUN_BIN:-$(command -v sun || printf '%s' "$ABI_ROOT/build/sun")}
