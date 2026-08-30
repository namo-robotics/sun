#!/bin/bash
set -euo pipefail
DIR=$(dirname "$0")
cd "$DIR"

# tls.moon embeds static OpenSSL, which the repository does not carry, so a
# build without the platform's OpenSSL script has no bundle to link against.
if [ ! -f ../../build/tls.moon ]; then
  echo "SKIP: ../../build/tls.moon not built — run ./scripts/fetch-openssl.sh (Linux) or ./scripts/build-openssl-macos.sh (macOS), then ./build.sh"
  exit 0
fi

# No -lssl/-lcrypto: tls.moon carries its own static OpenSSL.
sun --compile -o client client.sun
