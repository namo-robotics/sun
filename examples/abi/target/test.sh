#!/usr/bin/env bash
# Tests the paired ABI target examples.
set -euo pipefail
source "$(dirname "$0")/../common.sh"
abi_test target "incompatible target profile"
