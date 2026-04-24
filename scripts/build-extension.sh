#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXT_DIR="$ROOT_DIR/extensions/vscode-sun"
BUILD_DIR="$ROOT_DIR/build"
VSIX_PATH="$EXT_DIR/sun-language.vsix"

install_vsix() {
  local cli=""
  for candidate in code code-insiders codium; do
    if command -v "$candidate" >/dev/null 2>&1; then
      cli="$candidate"
      break
    fi
  done

  if [[ -z "$cli" ]]; then
    echo "==> Skipping auto-install (VS Code CLI not found: code/code-insiders/codium)"
    return 0
  fi

  echo "==> Installing VSIX via $cli"
  "$cli" --install-extension "$VSIX_PATH" --force
}

if ! command -v cmake >/dev/null 2>&1; then
  echo "Error: cmake is not installed or not on PATH." >&2
  exit 1
fi

if ! command -v npm >/dev/null 2>&1; then
  echo "Error: npm is not installed or not on PATH." >&2
  exit 1
fi

echo "==> Building sun-lsp"
cmake -B "$BUILD_DIR"
cmake --build "$BUILD_DIR" -j"$(nproc)" --target sun-lsp

echo "==> Installing extension dependencies"
npm --prefix "$EXT_DIR" install

echo "==> Compiling extension"
npm --prefix "$EXT_DIR" run compile

echo "==> Packaging VSIX"
pushd "$EXT_DIR" >/dev/null
npx -y @vscode/vsce package --allow-missing-repository --out "$(basename "$VSIX_PATH")"
popd >/dev/null

install_vsix

echo ""
echo "Done. VSIX created at: $VSIX_PATH"
echo "If needed, you can install manually via: Extensions > ... > Install from VSIX..."
