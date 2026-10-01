#!/usr/bin/env bash
# Host test of the shader pack identification and vertex input of the Vulkan
# backend (sdk/src/graphics/vulkan/pack_shaders.cpp). Needs g++ 13+ only.
#   bash shaders/test_pack_identify.sh                       synthetic, no game data
#   bash shaders/test_pack_identify.sh out/<build>/containers also every container
set -euo pipefail
ROOT=$(cd "$(dirname "$0")" && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
${CXX:-g++} -std=c++20 -O2 -I"$ROOT/test_stubs" -I"$ROOT/../sdk/include" \
  -I"$ROOT/../sdk/thirdparty/xxHash" -I"$ROOT/../sdk/thirdparty/fmt/include" \
  -I"$ROOT/../sdk/thirdparty/vulkan-headers/include" \
  "$ROOT/test_pack_identify.cpp" "$ROOT/../sdk/src/graphics/vulkan/pack_shaders.cpp" \
  -o "$OUT/test_pack_identify"
"$OUT/test_pack_identify" "$@"
