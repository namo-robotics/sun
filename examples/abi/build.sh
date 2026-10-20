#!/usr/bin/env bash
# Builds all ABI examples for the repository's example CI job.
set -euo pipefail
for example in target identity generics; do
  bash "$(dirname "$0")/$example/build.sh"
done
