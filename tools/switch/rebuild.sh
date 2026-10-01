#!/usr/bin/env bash
# Incremental rebuild after compile-check.sh: syncs app/ and the SDK sources
# (not thirdparty) into the volume, preserving mtimes, then relinks with the
# real Mesa driver and copies the NRO to app/out/switch.
set -euo pipefail
export DEVKITPRO=/opt/devkitpro
export PATH="$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH"
SOURCE=/work/superman-source
NVK=/project/.tools/mesa-sdk/opt/devkitpro/portlibs/switch
# Incremental only: a clean environment has neither the synced sources nor the
# configured build tree that compile-check.sh creates; run it first (a clean
# full build is "tools/build-docker.sh game").
if [ ! -f /work/game-check/CMakeCache.txt ] || [ ! -d "$SOURCE/app" ]; then
  echo "rebuild.sh: /work/game-check or $SOURCE is not prepared; run compile-check.sh first" >&2
  exit 1
fi
[ -f "$NVK/lib/libvulkan.a" ] || { echo "rebuild.sh: missing $NVK/lib/libvulkan.a" >&2; exit 1; }
tar -cf - --exclude=app/out -C /project app sdk/src sdk/include sdk/cmake sdk/CMakeLists.txt \
  tools/switch/cmake | tar -xf - -C "$SOURCE"
cmake -S "$SOURCE/app" -B /work/game-check -DREXGLUE_SWITCH_NVK_SDK="$NVK" >/dev/null
cmake --build /work/game-check --parallel "${JOBS:-4}" > /work/rebuild.log 2>&1 || {
  grep -E "error|undefined" /work/rebuild.log | head -40; exit 1; }
mkdir -p /project/app/out/switch
cp /work/game-check/superman_returns.nro /project/app/out/switch/
ls -la /project/app/out/switch/superman_returns.nro
