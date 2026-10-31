#!/usr/bin/env bash
# Install native build dependencies and prepare static TLS archives on macOS.
set -euo pipefail

DRY_RUN=false
for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY_RUN=true ;;
        --help|-h)
            echo "Usage: $0 [--dry-run]"
            echo "Install Homebrew build dependencies and build static OpenSSL on Apple Silicon."
            echo "Requires Homebrew and Xcode Command Line Tools."
            echo "--dry-run prints commands without installing or downloading anything."
            exit 0
            ;;
        *) echo "Unknown option: $arg" >&2; exit 2 ;;
    esac
done

if [[ "$(uname -s)" != Darwin || "$(uname -m)" != arm64 ]]; then
    echo "This script requires an Apple Silicon Mac for the static TLS build." >&2
    exit 1
fi
if ! command -v brew >/dev/null 2>&1; then
    echo "Install Homebrew from https://brew.sh and add brew to PATH, then rerun." >&2
    exit 1
fi
if ! xcode-select -p >/dev/null 2>&1; then
    echo "Run xcode-select --install, finish installation, then rerun." >&2
    exit 1
fi

PROJECT_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
PACKAGES=(cmake llvm@20 libarchive ninja ccache googletest)
if [[ "$DRY_RUN" == true ]]; then
    printf '%q ' brew install "${PACKAGES[@]}"
    printf '\n'
    printf '%q ' bash "$PROJECT_ROOT/scripts/build-openssl-macos.sh"
    printf '\n'
    exit 0
fi

brew install "${PACKAGES[@]}"
bash "$PROJECT_ROOT/scripts/build-openssl-macos.sh"
echo "Build dependencies installed. Run ./build.sh --reconfigure from the Sun repository."
