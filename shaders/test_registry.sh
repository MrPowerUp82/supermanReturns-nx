#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")" && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
${CXX:-g++} -std=c++23 -O2 -pthread -I"$ROOT/../app/src" \
  -I"$ROOT/../sdk/thirdparty/xxHash" "$ROOT/test_registry.cpp" \
  "$ROOT/../app/src/sr_shader_library.cpp" "$ROOT/../app/src/sr_shader_registry.cpp" \
  -o "$OUT/test_registry"
"$OUT/test_registry" "${1:?Expected local shader library path}"
