#include "sr_native_guest.h"
#include <limits>
#include <utility>

namespace sr::native {
GuestMemory::GuestMemory(Operations operations) : operations_(std::move(operations)) {}

bool GuestMemory::Valid(GuestAddress address, uint64_t size, bool write) const {
  if (size > std::numeric_limits<uint32_t>::max() ||
      uint64_t(address) + size > (uint64_t{1} << 32)) return false;
  if (!size) return true;
  return operations_.valid && operations_.valid(address, uint32_t(size), write);
}

bool GuestMemory::Copy(GuestAddress address, uint32_t size, std::vector<std::byte>& out) const {
  if (!Valid(address, size, false) || (size && !operations_.read)) return false;
  std::vector<std::byte> copy(size);
  if (size && !operations_.read(address, copy)) return false;
  out = std::move(copy);
  return true;
}

bool GuestMemory::ReadU32(GuestAddress address, uint32_t& out) const {
  std::vector<std::byte> bytes;
  if (!Copy(address, 4, bytes)) return false;
  uint32_t value = 0;
  for (const auto byte : bytes) value = (value << 8) | std::to_integer<uint32_t>(byte);
  out = value;
  return true;
}
bool GuestMemory::CopyPhysical(uint32_t address,uint32_t size,std::vector<std::byte>& out) const {
  if (uint64_t(address)+size>0x20000000ull) return false;
  if (size && (!operations_.valid_physical || !operations_.read_physical ||
      !operations_.valid_physical(address,size))) return false;
  std::vector<std::byte> copy(size);
  if (size && !operations_.read_physical(address,copy)) return false;
  out=std::move(copy);return true;
}

bool GuestMemory::Write(GuestAddress address, std::span<const std::byte> bytes) const {
  if (!Valid(address, bytes.size(), true)) return false;
  return bytes.empty() || (operations_.write && operations_.write(address, bytes));
}
}  // namespace sr::native
