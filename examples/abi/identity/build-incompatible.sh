#!/usr/bin/env bash
# Compiles the v2 consumer. Run build.sh first to build its dependencies.
set -euo pipefail
cd "$(dirname "$0")"
sun --compile -o rejected main_v2.sun
