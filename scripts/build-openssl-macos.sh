#!/bin/bash
set -euo pipefail

# Build the static OpenSSL archives that tls.moon carries on macOS.
#
# tls.moon embeds libssl.a and libcrypto.a, so a program using TLS links
# without -lssl/-lcrypto and runs without OpenSSL installed. Alpine has no
# Darwin packages (the Linux archives come from scripts/fetch-openssl.sh),
# so on macOS we compile a pinned OpenSSL release ourselves, with the same
# self-containment rule: nothing outside the archives themselves.
#
# The build sets --openssldir=/etc/ssl so certificate verification
# (SSL_CTX_set_default_verify_paths) finds the CA bundle stock macOS ships
# at /etc/ssl/cert.pem — no runtime setup needed. SSL_CERT_FILE still
# overrides it.
#
# Output: third_party/openssl/arm64-apple-darwin/lib{ssl,crypto}.a
#
# Usage: ./scripts/build-openssl-macos.sh

OPENSSL_VERSION=3.5.8
OPENSSL_SHA256=a8f84a39918ec6415ce765d9b429d313ba97b8143169c172e734b9514464f5b2

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
OUT_DIR="$PROJECT_ROOT/third_party/openssl/arm64-apple-darwin"
STAMP="$OUT_DIR/.built-openssl-$OPENSSL_VERSION"
WORK_DIR="$PROJECT_ROOT/tmp/openssl-src"

log() { echo "[build-openssl] $1"; }

if [[ "$(uname -s)" != "Darwin" || "$(uname -m)" != "arm64" ]]; then
    echo "[build-openssl] this script builds arm64 Darwin archives and must run on an Apple Silicon Mac" >&2
    exit 1
fi

# Idempotent so a restored CI cache exits fast; the stamp names the version,
# so bumping the pin above rebuilds.
if [[ -f "$STAMP" && -f "$OUT_DIR/libssl.a" && -f "$OUT_DIR/libcrypto.a" ]]; then
    log "OpenSSL $OPENSSL_VERSION already present in $OUT_DIR"
    exit 0
fi

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"
cd "$WORK_DIR"

log "downloading openssl-$OPENSSL_VERSION"
curl -fsSL --max-time 600 -o openssl.tar.gz \
    "https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION/openssl-$OPENSSL_VERSION.tar.gz"
echo "$OPENSSL_SHA256  openssl.tar.gz" | shasum -a 256 -c -

tar xzf openssl.tar.gz
cd "openssl-$OPENSSL_VERSION"

# no-shared: static archives only. no-module/no-dso/no-engine/no-legacy: no
# providers loaded at run time from a vendored static build. no-comp/no-zlib/
# no-zstd/no-brotli: no references to libraries we do not ship. 11.0 is the
# first macOS that runs on arm64.
export MACOSX_DEPLOYMENT_TARGET=11.0
log "configuring for darwin64-arm64-cc"
./Configure darwin64-arm64-cc \
    no-shared no-tests no-comp no-zlib no-zstd no-brotli \
    no-module no-dso no-engine no-legacy \
    --prefix=/usr/local --openssldir=/etc/ssl
make -j"$(sysctl -n hw.ncpu)" build_libs

# A reference to a library we do not ship would make every link fail with
# undefined symbols, so check before installing rather than at link time.
for sym in ZSTD_ BrotliDec jent_; do
    if nm -u libcrypto.a 2>/dev/null | grep -q "$sym"; then
        echo "error: libcrypto.a references $sym; it is not self-contained" >&2
        exit 1
    fi
done

# Debug info is no use inside a vendored dependency and would roughly double
# tls.moon. Darwin's strip drops the archive's table of contents along with
# it, so ranlib must rebuild the index or every link fails.
strip -S libssl.a libcrypto.a
ranlib libssl.a libcrypto.a

mkdir -p "$OUT_DIR"
cp libssl.a libcrypto.a "$OUT_DIR/"
touch "$STAMP"

log "wrote $OUT_DIR/{libssl.a,libcrypto.a}"
ls -lh "$OUT_DIR"
