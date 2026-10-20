#!/usr/bin/env bash
# Tests the paired ABI generics examples.
set -euo pipefail
source "$(dirname "$0")/../common.sh"
abi_test generics "does not satisfy constraint 'Value'"
