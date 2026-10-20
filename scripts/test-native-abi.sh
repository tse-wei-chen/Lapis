#!/usr/bin/env bash
set -euo pipefail
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
cmake -S "$repo" -B "$tmp/build" -DCMAKE_BUILD_TYPE=Release \
  -DBDMV_BUILD_TESTS=OFF -DBDMV_WITH_FFMPEG=OFF \
  -DBDMV_ENABLE_TEST_SOURCE=ON -DBDMV_BUILD_PREVIEW=OFF
cmake --build "$tmp/build" -j 2
for compiler in g++ clang++; do
  if ! command -v "$compiler" >/dev/null; then echo "Missing compiler $compiler" >&2; exit 2; fi
  "$compiler" -std=c++20 -Wall -Wextra -Werror \
    "$repo/tests/native-abi-synthetic-smoke.cpp" \
    -I"$repo/native/MediaRuntime/include" \
    -L"$tmp/build/native/MediaRuntime" \
    -Wl,-rpath,"$tmp/build/native/MediaRuntime" \
    -lMediaRuntime -o "$tmp/smoke"
  "$tmp/smoke"
done
