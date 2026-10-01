#include "sr_native_ring.h"
#include <cassert>
#include <initializer_list>
#include <limits>

using namespace sr::native;
static std::vector<std::byte> BE(std::initializer_list<uint32_t> words) {
  std::vector<std::byte> bytes;
  for (auto word : words)
    for (int shift : {24, 16, 8, 0}) bytes.push_back(std::byte((word >> shift) & 255));
  return bytes;
}
static void Check(uint32_t header, std::initializer_list<uint32_t> values) {
  auto bytes = BE({header, 0x12345678, 0x89abcdef});
  Cursor cursor{bytes, 0, uint32_t(values.size() + 1), 0};
  PacketView packet;
  assert(PeekPacket(cursor, packet) == PacketResult::kConsumed);
  assert(cursor.position == 0 && packet.header == header);
  assert(packet.payload == std::vector<uint32_t>(values));
  CommitPacket(cursor, packet);
  assert(cursor.position == values.size() + 1);
}
int main() {
  Check(0x80000000, {});
  Check(0x00010001, {0x12345678, 0x89abcdef});
  Check(0x40001001, {0x12345678, 0x89abcdef});
  Check(0xc0011000, {0x12345678, 0x89abcdef});
  Check(0, {});
  auto bytes = BE({0x12345678, 0, 0, 0xc0001000});
  Cursor cursor{bytes, 3, 0, 3};
  PacketView packet{99, {77}};
  assert(PeekPacket(cursor, packet) == PacketResult::kIncomplete);
  assert(cursor.position == 3 && packet.header == 99 && packet.payload.at(0) == 77);
  cursor.end = 1;
  assert(PeekPacket(cursor, packet) == PacketResult::kConsumed);
  assert(cursor.position == 3 && packet.payload.at(0) == 0x12345678);
  CommitPacket(cursor, packet);
  assert(cursor.position == 1);
  assert(PeekPacket(cursor, packet) == PacketResult::kIncomplete);
  auto wrapped = BE({0x89abcdef, 0, 0xc0011000, 0x12345678});
  Cursor multi{wrapped, 2, 1, 3};
  assert(PeekPacket(multi, packet) == PacketResult::kConsumed);
  assert((packet.payload == std::vector<uint32_t>{0x12345678, 0x89abcdef}));
  CommitPacket(multi, packet);
  assert(multi.position == 1);
  auto maximal = BE({0xffff1000, 0, 0, 0});
  Cursor large{maximal, 0, 3, 3};
  assert(PeekPacket(large, packet) == PacketResult::kIncomplete);
  Cursor empty{};
  assert(PeekPacket(empty, packet) == PacketResult::kIncomplete);
  auto truncated = BE({0xc0011000, 42});
  Cursor linear{truncated, 0, 2, 0};
  assert(PeekPacket(linear, packet) == PacketResult::kInvalid && linear.position == 0);
  linear.end = 0;
  assert(PeekPacket(linear, packet) == PacketResult::kIncomplete);
  for (Cursor invalid : {Cursor{bytes, 0, 1, 2}, Cursor{bytes, 4, 1, 3},
       Cursor{bytes, 0, 4, 3}, Cursor{bytes, 2, 1, 0}, Cursor{bytes, 0, 5, 0},
       Cursor{std::span(bytes).first(15), 0, 1, 3},
       Cursor{std::span(bytes).first(12), 0, 1, 3},
       Cursor{bytes, 0, 1, UINT32_MAX}})
    assert(PeekPacket(invalid, packet) == PacketResult::kInvalid);
  // An unaligned host pointer is safe: decoding uses bytes rather than uint32_t loads.
  auto unaligned = BE({0x80000000});
  unaligned.insert(unaligned.begin(), std::byte{0});
  Cursor odd{std::span(unaligned).subspan(1, 4), 0, 1, 0};
  assert(PeekPacket(odd, packet) == PacketResult::kConsumed);
  assert(!ValidPhysicalRange(0x1ffffffc, 8));
  assert(ValidPhysicalRange(0x1ffffffc, 4));
  assert(ValidPhysicalRange(0xbffffffc, 4));
  assert(!ValidPhysicalRange(0xbffffffc, 8));
  assert(ValidPhysicalRange(0xa0000000, 0x20000000));
  assert(!ValidPhysicalRange(0, 0x20000001));
  assert(!ValidPhysicalRange(0, std::numeric_limits<uint64_t>::max()));
  assert(ValidPhysicalRange(0xffffffff, 1));
  assert(!ValidPhysicalRange(0xffffffff, 2));
}
