#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
flags=(-std=c++23 -Wall -Wextra -Werror -pthread -Iapp/src -Isdk/include)
if [[ "${SANITIZE:-0}" == 1 ]]; then
  flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
fi
"${CXX:-c++}" "${flags[@]}" tests/test_sr_native_ring.cpp app/src/sr_native_ring.cpp -o "$build_dir/test_sr_native_ring"
"$build_dir/test_sr_native_ring"
