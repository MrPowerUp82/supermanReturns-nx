// Identity of a guest shader container: FNV-1a 64 of its exact bytes. The offline packer
// (tools/vkshaders/build_pack.py) computes the same value.
#pragma once
#include <cstdint>
#include <span>
namespace superman_returns::graphics::shaders {
inline uint64_t Fnv1a64(std::span<const uint8_t> bytes, uint64_t seed = 14695981039346656037ull) {
  uint64_t h = seed;
  for (auto b : bytes) {
    h ^= b;
    h *= 1099511628211ull;
  }
  return h;
}
inline uint64_t ContainerKey(std::span<const uint8_t> container) { return Fnv1a64(container); }
} // namespace superman_returns::graphics::shaders
