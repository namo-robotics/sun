#!/usr/bin/env bash
# Copy or remove Sun's known build artifacts under a host installation prefix.
set -euo pipefail

ACTION=${1:-}
PREFIX=${2:-/usr/local}
if [[ "$ACTION" != install && "$ACTION" != uninstall ]] || [[ "$PREFIX" != /* ]]; then
    echo "Usage: $0 install|uninstall /absolute/prefix" >&2
    exit 2
fi
WORKSPACE_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
BUILD_DIR="$WORKSPACE_ROOT/build"
PREFIX=${PREFIX%/}
[[ -n "$PREFIX" ]] || PREFIX=/

# Only request privilege for prefixes the invoking user cannot write.
EXISTING_PARENT="$PREFIX"
while [[ ! -e "$EXISTING_PARENT" ]]; do
    EXISTING_PARENT=$(dirname "$EXISTING_PARENT")
done
PRIVILEGE=()
if [[ "$EUID" -ne 0 && ! -w "$EXISTING_PARENT" ]]; then
    PRIVILEGE=(sudo)
fi

BIN_DIR="$PREFIX/bin"
LIB_DIR="$PREFIX/lib/sun"

# Keep target bundles in the same layout used by compiler library discovery.
TARGETS=(. x86_64-linux-gnu x86_64-linux-musl aarch64-linux-gnu
         aarch64-linux-musl arm64-apple-darwin aarch64-apple-darwin x86_64-apple-darwin)
BUNDLES=(stdlib tls cuda)

# Remove only Sun's standard files, preserving unrelated libraries and directories.
uninstall_build() {
    local target bundle
    "${PRIVILEGE[@]}" rm -f "$BIN_DIR/sun" "$BIN_DIR/sun-lsp"
    for target in "${TARGETS[@]}"; do
        for bundle in "${BUNDLES[@]}"; do
            "${PRIVILEGE[@]}" rm -f "$LIB_DIR/$target/$bundle.moon"
        done
    done
    for target in "${TARGETS[@]:1}"; do
        "${PRIVILEGE[@]}" rmdir "$LIB_DIR/$target" 2>/dev/null || true
    done
    "${PRIVILEGE[@]}" rmdir "$LIB_DIR" 2>/dev/null || true
    echo "Sun binaries and standard Moon bundles removed from $PREFIX."
}

# Check required outputs before copying binaries and available target bundles.
install_build() {
    local artifact binary target bundle source
    for artifact in sun sun-lsp stdlib.moon tls.moon; do
        if [[ ! -f "$BUILD_DIR/$artifact" ]]; then
            echo "Missing build/$artifact; build with stdlib enabled before installing." >&2
            exit 1
        fi
    done

    "${PRIVILEGE[@]}" install -d "$BIN_DIR" "$LIB_DIR"
    for binary in sun sun-lsp; do
        # Replace legacy symlinks without writing into their build-tree targets.
        if [[ -L "$BIN_DIR/$binary" ]]; then
            "${PRIVILEGE[@]}" rm "$BIN_DIR/$binary"
        fi
        "${PRIVILEGE[@]}" install -m 755 "$BUILD_DIR/$binary" "$BIN_DIR/$binary"
    done
    for target in "${TARGETS[@]}"; do
        for bundle in "${BUNDLES[@]}"; do
            source="$BUILD_DIR/$target/$bundle.moon"
            [[ -f "$source" ]] || continue
            "${PRIVILEGE[@]}" install -d "$LIB_DIR/$target"
            "${PRIVILEGE[@]}" install -m 644 "$source" "$LIB_DIR/$target/$bundle.moon"
        done
    done
    echo "Sun binaries installed in $BIN_DIR; Moon bundles installed in $LIB_DIR."
}

case "$ACTION" in
    install) install_build ;;
    uninstall) uninstall_build ;;
esac
