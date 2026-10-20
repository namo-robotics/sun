#!/usr/bin/env bash
# Locates Sun and checks expected ABI failures for the example scripts.
set -euo pipefail

ABI_EXAMPLES=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ABI_ROOT=$(cd "$ABI_EXAMPLES/../.." && pwd)
ABI_SUN=${SUN_BIN:-$(command -v sun || printf '%s' "$ABI_ROOT/build/sun")}

# Runs the compatible program and requires a specific compilation rejection.
abi_test() {
  local example=$1 diagnostic=$2 status=0
  local output="$ABI_ROOT/tmp/abi-examples/$example"
  "$output/compatible"
  rm -f "$output/rejected"
  "$ABI_SUN" --compile -o "$output/rejected" \
    "$ABI_EXAMPLES/$example/incompatible.sun" \
    > "$output/rejected.log" 2>&1 || status=$?

  if (( status == 0 || status >= 128 )) ||
      ! grep -Fq "$diagnostic" "$output/rejected.log" ||
      [[ -e "$output/rejected" ]]; then
    cat "$output/rejected.log"
    printf 'FAIL %s: expected rejection containing "%s" and no executable\n' \
      "$example" "$diagnostic" >&2
    return 1
  fi
  printf 'PASS %s: compatible program ran; incompatible case rejected\n' "$example"
}
