#!/usr/bin/env bash
# Builds the compatible dependency revision example.
set -euo pipefail
cd "$(dirname "$0")"
sun --compile sun-config.json
