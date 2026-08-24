set -euo pipefail
DIR=$(dirname "$0")
cd "$DIR"

sun --compile --debug -o main main.sun