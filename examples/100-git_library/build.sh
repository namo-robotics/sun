#!/usr/bin/env bash
# Build the Git dependency and consumer, forwarding extra compiler options.
# Prefer the workspace compiler; otherwise use sun from PATH.
set -euo pipefail

example_dir="$(dirname -- "${BASH_SOURCE[0]}")"
compiler="$example_dir/../../build/sun"
if [[ ! -x "$compiler" ]]; then
  compiler=sun
fi

exec "$compiler" -c --no-test "$@" "$example_dir/sun-config.json"
