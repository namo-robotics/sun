#!/usr/bin/env bash
# Prepare the selected target's OpenSSL archives, then compile TLS with Sun's
# inherited build context. Reuse existing archives without accessing the network.
set -euo pipefail

: "${SUN_EXECUTABLE:?Run this script through sun build}"
: "${SUN_TARGET:?Missing configured target}"

case "$SUN_TARGET" in
    x86_64-*linux*) archive_target=x86_64-linux-musl ;;
    aarch64-*linux*|arm64-*linux*) archive_target=aarch64-linux-musl ;;
    arm64-*darwin*|aarch64-*darwin*) archive_target=arm64-apple-darwin ;;
    *) echo "Unsupported TLS build target: $SUN_TARGET" >&2; exit 1 ;;
esac

archive_dir="third_party/openssl/$archive_target"
if [[ ! -s "$archive_dir/libssl.a" || ! -s "$archive_dir/libcrypto.a" ]]; then
    case "$archive_target" in
        *-linux-musl)
            bash scripts/fetch-openssl.sh --arch "${archive_target%%-*}"
            ;;
        arm64-apple-darwin)
            bash scripts/build-openssl-macos.sh
            ;;
    esac
fi

exec "$SUN_EXECUTABLE" build-compile
