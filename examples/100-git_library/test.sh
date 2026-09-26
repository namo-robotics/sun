#!/usr/bin/env bash
# Check the Git-library consumer's exit status and HTTP reason phrase.
set -euo pipefail
if [[ "$(uname -s)-$(uname -m)" != Linux-x86_64 ]]; then
  echo "SKIP: the Git-library example requires x86_64 Linux"
  exit 0
fi
example_dir="$(dirname -- "${BASH_SOURCE[0]}")"
output=$("$example_dir/build/git_library")
printf '%s\n' "$output"
[[ "${output##*$'\n'}" == OK ]]
