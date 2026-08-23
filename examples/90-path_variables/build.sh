set -euo pipefail
DIR=$(dirname "$0")
cd "$DIR"

export SUN_PATH="${DIR}:${DIR}/../../build"
mkdir -p libs
sun --emit-moon -o libs/mathlib.moon mathlib/entry.sun
sun --path-var LIBS=libs --compile -o main main.sun
