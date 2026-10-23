#!/usr/bin/env bash
# Runs the compatible consumer and checks the incompatible dependency rejection.
set -euo pipefail
cd "$(dirname "$0")"
output="."
"$output/revised"
rm -f "$output/rejected"

status=0
bash "./build-incompatible.sh" \
  2>&1 | tee "$output/rejected.log" || status=$?

# Require the intended error, not a crash or an unrelated build failure.
if (( status == 0 || status >= 128 )) ||
    ! grep -Fq "Library ABI mismatch" "$output/rejected.log" ||
    [[ -e "$output/rejected" ]]; then
  printf 'FAIL identity: expected dependency rejection\n' >&2
  exit 1
fi
printf 'PASS identity: revised consumer ran; incompatible consumer rejected\n'
