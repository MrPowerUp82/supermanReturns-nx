#!/usr/bin/env bash
set -euo pipefail
export DEVKITPRO=/opt/devkitpro
export PATH="$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH"
SOURCE=/work/superman-source
mkdir -p "$SOURCE"
if [ -f /project/.tools/project-build-source.tar ] && tar -tf /project/.tools/project-build-source.tar >/dev/null 2>&1; then
    tar -xf /project/.tools/project-build-source.tar -C "$SOURCE"
else
    tar -cf - --exclude=app/out --exclude=sdk/out -C /project app sdk tools/switch/cmake | tar -xf - -C "$SOURCE"
fi
cd "$SOURCE"
cmake -S app -B /work/game-switch -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$SOURCE/tools/switch/cmake/switch-devkitA64.cmake" \
  -DREXSDK_DIR="$SOURCE/sdk" \
  -DREXGLUE_SWITCH_NVK_SDK=/project/.tools/mesa-sdk/opt/devkitpro/portlibs/switch
cmake --build /work/game-switch --parallel "${JOBS:-4}"
mkdir -p /project/app/out/switch
cp /work/game-switch/superman_returns.nro /project/app/out/switch/
