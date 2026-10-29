#!/usr/bin/env bash
# Install Sun build dependencies from APT and static musl toolchains from musl.cc.
set -euo pipefail

APT_OPTIONS=()
DRY_RUN=false
for arg in "$@"; do
    case "$arg" in
        --yes|-y) APT_OPTIONS=(-y) ;;
        --dry-run) DRY_RUN=true ;;
        --help|-h)
            echo "Usage: $0 [--yes] [--dry-run]"
            echo "Install Ubuntu/Debian build dependencies and x86_64/AArch64 musl toolchains."
            echo "The musl.cc toolchains require an x86_64 Linux host. Installed under /opt/cross."
            echo "Requires configured repositories providing LLVM 20 development packages."
            echo "--yes accepts APT prompts; --dry-run prints commands without changing the system."
            exit 0
            ;;
        *) echo "Unknown option: $arg" >&2; exit 2 ;;
    esac
done

if ! command -v apt-get >/dev/null 2>&1; then
    echo "This script requires Ubuntu/Debian with apt-get." >&2
    exit 1
fi

# The musl.cc archives used by the Dockerfile contain x86_64 host executables.
if [[ "$(uname -s)" != Linux || "$(uname -m)" != x86_64 ]]; then
    echo "The bundled musl toolchains require an x86_64 Linux host." >&2
    exit 1
fi

# Unversioned Clang and LLD serve build.sh; LLVM and libclang must match.
PACKAGES=(
    build-essential cmake ninja-build git python3 ccache
    clang lld clang-20 lld-20 llvm-20 llvm-20-dev llvm-20-tools libclang-20-dev
    libarchive-dev libzstd-dev zlib1g-dev libxml2-dev libedit-dev libncurses-dev
    libcurl4-openssl-dev libffi-dev libgtest-dev
    openssl ca-certificates curl tar gzip
)

PRIVILEGE=()
if [[ "$EUID" -ne 0 ]]; then
    PRIVILEGE=(sudo)
    if [[ "$DRY_RUN" == false ]] && ! command -v sudo >/dev/null 2>&1; then
        echo "Run this script as root or install sudo." >&2
        exit 1
    fi
fi

# Preview the same argument arrays used for installation without refreshing APT.
if [[ "$DRY_RUN" == true ]]; then
    printf '%q ' "${PRIVILEGE[@]}" apt-get update --error-on=any
    printf '\n'
    printf '%q ' "${PRIVILEGE[@]}" apt-get install "${APT_OPTIONS[@]}" --no-install-recommends "${PACKAGES[@]}"
    printf '\n'
    for target in x86_64-linux-musl aarch64-linux-musl; do
        echo "Install https://musl.cc/$target-cross.tgz under /opt/cross if missing."
        echo "Expose /opt/cross/$target-cross/bin/$target-* in /usr/local/bin."
    done
    exit 0
fi

"${PRIVILEGE[@]}" apt-get update --error-on=any
"${PRIVILEGE[@]}" apt-get install "${APT_OPTIONS[@]}" --no-install-recommends "${PACKAGES[@]}"

# Keep downloads and extraction inside the workspace, including Docker's /opt/sun.
WORKSPACE_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
mkdir -p "$WORKSPACE_ROOT/tmp"
MUSL_STAGE=$(mktemp -d "$WORKSPACE_ROOT/tmp/musl-install.XXXXXX")
trap 'rm -rf "$MUSL_STAGE"' EXIT

# Download a missing toolchain, including the musl-built C++ runtime Sun needs.
install_toolchain() {
    local target="$1"
    local toolchain="/opt/cross/$target-cross"
    if [[ -x "$toolchain/bin/$target-gcc" ]]; then
        return
    fi
    if [[ -e "$toolchain" ]]; then
        echo "Incomplete musl toolchain at $toolchain; move it aside and rerun." >&2
        exit 1
    fi

    curl --fail --location --retry 3 \
        "https://musl.cc/$target-cross.tgz" -o "$MUSL_STAGE/$target.tgz"
    tar -xzf "$MUSL_STAGE/$target.tgz" -C "$MUSL_STAGE" --no-same-owner
    if [[ ! -x "$MUSL_STAGE/$target-cross/bin/$target-gcc" ]]; then
        echo "Downloaded archive is missing $target-gcc." >&2
        exit 1
    fi
    "${PRIVILEGE[@]}" mv "$MUSL_STAGE/$target-cross" "$toolchain"
}

# Expose a prefixed tool on PATH without replacing an unrelated installation.
link_tool() {
    local executable="$1"
    local link="/usr/local/bin/$(basename "$executable")"
    if [[ ! -e "$link" && ! -L "$link" ]]; then
        "${PRIVILEGE[@]}" ln -s "$executable" "$link"
    elif [[ "$(readlink -f "$link")" != "$(readlink -f "$executable")" ]]; then
        echo "Existing tool at $link conflicts with $executable; move it aside and rerun." >&2
        exit 1
    fi
}

"${PRIVILEGE[@]}" mkdir -p /opt/cross /usr/local/bin
for target in x86_64-linux-musl aarch64-linux-musl; do
    install_toolchain "$target"
    toolchain="/opt/cross/$target-cross"
    "$toolchain/bin/$target-gcc" --version >/dev/null
    for executable in "$toolchain/bin/$target-"*; do
        if [[ -x "$executable" && ! -d "$executable" ]]; then
            link_tool "$executable"
        fi
    done
done

echo "Build dependencies and static musl toolchains installed."
echo "Ensure /usr/local/bin is on PATH, then run ./build.sh --reconfigure."
