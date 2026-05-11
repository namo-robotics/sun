#!/bin/bash

set -euo pipefail
DIR=$(dirname "$0")
cd "$DIR"

SKIP_STDLIB=OFF
for arg in "$@"; do
    case $arg in
        --skip-stdlib)
            SKIP_STDLIB=ON
            shift
            ;;
    esac
done

mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-g3 -O0 -fno-omit-frame-pointer" \
      -DSKIP_STDLIB=$SKIP_STDLIB \
      ..
make -j$(nproc)

# Add sun to PATH by creating symlink
sudo ln -sf "$(pwd)/sun" /usr/local/bin/sun
echo "sun installed to /usr/local/bin/sun"