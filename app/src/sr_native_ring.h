#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <utility>
#include <vector>

namespace sr::native {
enum class PacketResult { kConsumed, kIncomplete, kBlocked, kInvalid, kCancelled };
struct Cursor {
  std::span<const std::byte> bytes;
  uint32_t position = 0;
  uint32_t end = 0;
  uint32_t mask = 0;  // Ring capacity minus one, in words; zero for a linear buffer.
};
struct PacketView {
  uint32_t header = 0;
  std::vector<uint32_t> payload;  // Host-order words, owned across a ring wrap.
};
// Neither the cursor nor the output packet changes unless a full packet is available.
PacketResult PeekPacket(const Cursor&, PacketView&);
// Call only for the packet just successfully peeked from this cursor.
void CommitPacket(Cursor&, const PacketView&);
// Physical extent only; committed-page validation belongs to the memory adapter.
bool ValidPhysicalRange(uint32_t address, uint64_t bytes);
}  // namespace sr::native
