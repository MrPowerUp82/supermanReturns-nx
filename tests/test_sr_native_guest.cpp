#include "sr_native_guest.h"
#include <array>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <limits>
using namespace sr::native;
int main() {
  std::array<std::byte, 32> bytes{};
  bytes[0] = std::byte{0x12}; bytes[1] = std::byte{0x34};
  bytes[2] = std::byte{0x56}; bytes[3] = std::byte{0x78};
  bool committed = true, writable = true, read_ok = true;
  unsigned reads = 0, writes = 0;
  GuestMemory memory({
      [&](GuestAddress a, uint32_t n, bool write) {
        return committed && (!write || writable) && uint64_t(a) + n <= bytes.size();
      },
      [&](GuestAddress a, std::span<std::byte> out) {
        ++reads;
        if (!read_ok) { out[0] = std::byte{0xff}; return false; }
        std::memcpy(out.data(), bytes.data() + a, out.size()); return true;
      },
      [&](GuestAddress a, std::span<const std::byte> in) {
        ++writes; std::memcpy(bytes.data() + a, in.data(), in.size()); return true;
      }});
  uint32_t word = 0;
  assert(memory.ReadU32(0, word) && word == 0x12345678);
  std::vector<std::byte> out{std::byte{0xaa}};
  assert(!memory.Copy(0xfffffffeu, 4, out));
  assert(out == std::vector<std::byte>{std::byte{0xaa}} && reads == 1);
  committed = false;
  assert(!memory.Copy(28, 8, out) && out[0] == std::byte{0xaa});
  assert(!memory.Copy(0, 4, out));
  committed = true; read_ok = false;
  assert(!memory.Copy(0, 4, out) && out[0] == std::byte{0xaa});
  word = 77;
  assert(!memory.ReadU32(0, word) && word == 77);
  read_ok = true; writable = false;
  const std::array replacement{std::byte{0xee}, std::byte{0xff}};
  assert(!memory.Write(0, replacement) && writes == 0 && bytes[0] == std::byte{0x12});
  writable = true;
  assert(memory.Write(30, replacement) && writes == 1 && bytes[31] == std::byte{0xff});
  assert(!memory.Write(31, replacement) && writes == 1);
  assert(memory.Copy(0, 0, out) && out.empty());
  GuestMemory disconnected({});
  assert(!disconnected.Copy(0, 1, out));
  std::puts("guest memory: transactional range and endian tests passed");
}
