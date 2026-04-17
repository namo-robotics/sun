#!/bin/bash

set -euo pipefail
DIR=$(dirname "$0")
cd "$DIR"

mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-g3 -O0 -fno-omit-frame-pointer" \
      ..
make -j$(nproc)

# Add sun to PATH by creating symlink
sudo ln -sf "$(pwd)/sun" /usr/local/bin/sun
echo "sun installed to /usr/local/bin/sun"