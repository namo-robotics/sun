#!/bin/bash

set -euo pipefail
DIR=$(dirname "$0")
cd "$DIR"

SKIP_STDLIB=OFF
RECONFIGURE=false
for arg in "$@"; do
    case $arg in
        --skip-stdlib)
            SKIP_STDLIB=ON
            shift
            ;;
        --reconfigure|-r)
            RECONFIGURE=true
            shift
            ;;
    esac
done

mkdir -p build
cd build

# Check if SKIP_STDLIB setting changed from cached value
CACHED_SKIP_STDLIB=""
if [[ -f CMakeCache.txt ]]; then
    CACHED_SKIP_STDLIB=$(grep "^SKIP_STDLIB:" CMakeCache.txt 2>/dev/null | cut -d= -f2 || echo "")
fi

# Reconfigure if: no cache, --reconfigure flag, or SKIP_STDLIB changed
if [[ ! -f CMakeCache.txt ]] || [[ "$RECONFIGURE" == "true" ]] || [[ "$CACHED_SKIP_STDLIB" != "$SKIP_STDLIB" ]]; then
    cmake -DCMAKE_BUILD_TYPE=Debug \
          -DCMAKE_CXX_FLAGS="-g3 -O0 -fno-omit-frame-pointer" \
          -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
          -DCMAKE_C_COMPILER_LAUNCHER=ccache \
          -DSKIP_STDLIB=$SKIP_STDLIB \
          ..
fi

make -j$(nproc)

# Add sun to PATH by creating symlink
sudo ln -sf "$(pwd)/sun" /usr/local/bin/sun
echo "sun installed to /usr/local/bin/sun"