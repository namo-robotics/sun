#!/bin/bash

# Build Sun, optionally installing or removing its host binaries and bundles.

set -euo pipefail
DIR=$(dirname "$0")
cd "$DIR"

SKIP_STDLIB=OFF
RECONFIGURE=false
INSTALL=false
UNINSTALL=false
INSTALL_PREFIX=/usr/local
while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-stdlib) SKIP_STDLIB=ON ;;
        --reconfigure|-r) RECONFIGURE=true ;;
        --install) INSTALL=true ;;
        --uninstall) UNINSTALL=true ;;
        --prefix)
            if [[ $# -lt 2 || "$2" != /* ]]; then
                echo "--prefix requires an absolute installation path." >&2
                exit 2
            fi
            INSTALL_PREFIX="$2"
            shift
            ;;
        --help|-h)
            echo "Usage: $0 [--reconfigure] [--skip-stdlib] [--install | --uninstall] [--prefix /path]"
            echo "Build by default. --install copies binaries and Moon bundles after building."
            echo "--uninstall removes them without building. Default prefix: /usr/local."
            exit 0
            ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

if [[ "$INSTALL" == true && ( "$UNINSTALL" == true || "$SKIP_STDLIB" == ON ) ]]; then
    echo "--install cannot be combined with --uninstall or --skip-stdlib." >&2
    exit 2
fi
if [[ "$UNINSTALL" == true ]]; then
    exec bash scripts/install-build.sh uninstall "$INSTALL_PREFIX"
fi

# TLS bundles and tests require the pinned static archives before compilation.
if [[ "$SKIP_STDLIB" == OFF && "$(uname -s)" == Darwin ]]; then
    bash scripts/build-openssl-macos.sh
fi

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

# Locate Homebrew dependencies that macOS does not expose to CMake by default.
EXTRA_CMAKE_ARGS=()
if [[ "$(uname -s)" == "Darwin" ]] && command -v brew >/dev/null 2>&1; then
    LLVM_PREFIX=$(brew --prefix llvm@20 2>/dev/null || brew --prefix llvm 2>/dev/null || true)
    if [[ -n "$LLVM_PREFIX" ]]; then
        EXTRA_CMAKE_ARGS+=("-DLLVM_DIR=$LLVM_PREFIX/lib/cmake/llvm")
    fi
    LIBARCHIVE_PREFIX=$(brew --prefix libarchive 2>/dev/null || true)
    if [[ -n "$LIBARCHIVE_PREFIX" ]]; then
        EXTRA_CMAKE_ARGS+=("-DLibArchive_ROOT=$LIBARCHIVE_PREFIX")
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

BUILD_CORES=$(getconf _NPROCESSORS_ONLN)
BUILD_JOBS=$((BUILD_CORES / 2))
((BUILD_JOBS > 0)) || BUILD_JOBS=1
cmake --build . -j"$BUILD_JOBS"

if [[ "$INSTALL" == true ]]; then
    bash ../scripts/install-build.sh install "$INSTALL_PREFIX"
fi
