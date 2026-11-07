#!/usr/bin/env bash
# Builds the language server and packages and installs the VS Code extension.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXT_DIR="$ROOT_DIR/extensions/vscode-sun"
BUILD_DIR="$ROOT_DIR/build"
VSIX_PATH="$EXT_DIR/sun-language.vsix"

# Installs the built extension through an available VS Code command-line tool.
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

if ! command -v node >/dev/null 2>&1; then
  echo "Error: Node.js 22 or newer is required to package the extension." >&2
  exit 1
fi

if ! node -e 'process.exit(Number(process.versions.node.split(".")[0]) >= 22 ? 0 : 1)'; then
  echo "Error: Node.js 22 or newer is required to package the extension (found $(node --version))." >&2
  echo "With nvm, run: nvm install 24 && nvm use 24; then rerun this script." >&2
  exit 1
fi

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
BUILD_JOBS=$(( $(nproc) / 2 ))
if (( BUILD_JOBS < 1 )); then
  BUILD_JOBS=1
fi
cmake --build "$BUILD_DIR" -j"$BUILD_JOBS" --target sun-lsp

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
