#!/usr/bin/env bash
# Run in the project-specific Mesa image with /project and /work mounted.
set -euo pipefail
export DEVKITPRO=/opt/devkitpro
export PATH="/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:/root/.cargo/bin:$PATH"
export MESA_SWITCH_RUST_TARGET=aarch64-unknown-linux-gnu
JOBS=${JOBS:-4}
MESA=/work/mesa
mkdir -p "$MESA"
# Re-sync on every run: the tarball carries the patched source, and a patch
# change must reach the build tree. tar keeps mtimes, so ninja only rebuilds
# the files that changed.
tar -xf /project/.tools/mesa-build-source.tar -C "$MESA"
python3 - "$MESA" <<'PY'
from pathlib import Path
import subprocess
import sys
root = Path(sys.argv[1])
for path in root.rglob('*.sh'):
    content = path.read_bytes()
    if b'\r\n' in content:
        path.write_bytes(content.replace(b'\r\n', b'\n'))
# NTFS archives do not preserve Git's executable bit. Restore it for host tools.
for entry in subprocess.check_output(['git', '-C', str(root), 'ls-files', '-s'], text=True).splitlines():
    metadata, name = entry.split('\t', 1)
    if metadata.startswith('100755 '):
        path = root / name
        path.chmod(path.stat().st_mode | 0o111)
PY
test "$(git -C "$MESA" rev-parse HEAD)" = 1a8c1a66d6fd8d65f10107c4627ffc3606ba5631
rustup target add "$MESA_SWITCH_RUST_TARGET"
mkdir -p /usr/local/libexec
cp "$MESA/bindgen-switch-wrapper.sh" /usr/local/libexec/bindgen
cp "$MESA/rustc-switch-wrapper.sh" /usr/local/libexec/rustc
cp "$MESA/bindgen-atomic-shim.h" /usr/local/libexec/
chmod +x /usr/local/libexec/bindgen /usr/local/libexec/rustc
cp "$MESA/src/gallium/winsys/nouveau/drm/nouveau.h" "$DEVKITPRO/portlibs/switch/include/"
cp "$MESA/src/nouveau/headers/nv_device_info.h" "$DEVKITPRO/portlibs/switch/include/"
if [ ! -d /usr/lib/llvm-15/lib/clang/15/include ]; then
    # The resource directory carries the point release (15.0.6 on bookworm).
    CLANG_RESOURCE=$(ls -d /usr/lib/llvm-15/lib/clang/15.*/include | head -n 1)
    mkdir -p /usr/lib/llvm-15/lib/clang/15
    ln -s "$CLANG_RESOURCE" /usr/lib/llvm-15/lib/clang/15/include
fi
cd "$MESA"
if [ ! -f builddir-native/build.ninja ]; then
    # MESA_NATIVE_SETUP_ARGS lets hosts with several LLVMs pin LLVM 15, e.g.
    # "--native-file llvm15.ini"; the Docker image only has LLVM 15.
    # shellcheck disable=SC2086
    meson setup builddir-native ${MESA_NATIVE_SETUP_ARGS:-} -Dvulkan-drivers= -Dgallium-drivers= \
      -Dshader-cache=true -Dplatforms= -Dglx=disabled -Degl=disabled \
      -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dtools=[] \
      -Dllvm=enabled -Dmesa-clc=enabled -Dprecomp-compiler=enabled -Dinstall-mesa-clc=true
fi
ninja -C builddir-native -j"$JOBS" src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2
export PATH="/usr/local/libexec:$MESA/builddir-native/src/compiler/clc:$MESA/builddir-native/src/compiler/spirv:$PATH"
if [ ! -f builddir-switch/build.ninja ]; then
    meson setup builddir-switch --cross-file switch_cross_file.txt --buildtype=release \
      --prefix=/opt/devkitpro/portlibs/switch -Doptimization=2 -Db_lto=false -Db_ndebug=true \
      -Dvulkan-drivers=nouveau -Dgallium-drivers=nouveau -Dshader-cache=true \
      -Dgallium-rusticl=false -Dplatforms=switch -Dglx=disabled -Degl=disabled \
      -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dllvm=disabled \
      -Dshared-glapi=disabled -Dshared-llvm=disabled -Dmesa-clc=system \
      -Dprecomp-compiler=system -Dcpp_rtti=false
fi
ninja -C builddir-switch -j"$JOBS" src/nouveau/vulkan/libvulkan.a
# This loaderless archive folds Mesa's private archives into one library.
# Consumers get zlib/expat/zstd from devkitPro and Vulkan headers from the SDK.
STAGE=/project/.tools/mesa-sdk/opt/devkitpro/portlibs/switch/lib
mkdir -p "$STAGE"
cp builddir-switch/src/nouveau/vulkan/libvulkan.a "$STAGE/"
# The MRI merge leaves an index without the bundled Rust std/core members, so
# the game link reports core::panicking::* as undefined. Rebuild the index.
aarch64-none-elf-ranlib "$STAGE/libvulkan.a"
echo "Mesa SDK ready under .tools/mesa-sdk/opt/devkitpro/portlibs/switch"
