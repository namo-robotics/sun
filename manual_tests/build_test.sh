#!/bin/bash

set -euo pipefail
DIR=$(dirname "$0")
cd "$DIR"

export SUN_PATH=${DIR}/..:${DIR}/../build

sun -c test.sun -o test --emit-ir