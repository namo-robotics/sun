#!/bin/bash
set -euo pipefail

# Format the C++ sources. Only src/, include/ and tests/ — clang-format
# mangles the root files, which are not C++.

cd "$(dirname "${BASH_SOURCE[0]}")/.."

find src include tests \( -name '*.cpp' -o -name '*.h' \) -print0 |
  xargs -0 clang-format-20 -i
