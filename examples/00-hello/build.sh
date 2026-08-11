set -euo pipefail
DIR=$(dirname "$0")
cd "$DIR"

export SUN_PATH="${DIR}:${DIR}/../../build"
sun --compile -o main main.sun
