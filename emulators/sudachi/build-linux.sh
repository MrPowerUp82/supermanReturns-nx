#!/usr/bin/env bash
# Builds a headless Sudachi (sudachi-cmd) on Linux for diagnosing the port.
#
#   emulators/sudachi/build-linux.sh WORKDIR [stock|compat]
#
# stock  : the source mirror with only sudachi-linux-build.patch (build fixes for
#          a current Ubuntu toolchain, no behavior change). Reproduces Sudachi
#          1.0.15's behavior with this port.
# compat : stock plus sudachi-nvk-compat.patch (the emulator fixes).
#
# Source: https://github.com/p-yukusai/sudachi-emu (a7e2127, a mirror of Sudachi
# as of 2024-09-19; the original repositories are no longer public). Its
# submodule pins are not recorded, so the dependencies below are the
# yuzu-mirror/upstream revisions that build with it. This is NOT the binary the
# user runs on Windows (sudachi-refresh-c7431bd, 1.0.15); see README.md.
#
# Needs: git, cmake, ninja, gcc/g++ 13, and the Ubuntu 24.04 packages listed in
# README.md. Result: WORKDIR/sudachi/build/bin/sudachi-cmd
set -euo pipefail

WORK=${1:?usage: $0 WORKDIR [stock|compat]}
MODE=${2:-compat}
HERE=$(cd "$(dirname "$0")" && pwd)
SRC="$WORK/sudachi"

clone_at() {  # dir url commit
    local dir=$1 url=$2 commit=$3
    if [ ! -d "$dir/.git" ]; then
        rm -rf "$dir"
        git init -q "$dir"
        git -C "$dir" remote add origin "$url"
    fi
    git -C "$dir" fetch -q --depth 1 origin "$commit"
    git -C "$dir" -c advice.detachedHead=false checkout -q FETCH_HEAD
    git -C "$dir" submodule update -q --init --recursive --depth 1
}

mkdir -p "$WORK"
clone_at "$SRC" https://github.com/p-yukusai/sudachi-emu a7e2127f17841728eaf2ee2eea51aa5a58a85e80
E="$SRC/externals"
clone_at "$E/dynarmic" https://github.com/yuzu-mirror/dynarmic f884bc0dfcc7eccaaf63e3b2f99ac2a180479cbb
clone_at "$E/sirit" https://github.com/yuzu-mirror/sirit 4ab79a8c023aa63caaa93848b09b9fe8b183b1a9
clone_at "$E/mbedtls" https://github.com/yuzu-mirror/mbedtls 8c88150ca139e06aa2aae8349df8292a88148ea1
clone_at "$E/vulkan-headers" https://github.com/KhronosGroup/Vulkan-Headers b379292b2ab6df5771ba9870d53cf8b2c9295daf
clone_at "$E/Vulkan-Utility-Libraries" https://github.com/KhronosGroup/Vulkan-Utility-Libraries 5f26cf65a18bc89a8e3d6569c14314b6fdac8d4d
clone_at "$E/VulkanMemoryAllocator" https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator 009ecd192c1289c7529bff248a16cfe896254816
clone_at "$E/xbyak" https://github.com/herumi/xbyak aabb091ae37068498751fd58202a9854408ecb0e
clone_at "$E/enet" https://github.com/lsalzman/enet 2662c0de09e36f2a2030ccc2c528a3e4c9e8138a
clone_at "$E/opus" https://github.com/xiph/opus 82ac57d9f1aaf575800cf17373348e45b7ce6c0d
clone_at "$E/simpleini" https://github.com/brofield/simpleini 09c21bda1dc1b578fa55f4a005d79b0afd481296
clone_at "$E/SDL" https://github.com/libsdl-org/SDL 9519b9916cd29a14587af0507292f2bd31dd5752
clone_at "$E/nx_tzdb/tzdb_to_nx" https://github.com/lat9nq/tzdb_to_nx 97929690234f2b4add36b33657fe3fe09bd57dfd

git -C "$SRC" checkout -q -- src
git -C "$SRC" apply "$HERE/sudachi-linux-build.patch"
case "$MODE" in
    stock) ;;
    compat) git -C "$SRC" apply "$HERE/sudachi-nvk-compat.patch" ;;
    *) echo "unknown mode $MODE" >&2; exit 1 ;;
esac

cmake -S "$SRC" -B "$SRC/build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
    -DENABLE_QT=OFF -DENABLE_WEB_SERVICE=OFF -DENABLE_CUBEB=OFF -DENABLE_LIBUSB=OFF \
    -DSUDACHI_USE_EXTERNAL_SDL2=OFF -DSUDACHI_TESTS=OFF -DSUDACHI_ROOM=OFF \
    -DSUDACHI_CHECK_SUBMODULES=OFF -DSUDACHI_USE_PRECOMPILED_HEADERS=OFF
cmake --build "$SRC/build" --target sudachi-cmd --parallel "${JOBS:-$(nproc)}"
echo "Built $SRC/build/bin/sudachi-cmd ($MODE)"
