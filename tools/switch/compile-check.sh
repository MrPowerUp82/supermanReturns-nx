#!/usr/bin/env bash
# Compiles every game/SDK object for the Switch without the real Mesa driver.
# An empty libvulkan.a satisfies configure; only the final link is expected to fail.
# Run in devkitpro/devkita64 with /project mounted and a volume on /work.
set -euo pipefail
export DEVKITPRO=/opt/devkitpro
export PATH="$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH"
SOURCE=/work/superman-source
STUB=/work/stub-nvk
mkdir -p "$SOURCE" "$STUB/lib"
[ -f "$STUB/lib/libvulkan.a" ] || aarch64-none-elf-ar rc "$STUB/lib/libvulkan.a"
tar -cf - --exclude=app/out --exclude=sdk/out -C /project app sdk tools/switch/cmake | tar -xf - -C "$SOURCE"
cmake -S "$SOURCE/app" -B /work/game-check -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$SOURCE/tools/switch/cmake/switch-devkitA64.cmake" \
  -DREXSDK_DIR="$SOURCE/sdk" -DREXGLUE_SWITCH_NVK_SDK="$STUB"
cmake --build /work/game-check --parallel "${JOBS:-4}" -- -k 2>&1 | tee /work/compile-check.log | grep -E 'error|Error|warning: .*alias' || true
echo "Full log: /work/compile-check.log"
