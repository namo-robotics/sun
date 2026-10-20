#!/usr/bin/env bash
# Tests the paired ABI identity examples.
set -euo pipefail
source "$(dirname "$0")/../common.sh"
abi_test identity "moon exact dependency"
