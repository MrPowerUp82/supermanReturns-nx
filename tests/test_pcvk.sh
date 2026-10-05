#!/usr/bin/env bash
# Host tests of the native Vulkan renderer imported from superman_returns_recomp (app/src/pcvk).
# No game data, no SDK build and no console: the renderer is exercised on whatever Vulkan device the
# host has (Mesa lavapipe is enough) and a synthetic guest.
#
# Needs: cmake, ninja, a C++23 compiler, the Vulkan headers and loader (libvulkan-dev), a Vulkan
# driver (mesa-vulkan-drivers), libxxhash-dev. Optional, enables the shader-contract and end-to-end
# tests: DXC (.tools/dxc or on PATH) and the patched emitter tree
# (python tools/vkshaders/fetch_xenosrecomp.py).
#
#   tests/test_pcvk.sh                      # Release build, all tests
#   SANITIZE=address,undefined tests/test_pcvk.sh   # needs g++ (CXX=g++): clang lacks the runtime here
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=${BUILD_DIR:-out/tests-pcvk}
flags=()
if [[ -n "${SANITIZE:-}" ]]; then
  flags+=("-DCMAKE_CXX_FLAGS=-fsanitize=${SANITIZE} -fno-omit-frame-pointer" -DCMAKE_BUILD_TYPE=RelWithDebInfo)
else
  flags+=(-DCMAKE_BUILD_TYPE=Release)
fi
if [[ -d .tools/dxc/lib ]]; then export LD_LIBRARY_PATH="$PWD/.tools/dxc/lib:${LD_LIBRARY_PATH:-}"; fi
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/tmp/xdg}; mkdir -p "$XDG_RUNTIME_DIR"
cmake -S tests/pcvk -B "$build_dir" -G Ninja "${flags[@]}" ${CXX:+-DCMAKE_CXX_COMPILER=$CXX} >/dev/null
cmake --build "$build_dir"
ctest --test-dir "$build_dir" --output-on-failure
if [[ -x "$build_dir/sr_pcvk_pack_check" ]]; then
  SR_PCVK_PACK_CHECK="$PWD/$build_dir/sr_pcvk_pack_check" python3 -m unittest tests.test_vkshaders
fi
