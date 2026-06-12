set -euo pipefail
DIR=$(dirname "$0")
cd "$DIR"

export SUN_PATH="${DIR}:${DIR}/../../build"
sun --emit-moon -o moon3.moon moon3/entry.sun
sun --emit-moon -o moon2.moon moon2/entry.sun
sun --emit-moon -o moon1.moon moon1/entry.sun
sun --compile --debug -o main main.sun