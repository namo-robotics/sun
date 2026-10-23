#!/usr/bin/env bash
# Builds the compatible ABI identity example.
set -euo pipefail
cd "$(dirname "$0")"
sun --compile sun-config.json
