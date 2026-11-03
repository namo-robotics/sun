#!/usr/bin/env bash
# Runs a compatible borrow and checks rejection of an incompatible revision.
set -euo pipefail
cd "$(dirname "$0")"
output=$(sun describe --field build_root)
"$output/revised"
rm -f "$output/rejected"

status=0
bash "./build-incompatible.sh" \
  2>&1 | tee "$output/rejected.log" || status=$?

# Require the intended error, not a crash or an unrelated build failure.
if (( status == 0 || status >= 128 )) ||
    ! grep -Fq "read" "$output/rejected.log" ||
    [[ -e "$output/rejected" ]]; then
  printf 'FAIL identity: expected cross-revision argument rejection\n' >&2
  exit 1
fi
printf 'PASS identity: revised consumer ran; cross-revision call rejected\n'
