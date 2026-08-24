set -euo pipefail
DIR=$(dirname "$0")
cd "$DIR"

sun --compile --debug -o listener listener.sun
sun --compile --debug -o talker talker.sun