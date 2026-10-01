#!/usr/bin/env bash
# Builds out/probe/vk-probe.nro, the game-free Vulkan/NVK bring-up probe.
# Run in devkitpro/devkita64 with the project at /project (tools/build-docker.sh
# vk-probe does this), or natively with DEVKITPRO set and PROJECT pointing at the
# checkout. Needs the Mesa SDK from "tools/build-docker.sh mesa".
set -euo pipefail
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export PATH="$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH"
PROJECT=${PROJECT:-/project}
NVK=${NVK_SDK:-$PROJECT/.tools/mesa-sdk/opt/devkitpro/portlibs/switch}
VULKAN_INCLUDE=${VULKAN_INCLUDE:-$PROJECT/.tools/mesa-switch/include}
OUT=$PROJECT/out/probe
SRC=$PROJECT/tools/switch/vk-probe

[ -f "$NVK/lib/libvulkan.a" ] || { echo "missing $NVK/lib/libvulkan.a" >&2; exit 1; }
[ -f "$VULKAN_INCLUDE/vulkan/vulkan_vi.h" ] || { echo "missing Vulkan headers in $VULKAN_INCLUDE" >&2; exit 1; }
mkdir -p "$OUT"
# Flags of tools/switch/cmake/switch-devkitA64.cmake, so the probe exercises the
# driver exactly as the game links it.
ARCH=(-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -ftls-model=local-exec -fPIE
  -ffunction-sections -fdata-sections -D_GNU_SOURCE)
aarch64-none-elf-gcc -c "$PROJECT/sdk/src/ui/switch_elf_stubs.c" -o "$OUT/switch_elf_stubs.o" \
  -O2 "${ARCH[@]}" -D__SWITCH__ -I"$DEVKITPRO/libnx/include"
# The libraries of sdk/cmake/rexglue_switch.cmake: whole-archive NVK, then a
# group so the remaining archives are rescanned.
aarch64-none-elf-g++ "$SRC/vk_probe.cpp" "$OUT/switch_elf_stubs.o" -o "$OUT/vk-probe.elf" \
  -std=c++17 -O2 -g "${ARCH[@]}" -D__SWITCH__ -I"$SRC" -I"$VULKAN_INCLUDE" \
  -I"$DEVKITPRO/libnx/include" -specs="$DEVKITPRO/libnx/switch.specs" -pthread \
  -Wl,--gc-sections -Wl,--allow-multiple-definition -Wl,--no-relax \
  -L"$NVK/lib" -L"$DEVKITPRO/portlibs/switch/lib" -L"$DEVKITPRO/libnx/lib" \
  -Wl,--whole-archive -lvulkan -Wl,--no-whole-archive \
  -Wl,--start-group -lexpat -lzstd -lz -lnx -lstdc++ -lm -Wl,--end-group
nacptool --create "Superman Returns NX Vulkan probe" "SR NX contributors" "0.1.0" "$OUT/vk-probe.nacp"
elf2nro "$OUT/vk-probe.elf" "$OUT/vk-probe.nro" --nacp="$OUT/vk-probe.nacp" \
  --icon="$DEVKITPRO/libnx/default_icon.jpg"
ls -l "$OUT/vk-probe.nro"
