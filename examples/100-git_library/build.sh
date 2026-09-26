#!/usr/bin/env bash
# Build the Git dependency and consumer, forwarding extra compiler options.
# Prefer the workspace compiler; otherwise use sun from PATH.
set -euo pipefail

# sun_serve currently implements the Linux epoll ABI on x86_64.
if [[ "$(uname -s)-$(uname -m)" != Linux-x86_64 ]]; then
  echo "SKIP: the Git-library example requires x86_64 Linux"
  exit 0
fi

example_dir="$(dirname -- "${BASH_SOURCE[0]}")"
compiler="$example_dir/../../build/sun"
if [[ ! -x "$compiler" ]]; then
  compiler=sun
fi

native_dir="${SUN_EXAMPLE_NATIVE_LIBS:-$example_dir/../../third_party/openssl/x86_64-linux-musl}"
for library in libz.a libssl.a libcrypto.a; do
  if [[ ! -f "$native_dir/$library" ]]; then
    echo "Missing $native_dir/$library; run scripts/fetch-openssl.sh or set SUN_EXAMPLE_NATIVE_LIBS" >&2
    exit 1
  fi
done

exec "$compiler" -c --no-test --path-var "MUSL_LIB=$native_dir" "$@" "$example_dir/sun-config.json"
