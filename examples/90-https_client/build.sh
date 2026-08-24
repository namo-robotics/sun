#!/bin/bash
set -euo pipefail
DIR=$(dirname "$0")
cd "$DIR"

# No -lssl/-lcrypto: tls.moon carries its own static OpenSSL.
sun --compile -o client client.sun
