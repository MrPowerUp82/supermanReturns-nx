#pragma once
#include "loader.h"
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>
namespace superman_returns::graphics::vulkan {
// Header sanity of a SPIR-V module (magic, version 1.0-1.3, non-zero bound, schema 0).
bool ValidSpirv(std::span<const uint32_t> words);
// Reads a validated module from disk (tests, offline tools).
bool ReadSpirv(const std::filesystem::path &, std::vector<uint32_t> &, Error &);
} // namespace superman_returns::graphics::vulkan
