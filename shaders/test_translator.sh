#!/usr/bin/env bash
# Run in the shader-tools image. The fixtures contain no game data.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")" && pwd)
SDK=${SDK:-$ROOT/../sdk}
DXC=${DXC:-dxc}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
${CXX:-g++} -std=c++23 -O1 -I"$ROOT" -I"$ROOT/XenosRecomp" \
  -I"$SDK/thirdparty/fmt/include" -I"$SDK/thirdparty/xxHash" \
  -include "$ROOT/pch_min.h" -DFMT_HEADER_ONLY -DXXH_INLINE_ALL -DNFSMW_RECOMP \
  "$ROOT/test_cube.cpp" "$ROOT/XenosRecomp/shader_recompiler.cpp" -o "$OUT/test_cube"
"$OUT/test_cube" "$OUT/cube.hlsl"
"$DXC" -spirv -T ps_6_6 -E main -HV 2021 -fspv-target-env=vulkan1.2 \
  -Werror=parameter-usage -Fo "$OUT/cube.spv" "$OUT/cube.hlsl"
${SPIRV_VAL:-spirv-val} --target-env vulkan1.2 "$OUT/cube.spv"
echo "Synthetic CUBE HLSL compiled and SPIR-V validated"
