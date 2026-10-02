#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
flags=(-std=c++23 -Wall -Wextra -Werror -pthread -Iapp/src -Isdk/include)
if [[ "${SANITIZE:-0}" == 1 ]]; then
  flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
elif [[ "${SANITIZE:-0}" == undefined ]]; then
  flags+=(-fsanitize=undefined -fno-omit-frame-pointer -g)
fi
"${CXX:-c++}" "${flags[@]}" tests/test_sr_native_profile.cpp -o "$build_dir/test_sr_native_profile"
"$build_dir/test_sr_native_profile"
"${CXX:-c++}" "${flags[@]}" tests/test_sr_native_guest.cpp app/src/sr_native_guest.cpp -o "$build_dir/test_sr_native_guest"
"$build_dir/test_sr_native_guest"
"${CXX:-c++}" "${flags[@]}" tests/test_sr_native_capture.cpp app/src/sr_native_capture.cpp app/src/sr_native_mirror.cpp app/src/sr_native_guest.cpp app/src/sr_native_ring.cpp -o "$build_dir/test_sr_native_capture"
"$build_dir/test_sr_native_capture"
"${CXX:-c++}" "${flags[@]}" tests/test_sr_native_ring.cpp app/src/sr_native_ring.cpp -o "$build_dir/test_sr_native_ring"
"$build_dir/test_sr_native_ring"
"${CXX:-c++}" "${flags[@]}" tests/test_sr_native_lifecycle.cpp -o "$build_dir/test_sr_native_lifecycle"
"$build_dir/test_sr_native_lifecycle"
"${CXX:-c++}" "${flags[@]}" tests/test_sr_native_shader_safety.cpp app/src/sr_native_shader_safety.cpp -o "$build_dir/test_sr_native_shader_safety"
"$build_dir/test_sr_native_shader_safety"
