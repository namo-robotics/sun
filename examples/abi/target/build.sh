#!/usr/bin/env bash
# Builds the paired ABI target examples.
set -euo pipefail
source "$(dirname "$0")/../common.sh"
mkdir -p "$ABI_ROOT/tmp/abi-examples/target"
"$ABI_SUN" --compile "$ABI_EXAMPLES/target/sun-config.json"

# Foreign bitcode needs no cross linker or sysroot.
case "$(uname -m)" in
  x86_64|amd64) foreign=aarch64-linux-gnu ;;
  *) foreign=x86_64-linux-gnu ;;
esac
"$ABI_SUN" --emit-moon --target "$foreign" \
  -o "$ABI_ROOT/tmp/abi-examples/target/incompatible.moon" \
  "$ABI_EXAMPLES/target/library.sun"
