#!/usr/bin/env bash
set -euo pipefail
cd /project
/opt/devkitpro/devkitA64/bin/aarch64-none-elf-g++ \
  -std=c++23 -fsyntax-only -Werror=attributes -march=armv8-a+crc+crypto -mtp=soft \
  -include app/src/sr_recomp_compat.h \
  -D__SWITCH__ -DNX -D_GNU_SOURCE -DSPDLOG_NO_TZ_OFFSET \
  -DSPDLOG_FMT_EXTERNAL -DREX_HAS_VULKAN=1 \
  -I sdk/switch_compat -I sdk/include -I app -I app/src \
  -I /opt/devkitpro/libnx/include -I sdk/thirdparty/fmt/include \
  -I sdk/thirdparty/spdlog/include -I sdk/thirdparty/simde \
  -I sdk/thirdparty/imgui -I sdk/thirdparty/vulkan-headers/include \
  -I sdk/thirdparty/vulkan-memory-allocator/include \
  app/src/main.cpp app/src/xma_fixes.cpp app/src/sr_settings.cpp app/src/skip_intro.cpp \
  app/generated/default/superman_returns_recomp.0.cpp
echo 'ARM64 source syntax OK (not a linked game build)'
