#!/usr/bin/env bash
set -euo pipefail
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export PATH="$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH"
cd /project
mkdir -p out/probe
aarch64-none-elf-g++ tools/switch/probe.cpp -o out/probe/platform-probe.elf \
  -std=c++17 -O2 -march=armv8-a+crc+crypto -mtune=cortex-a57 \
  -mtp=soft -fPIE -D__SWITCH__ -I"$DEVKITPRO/libnx/include" \
  -L"$DEVKITPRO/libnx/lib" -specs="$DEVKITPRO/libnx/switch.specs" -lnx
nacptool --create "Superman Returns NX platform probe" "SR NX contributors" "0.1.0" out/probe/platform-probe.nacp
elf2nro out/probe/platform-probe.elf out/probe/platform-probe.nro \
  --nacp=out/probe/platform-probe.nacp --icon="$DEVKITPRO/libnx/default_icon.jpg"
