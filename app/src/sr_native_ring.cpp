#include "sr_native_ring.h"

namespace sr::native {
namespace {
uint32_t ReadWord(const Cursor& cursor, uint64_t position) {
  if (cursor.mask) position &= cursor.mask;
  const size_t offset = static_cast<size_t>(position * 4);
  uint32_t value = 0;
  for (size_t i = 0; i < 4; ++i)
    value = (value << 8) | std::to_integer<uint32_t>(cursor.bytes[offset + i]);
  return value;
}
uint64_t PayloadWords(uint32_t header) {
  if (header == 0 || (header >> 30) == 2) return 0;
  if ((header >> 30) == 1) return 2;
  return ((header >> 16) & 0x3fff) + 1;
}
}  // namespace

PacketResult PeekPacket(const Cursor& cursor, PacketView& packet) {
  if (cursor.bytes.size() % 4 != 0) return PacketResult::kInvalid;
  const uint64_t words = cursor.bytes.size() / 4;
  uint64_t available = 0;
  if (cursor.mask) {
    const uint64_t capacity = uint64_t(cursor.mask) + 1;
    if ((capacity & (capacity - 1)) != 0 || words != capacity ||
        cursor.position > cursor.mask || cursor.end > cursor.mask)
      return PacketResult::kInvalid;
    available = (cursor.end - cursor.position) & cursor.mask;
  } else {
    if (cursor.position > cursor.end || cursor.end > words) return PacketResult::kInvalid;
    available = uint64_t(cursor.end) - cursor.position;
  }
  if (!available) return PacketResult::kIncomplete;
  const uint32_t header = ReadWord(cursor, cursor.position);
  const uint64_t payload_words = PayloadWords(header);
  if (payload_words + 1 > available)
    return cursor.mask ? PacketResult::kIncomplete : PacketResult::kInvalid;
  PacketView complete;
  complete.header = header;
  complete.payload.reserve(static_cast<size_t>(payload_words));
  for (uint64_t i = 0; i < payload_words; ++i)
    complete.payload.push_back(ReadWord(cursor, uint64_t(cursor.position) + 1 + i));
  packet = std::move(complete);
  return PacketResult::kConsumed;
}

void CommitPacket(Cursor& cursor, const PacketView& packet) {
  const uint64_t next = uint64_t(cursor.position) + PayloadWords(packet.header) + 1;
  cursor.position = static_cast<uint32_t>(cursor.mask ? next & cursor.mask : next);
}

bool ValidPhysicalRange(uint32_t address, uint64_t bytes) {
  constexpr uint64_t kPhysicalSize = 0x20000000;
  const uint64_t normalized = address & (kPhysicalSize - 1);
  return bytes <= kPhysicalSize - normalized;
}
}  // namespace sr::native
