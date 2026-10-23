#!/usr/bin/env bash
# Compiles the v2 consumer. Run build.sh first to build its dependencies.
set -euo pipefail
source "$(dirname "$0")/../common.sh"
cd "$ABI_EXAMPLES/identity"
"$ABI_SUN" --compile -o rejected main_v2.sun
