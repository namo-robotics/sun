#!/usr/bin/env bash
# Runs the compatible example and shows the expected incompatible diagnostic.
set -euo pipefail
source "$(dirname "$0")/../common.sh"
output="$ABI_ROOT/tmp/abi-examples/target"
"$output/compatible"

# Capture the standalone build script’s output for CI checks.
status=0
bash "$ABI_EXAMPLES/target/build-incompatible.sh" 2>&1 | tee "$output/rejected.log" || status=$?

# CI requires the intended error, not a crash or an unrelated build failure.
if (( status == 0 || status >= 128 )) ||
    ! grep -Fq "incompatible target profile" "$output/rejected.log" ||
    [[ -e "$output/rejected" ]]; then
  printf 'FAIL target: expected compilation rejection\n' >&2
  exit 1
fi
printf 'PASS target: compatible program ran; incompatible case rejected\n'
