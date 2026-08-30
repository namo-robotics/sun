#!/bin/bash
set -euo pipefail

# Build the macOS release of the Sun compiler — the deb's counterpart. The
# payload (bin/sun, bin/sun-lsp, lib/sun/{stdlib,tls}.moon, share/sun/stdlib)
# is packed as a plain .tar.gz, which is what the Homebrew formula unpacks;
# the compiler's bundle search finds those bundles beside its own binary
# wherever Homebrew puts them. The binary links Homebrew's LLVM the way the
# deb links apt's libllvm20, so `brew install llvm@20` is its one runtime
# dependency. The tls bundle carries its own static OpenSSL, built by
# scripts/build-openssl-macos.sh; the bundle is also published on its own as
# dist/tls-<triple>.moon.
#
# Usage: ./scripts/build-macos.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_ROOT"

log() { echo "[build-macos] $1"; }

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "[build-macos] this script builds the macOS release and must run on a Mac" >&2
    exit 1
fi

LLVM_PREFIX=$(brew --prefix llvm@20 2>/dev/null || brew --prefix llvm 2>/dev/null)
if [[ -z "$LLVM_PREFIX" ]]; then
    echo "[build-macos] Homebrew LLVM not found: brew install llvm@20" >&2
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

# ---- Static OpenSSL for tls.moon -------------------------------------------
# The release ships tls.moon, which embeds these archives. The script exits
# fast when they are already built (a restored CI cache).
if [[ ! -f third_party/openssl/arm64-apple-darwin/libssl.a ||
      ! -f third_party/openssl/arm64-apple-darwin/libcrypto.a ]]; then
    ./scripts/build-openssl-macos.sh
fi

# ---- Release build ---------------------------------------------------------
BUILD_DIR=build-macos
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
# Fails loudly if the bundle was not built — a release without TLS support
# must never ship silently.
install -m 644 "$BUILD_DIR/tls.moon" "$STAGE/lib/sun/tls.moon"
install -m 644 stdlib/*.sun "$STAGE/share/sun/stdlib/"

# ---- Pack ------------------------------------------------------------------
mkdir -p dist
TARBALL="dist/sun-$VERSION-$ARCH-apple-darwin.tar.gz"
tar -czf "$TARBALL" -C "$STAGE" bin lib share
log "built $TARBALL"

# The tls bundle on its own, for dropping next to an existing compiler —
# named like the deb job's cross bundles (stdlib-arm64-apple-darwin.moon).
cp "$BUILD_DIR/tls.moon" "dist/tls-$ARCH-apple-darwin.moon"
log "built dist/tls-$ARCH-apple-darwin.moon"
