#!/usr/bin/env bash
# Print the display name for bundles distributed from this source checkout.
set -euo pipefail
source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ "${SUN_VERSION_NAME+x}" ]]; then
    printf '%s\n' "$SUN_VERSION_NAME"
    exit 0
fi
name=dev
if dirty=$(git -C "$source_root" status --porcelain --untracked-files=no -- . ':!debian/changelog' 2>/dev/null) && [[ -z "$dirty" ]]; then
    tags=$(git -C "$source_root" tag --points-at HEAD 2>/dev/null || true)
    tag=$(printf '%s\n' "$tags" | LC_ALL=C sort | sed -n '/^v\{0,1\}[0-9]/{p;q;}')
    if [[ -n "$tag" ]]; then
        name="$tag"
    fi
fi
printf '%s\n' "$name"
