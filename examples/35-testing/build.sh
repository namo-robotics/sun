set -euo pipefail
DIR=$(dirname "$0")
cd "$DIR"

# -c emits both binaries when the program has tests: main and main_test.
sun --compile -o main main.sun
