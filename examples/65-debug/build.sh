#!/usr/bin/env bash
# Build every entrypoint in sun-config.json with compiler inspection artifacts.
set -euo pipefail
example_dir=$(dirname "$0")
sun_bin=${SUN_BIN:-sun}

"$sun_bin" -c --debug --no-test "$example_dir/sun-config.json"
