#!/usr/bin/env bash
# Builds the paired ABI target examples.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p "../../../tmp/abi-examples/target"
sun --compile "./sun-config.json"

# Foreign bitcode needs no cross linker or sysroot.
case "$(uname -m)" in
  x86_64|amd64) foreign=aarch64-linux-gnu ;;
  *) foreign=x86_64-linux-gnu ;;
esac
sun --emit-moon --target "$foreign" \
  -o "../../../tmp/abi-examples/target/incompatible.moon" \
  "./library.sun"
