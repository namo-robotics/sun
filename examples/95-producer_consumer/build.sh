set -euo pipefail
DIR=$(dirname "$0")
cd "$DIR"

sun --compile -o main main.sun
