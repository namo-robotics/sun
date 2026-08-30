#!/bin/bash
set -euo pipefail

# Fetch the static OpenSSL archives that tls.moon carries.
#
# tls.moon embeds libssl.a and libcrypto.a, so a program using TLS links
# without -lssl/-lcrypto and runs without OpenSSL installed. That needs
# archives which are musl-built (Sun links statically through the musl
# toolchain by default) and self-contained (no outside zstd, brotli or
# jitterentropy). Alpine's openssl-libs-static is both, so we take it from
# there rather than compiling OpenSSL ourselves. The macOS counterpart is
# scripts/build-openssl-macos.sh.
#
# Output: third_party/openssl/x86_64-linux-musl/lib{ssl,crypto}.a
#
# Usage: ./scripts/fetch-openssl.sh [--alpine-release v3.21]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

ALPINE_RELEASE="${SUN_ALPINE_RELEASE:-v3.21}"

while [[ $# -gt 0 ]]; do
    case $1 in
        --alpine-release) ALPINE_RELEASE="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: $0 [--alpine-release v3.21]"
            exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

OUT_DIR="$PROJECT_ROOT/third_party/openssl/x86_64-linux-musl"
WORK_DIR="$PROJECT_ROOT/tmp/openssl-alpine"
MIRROR="https://dl-cdn.alpinelinux.org/alpine/$ALPINE_RELEASE/main/x86_64"

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"
cd "$WORK_DIR"

PKG=$(curl -fsSL --max-time 60 "$MIRROR/" \
      | grep -oE 'openssl-libs-static-[0-9][^"]*\.apk' | head -1)
if [ -z "$PKG" ]; then
    echo "error: no openssl-libs-static package at $MIRROR" >&2
    exit 1
fi

echo "[fetch-openssl] downloading $PKG (Alpine $ALPINE_RELEASE)"
curl -fsSL --max-time 300 -o pkg.apk "$MIRROR/$PKG"
echo "[fetch-openssl] sha256: $(sha256sum pkg.apk | cut -d' ' -f1)"

# .apk files are gzipped tarballs with extra metadata headers tar warns about
tar xzf pkg.apk 2>/dev/null || true

if [ ! -f usr/lib/libssl.a ] || [ ! -f usr/lib/libcrypto.a ]; then
    echo "error: package did not contain libssl.a and libcrypto.a" >&2
    exit 1
fi

# A reference to a library we do not ship would make every link fail with
# undefined symbols, so check before installing rather than at link time.
for sym in ZSTD_ BrotliDec jent_; do
    if nm --undefined-only usr/lib/libcrypto.a 2>/dev/null | grep -q "$sym"; then
        echo "error: libcrypto.a references $sym; it is not self-contained" >&2
        exit 1
    fi
done

# Alpine ships these unstripped, which would roughly double tls.moon. Debug
# info is no use inside a vendored dependency; the symbol table linking needs
# is kept.
strip --strip-debug usr/lib/libssl.a usr/lib/libcrypto.a

mkdir -p "$OUT_DIR"
cp usr/lib/libssl.a usr/lib/libcrypto.a "$OUT_DIR/"

echo "[fetch-openssl] wrote $OUT_DIR/{libssl.a,libcrypto.a}"
ls -lh "$OUT_DIR"
