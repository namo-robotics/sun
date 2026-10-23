#!/usr/bin/env bash
# Removes the generated libraries, executables, and test log for this example.
set -euo pipefail
source "$(dirname "$0")/../common.sh"
output="$ABI_EXAMPLES/identity"
rm -f -- "$output/shapes_v1.moon" "$output/shapes_v1.1.moon" \
  "$output/shapes_v2.moon" "$output/shapes_user.moon" \
  "$output/revised" "$output/rejected" \
  "$output/rejected.log"
