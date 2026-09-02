set -euo pipefail
DIR=$(dirname "$0")
cd "$DIR"

sun --compile --debug -o receiver receiver.sun
sun --compile --debug -o sender sender.sun
