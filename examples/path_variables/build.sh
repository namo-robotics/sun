set -euo pipefail
DIR=$(dirname "$0")
cd "$DIR"

# sun-config.json supplies the library search path and the $LIBS variable
mkdir -p libs
sun --emit-moon -o libs/mathlib.moon mathlib/entry.sun
sun --compile -o main main.sun
