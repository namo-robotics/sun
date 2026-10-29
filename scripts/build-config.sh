#!/usr/bin/env bash
# Build distributed config products with the source checkout's release name.
set -euo pipefail
compiler="$1"
shift
version_name="$(bash "$(dirname "${BASH_SOURCE[0]}")/release-version-name.sh")"
exec "$compiler" -c --version-name "$version_name" "$@"
