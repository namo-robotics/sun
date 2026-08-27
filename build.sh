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

# A cache configured with the wrong generator or compiler (e.g. by an IDE or
# a fresh container picking defaults) poisons the build dir — wipe and redo.
if [[ -f CMakeCache.txt ]]; then
    if ! grep -q "CMAKE_GENERATOR:INTERNAL=Ninja" CMakeCache.txt || \
       ! grep -q "CMAKE_CXX_COMPILER:.*clang++" CMakeCache.txt; then
        echo "Build dir configured with wrong generator/compiler — wiping"
        find . -mindepth 1 -delete
    fi
fi

# Check if SKIP_STDLIB setting changed from cached value
CACHED_SKIP_STDLIB=""
if [[ -f CMakeCache.txt ]]; then
    CACHED_SKIP_STDLIB=$(grep "^SKIP_STDLIB:" CMakeCache.txt 2>/dev/null | cut -d= -f2 || echo "")
fi

# On a Mac, point CMake at Homebrew's LLVM (Apple ships no LLVMConfig.cmake)
EXTRA_CMAKE_ARGS=()
if [[ "$(uname -s)" == "Darwin" ]] && command -v brew >/dev/null 2>&1; then
    LLVM_PREFIX=$(brew --prefix llvm@20 2>/dev/null || brew --prefix llvm 2>/dev/null || true)
    if [[ -n "$LLVM_PREFIX" ]]; then
        EXTRA_CMAKE_ARGS+=("-DLLVM_DIR=$LLVM_PREFIX/lib/cmake/llvm")
    fi
fi

# Reconfigure if: no cache, --reconfigure flag, or SKIP_STDLIB changed
if [[ ! -f CMakeCache.txt ]] || [[ "$RECONFIGURE" == "true" ]] || [[ "$CACHED_SKIP_STDLIB" != "$SKIP_STDLIB" ]]; then
    cmake -G Ninja \
          -DCMAKE_CXX_COMPILER=clang++ \
          -DCMAKE_C_COMPILER=clang \
          -DCMAKE_BUILD_TYPE=Debug \
          -DCMAKE_CXX_FLAGS="-g3 -O0 -fno-omit-frame-pointer" \
          -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
          -DCMAKE_C_COMPILER_LAUNCHER=ccache \
          -DSKIP_STDLIB=$SKIP_STDLIB \
          ${EXTRA_CMAKE_ARGS[@]+"${EXTRA_CMAKE_ARGS[@]}"} \
          ..
fi

cmake --build . -j8

# Add sun to PATH by creating symlink
sudo ln -sf "$(pwd)/sun" /usr/local/bin/sun
echo "sun installed to /usr/local/bin/sun"