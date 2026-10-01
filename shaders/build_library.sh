#!/usr/bin/env bash
# build_library.sh OUTPUT GAME [EXTRA FILES...]
# Builds superman_returns_shaders.srsp from the player's own game files:
#   1. builds the scanner, the translator (XenosRecomp with -DNFSMW_RECOMP) and the packer;
#   2. extracts and validates every shader container (GAME/*.AST, GAME/default.xex, extras);
#   3. translates to HLSL, compiles with DXC and validates with spirv-val;
#   4. packs and checks that every container is found in the library.
# Adapted from nfsmw-nx's nfsmw_regenerar_biblioteca_pcf.sh. The NFSMW-specific HLSL
# rewrites (shadow map PCF, minimum shadow, radial blur of p_000139) are not applied:
# they target NFSMW's samplers and shader numbering.
# OUTPUT must be a new folder. Generated files contain game data: never commit them.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")" && pwd)
SDK=${SDK:-$ROOT/../sdk}
DXC=${DXC:-dxc}
VAL=${SPIRV_VAL:-spirv-val}
CXX=${CXX:-g++}
OUT=${1:?missing output folder}
GAME=${2:?missing game folder}
shift 2

[ -e "$OUT" ] && { echo "Output folder already exists"; exit 1; }
mkdir -p "$OUT/bin" "$OUT/hlsl" "$OUT/spirv"
INC=(-I"$ROOT" -I"$ROOT/XenosRecomp" -I"$SDK/thirdparty/fmt/include" -I"$SDK/thirdparty/xxHash")
$CXX -std=c++23 -O2 "${INC[@]}" "$ROOT/sr_find_containers.cpp" -o "$OUT/bin/sr_find_containers"
$CXX -std=c++23 -O1 "${INC[@]}" -include "$ROOT/pch_min.h" -DFMT_HEADER_ONLY -DXXH_INLINE_ALL -DNFSMW_RECOMP \
  "$ROOT/nfsmw_hlsl.cpp" "$ROOT/XenosRecomp/shader_recompiler.cpp" -o "$OUT/bin/sr_hlsl" \
  > "$OUT/compile.log" 2>&1 || { echo "Translator build failed; see compile.log"; exit 1; }
$CXX -std=c++23 -O2 "${INC[@]}" "$ROOT/sr_pack.cpp" "$ROOT/../app/src/sr_shader_library.cpp" \
  -o "$OUT/bin/sr_pack"

"$OUT/bin/sr_find_containers" "$GAME" "$OUT/containers" "$@" > "$OUT/scan.log"
tail -1 "$OUT/scan.log"
"$OUT/bin/sr_hlsl" "$OUT/containers" "$OUT/hlsl" "$ROOT/XenosRecomp/shader_common.h" > "$OUT/translate.log" \
  || { tail -5 "$OUT/translate.log"; echo "Some shaders were not translated; see translate.log"; exit 1; }
tail -1 "$OUT/translate.log"

: > "$OUT/dxc.log"
: > "$OUT/spirv-val.log"
n=0
failed=0
for f in "$OUT"/hlsl/*.hlsl; do
  base=$(basename "$f" .hlsl)
  if [[ "$base" == p_* ]]; then stage=ps_6_6; extra=(); else stage=vs_6_6; extra=(-fvk-invert-y); fi
  "$DXC" -spirv -T "$stage" -E main -HV 2021 -fspv-target-env=vulkan1.2 -fvk-use-dx-layout \
    -Werror=parameter-usage "${extra[@]}" -Fo "$OUT/spirv/$base.spv" "$f" \
    2>> "$OUT/dxc.log" || { echo "DXC rejected $base"; failed=$((failed + 1)); continue; }
  "$VAL" --target-env vulkan1.2 --scalar-block-layout "$OUT/spirv/$base.spv" \
    2>> "$OUT/spirv-val.log" || { echo "Invalid SPIR-V: $base"; exit 1; }
  n=$((n + 1))
done
expected=$(ls "$OUT"/containers/*.bin | wc -l)
echo "spirv=$n expected=$expected dxc_failed=$failed"
[ "$n" -eq "$expected" ] || { echo "Output count does not match the input"; exit 1; }

"$OUT/bin/sr_pack" "$OUT/containers" "$OUT/spirv" "$OUT/superman_returns_shaders.srsp" | tee "$OUT/pack.log"
sha256sum "$OUT/superman_returns_shaders.srsp" | tee "$OUT/superman_returns_shaders.srsp.sha256"
echo "library complete"
