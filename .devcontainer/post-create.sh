#!/usr/bin/env bash
# Make the persistent compiler cache writable by the development user.
# Container startup preserves the compiler and project dependencies in the image.
set -euo pipefail

source /etc/os-release
if [[ "$ID" != ubuntu || "$VERSION_ID" != 26.04 ]]; then
    echo 'The Sun devcontainer template requires Ubuntu 26.04.' >&2
    exit 1
fi

cache_dir="${CCACHE_DIR:-$HOME/.cache/ccache}"
sudo chown -R "$(id -u):$(id -g)" "$cache_dir"
sudo install -d -o "$(id -u)" -g "$(id -g)" "$HOME/.config/dconf"
