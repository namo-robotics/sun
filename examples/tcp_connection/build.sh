set -euo pipefail
DIR=$(dirname "$0")
cd "$DIR"

export SUN_PATH="${DIR}:${DIR}/../../build"
sun --compile --debug -o listener listener.sun
sun --compile --debug -o talker talker.sun