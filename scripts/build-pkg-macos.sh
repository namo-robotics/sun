#!/bin/bash
set -euo pipefail

# Build a macOS installer package (.pkg) for the Sun compiler — the deb's
# counterpart. Installs under /usr/local (bin/sun, bin/sun-lsp,
# lib/sun/stdlib.moon, share/sun/stdlib), which the compiler's bundle search
# already covers. The binary links Homebrew's LLVM the way the deb links
# apt's libllvm20, so `brew install llvm@20` is the package's one runtime
# dependency. tls.moon is not included yet: its OpenSSL archives are
# Linux-only for now (see the roadmap's cross-target archives item).
#
# Usage: ./scripts/build-pkg-macos.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_ROOT"

log() { echo "[build-pkg] $1"; }

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "[build-pkg] this script builds the macOS package and must run on a Mac" >&2
    exit 1
fi

LLVM_PREFIX=$(brew --prefix llvm@20 2>/dev/null || brew --prefix llvm 2>/dev/null)
if [[ -z "$LLVM_PREFIX" ]]; then
    echo "[build-pkg] Homebrew LLVM not found: brew install llvm@20" >&2
    exit 1
fi

# Same version rule as build-deb.sh: a v<N> tag names a release, anything
# else is the rolling dev build.
if git describe --tags --exact-match HEAD 2>/dev/null | grep -qE '^v[0-9]+$'; then
    VERSION=$(git describe --tags --exact-match HEAD | sed 's/^v//')
else
    VERSION="0.dev"
fi
ARCH=$(uname -m)  # arm64 on Apple Silicon
log "Building version $VERSION for $ARCH"

# ---- Release build ---------------------------------------------------------
BUILD_DIR=build-pkg
cmake -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$LLVM_PREFIX/bin/clang" \
    -DCMAKE_CXX_COMPILER="$LLVM_PREFIX/bin/clang++" \
    -DLLVM_DIR="$LLVM_PREFIX/lib/cmake/llvm" \
    -DBUILD_TESTING=OFF \
    ${CMAKE_EXTRA_ARGS:-}
cmake --build "$BUILD_DIR" -j"$(sysctl -n hw.ncpu)"

# ---- Stage the payload -----------------------------------------------------
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
install -d "$STAGE/bin" "$STAGE/lib/sun" "$STAGE/share/sun/stdlib"
install -m 755 "$BUILD_DIR/sun" "$STAGE/bin/sun"
install -m 755 "$BUILD_DIR/sun-lsp" "$STAGE/bin/sun-lsp"
install -m 644 "$BUILD_DIR/stdlib.moon" "$STAGE/lib/sun/stdlib.moon"
install -m 644 stdlib/*.sun "$STAGE/share/sun/stdlib/"

# ---- Build the installer ---------------------------------------------------
mkdir -p dist
PKG="dist/sun-$VERSION-$ARCH-apple-darwin.pkg"
pkgbuild \
    --root "$STAGE" \
    --identifier com.namo-robotics.sun \
    --version "$VERSION" \
    --install-location /usr/local \
    --ownership recommended \
    "$PKG"
log "built $PKG"
