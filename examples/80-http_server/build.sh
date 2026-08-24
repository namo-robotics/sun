#!/bin/bash
set -euo pipefail
DIR=$(dirname "$0")
cd "$DIR"

sun --compile -o server server.sun
