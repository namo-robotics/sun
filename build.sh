#!/bin/bash

# Build Sun, optionally installing or removing its host binaries and bundles.

set -euo pipefail
# Match CI's ccache support for precompiled headers, preserving user overrides.
export CCACHE_SLOPPINESS="${CCACHE_SLOPPINESS:-pch_defines,time_macros}"
DIR=$(dirname "$0")
cd "$DIR"

SKIP_STDLIB=OFF
RECONFIGURE=false
INSTALL=false
UNINSTALL=false
INSTALL_PREFIX=/usr/local
BUILD_TYPE=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-stdlib) SKIP_STDLIB=ON ;;
        --reconfigure|-r) RECONFIGURE=true ;;
        --install) INSTALL=true ;;
        --uninstall) UNINSTALL=true ;;
        --build-type)
            case "${2:-}" in
                Debug|Release|RelWithDebInfo|MinSizeRel) BUILD_TYPE="$2" ;;
                *) echo "--build-type requires Debug, Release, RelWithDebInfo, or MinSizeRel." >&2; exit 2 ;;
            esac
            shift
            ;;
        --prefix)
            if [[ $# -lt 2 || "$2" != /* ]]; then
                echo "--prefix requires an absolute installation path." >&2
                exit 2
            fi
            INSTALL_PREFIX="$2"
            shift
            ;;
        --help|-h)
            echo "Usage: $0 [--reconfigure] [--skip-stdlib] [--install | --uninstall] [--prefix /path] [--build-type TYPE]"
            echo "Build type defaults to Debug, or Release with --install."
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

CC_BIN=clang-20
CXX_BIN=clang++-20
if [[ "$(uname -s)" == Darwin ]]; then
    LLVM_PREFIX=$(brew --prefix llvm@20)
    CC_BIN="$LLVM_PREFIX/bin/clang"
    CXX_BIN="$LLVM_PREFIX/bin/clang++"
    export CMAKE_PREFIX_PATH="$LLVM_PREFIX:$(brew --prefix libarchive)${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
fi
if ! command -v "$CC_BIN" >/dev/null || ! command -v "$CXX_BIN" >/dev/null; then
    echo "Build requires Clang 20 ($CC_BIN and $CXX_BIN)." >&2
    exit 1
fi

# Installed compilers should run optimized; local development keeps debug builds.
if [[ "$INSTALL" == true ]]; then
    BUILD_TYPE=${BUILD_TYPE:-Release}
fi
BUILD_TYPE=${BUILD_TYPE:-Debug}

# TLS bundles and tests require static OpenSSL archives before compilation.
if [[ "$SKIP_STDLIB" == OFF ]]; then
    case "$(uname -s)" in
        Linux)
            OPENSSL_ARCH=$(uname -m)
            OPENSSL_DIR="third_party/openssl/$OPENSSL_ARCH-linux-musl"
            if [[ ! -f "$OPENSSL_DIR/libssl.a" || ! -f "$OPENSSL_DIR/libcrypto.a" ]]; then
                bash scripts/fetch-openssl.sh --arch "$OPENSSL_ARCH"
            fi
            ;;
        Darwin)
            bash scripts/build-openssl-macos.sh
            ;;
    esac
fi

mkdir -p build
cd build

# Reconfigure incompatible builds while keeping downloaded dependencies.
if [[ -f CMakeCache.txt ]] && { ! grep -qx 'CMAKE_GENERATOR:INTERNAL=Ninja' CMakeCache.txt ||
    ! grep -qs '^set(CMAKE_C_COMPILER_VERSION "20[.]' CMakeFiles/*/CMakeCCompiler.cmake ||
    ! grep -qs '^set(CMAKE_CXX_COMPILER_VERSION "20[.]' CMakeFiles/*/CMakeCXXCompiler.cmake; }; then
    rm CMakeCache.txt
fi

# Keep preset flags when the cached build already matches the request.
if [[ "$RECONFIGURE" == true ]] ||
   ! grep -Fqsx "CMAKE_BUILD_TYPE:STRING=$BUILD_TYPE" CMakeCache.txt ||
   ! grep -Fqsx "SKIP_STDLIB:BOOL=$SKIP_STDLIB" CMakeCache.txt; then
    cmake -G Ninja \
          -DCMAKE_CXX_COMPILER="$CXX_BIN" \
          -DCMAKE_C_COMPILER="$CC_BIN" \
          -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
          -DCMAKE_CXX_FLAGS="-fno-omit-frame-pointer" \
          -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
          -DCMAKE_C_COMPILER_LAUNCHER=ccache \
          -DSKIP_STDLIB=$SKIP_STDLIB \
          ..
fi

BUILD_CORES=$(getconf _NPROCESSORS_ONLN)
BUILD_JOBS=$((BUILD_CORES / 2))
((BUILD_JOBS > 0)) || BUILD_JOBS=1
cmake --build . -j"$BUILD_JOBS"

if [[ "$INSTALL" == true ]]; then
    bash ../scripts/install-build.sh install "$INSTALL_PREFIX"
fi
