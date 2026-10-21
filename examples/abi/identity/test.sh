#!/usr/bin/env bash
# Runs the compatible consumer and checks the incompatible dependency rejection.
set -euo pipefail
source "$(dirname "$0")/../common.sh"
output="$ABI_ROOT/tmp/abi-examples/identity"
"$output/compatible"
rm -f "$output/rejected"

status=0
bash "$ABI_EXAMPLES/identity/build-incompatible.sh" \
  2>&1 | tee "$output/rejected.log" || status=$?

# Require the intended error, not a crash or an unrelated build failure.
if (( status == 0 || status >= 128 )) ||
    ! grep -Fq "moon exact dependency" "$output/rejected.log" ||
    [[ -e "$output/rejected" ]]; then
  printf 'FAIL identity: expected dependency rejection\n' >&2
  exit 1
fi
printf 'PASS identity: compatible consumer ran; incompatible consumer rejected\n'
