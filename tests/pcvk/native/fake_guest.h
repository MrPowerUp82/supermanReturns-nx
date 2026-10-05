// Synthetic guest memory for front-end tests: low addresses are the guest heap, the upper half
// of the arena is physical memory (reachable as physical addresses or the 0xA0000000 alias).
#pragma once
#include <bit>
#include <cstdint>
#include <vector>

#include "pcvk/native_renderer/frontend.h"
#include "pcvk/native_renderer/game_profile.h"

namespace fake {
constexpr uint32_t kDevice = 0x10000;
constexpr uint32_t kRing = 0x40000;
constexpr uint32_t kDecl = 0x50000;
constexpr uint32_t kVertexBuffer = 0x100000;  // physical address of the vertex data
constexpr uint32_t kPhysicalBase = 0x800000;  // arena offset of physical memory
constexpr uint32_t kArena = 0x1000000;

class FakeGuest final : public superman_returns::native::GuestAccess {
 public:
  FakeGuest() : mem(kArena) {}
  const uint8_t* Readable(uint32_t address, uint32_t length) override {
    if (address >= 0xA0000000u) return ReadablePhysical(address - 0xA0000000u, length);
    if (uint64_t(address) + length > kPhysicalBase) return nullptr;
    return mem.data() + address;
  }
  const uint8_t* ReadablePhysical(uint32_t physical, uint32_t length) override {
    if (uint64_t(physical) + length > kArena - kPhysicalBase) return nullptr;
    return mem.data() + kPhysicalBase + physical;
  }
  void Put32(uint32_t address, uint32_t value) {
    for (int i = 0; i < 4; ++i) mem[address + i] = uint8_t(value >> (24 - 8 * i));
  }
  void PutF(uint32_t address, float value) { Put32(address, std::bit_cast<uint32_t>(value)); }
  void PutPhysical32(uint32_t physical, uint32_t value) {
    for (int i = 0; i < 4; ++i) mem[kPhysicalBase + physical + i] = uint8_t(value >> (24 - 8 * i));
  }
  std::vector<uint8_t> mem;
};


}  // namespace fake
