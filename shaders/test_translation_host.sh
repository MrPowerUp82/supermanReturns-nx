#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")" && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
${CXX:-g++} -std=c++23 -O1 -I"$ROOT" -I"$ROOT/XenosRecomp" \
  -I"$ROOT/../sdk/thirdparty/fmt/include" -I"$ROOT/../sdk/thirdparty/xxHash" \
  -include "$ROOT/pch_min.h" -DFMT_HEADER_ONLY -DXXH_INLINE_ALL -DNFSMW_RECOMP \
  "$ROOT/test_cube.cpp" "$ROOT/XenosRecomp/shader_recompiler.cpp" -o "$OUT/test_translation"
"$OUT/test_translation" "$OUT/cube.hlsl"
