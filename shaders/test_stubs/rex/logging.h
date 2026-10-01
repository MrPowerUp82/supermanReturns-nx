#pragma once
// Host test stub for the SDK logging macros used by pack_shaders.cpp.
#define FMT_HEADER_ONLY
#include <fmt/format.h>
#include <cstdio>
#define REXGPU_INFO(...) std::fprintf(stderr, "[info] %s\n", fmt::format(__VA_ARGS__).c_str())
#define REXGPU_WARN(...) std::fprintf(stderr, "[warn] %s\n", fmt::format(__VA_ARGS__).c_str())
#define REXGPU_ERROR(...) std::fprintf(stderr, "[error] %s\n", fmt::format(__VA_ARGS__).c_str())
