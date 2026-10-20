#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
if [[ "$#" -gt 0 ]]; then
  compilers=("$@")
else
  compilers=(g++ clang++)
fi
for cc in "${compilers[@]}"; do
  if ! command -v "$cc" >/dev/null 2>&1; then
    echo "Missing compiler: $cc" >&2
    exit 2
  fi
  "$cc" -std=c++20 -Wall -Wextra -Werror -pthread \
    -I"$root/native/MediaRuntime/src" \
    "$root/tests/imported-core-contracts.cpp" -o "$work/core-contract"
  "$work/core-contract"
done
