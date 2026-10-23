#!/usr/bin/env bash
# Tests all ABI examples for the repository's example CI job.
set -euo pipefail
for example in target identity; do
  bash "$(dirname "$0")/$example/test.sh"
done
