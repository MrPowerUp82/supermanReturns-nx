#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace sr::native {
using GuestAddress = uint32_t;

// The binding validates the entire committed interval before copying. A failed
// write callback must leave guest memory unchanged. Copy stages its result so a
// failed read never publishes partial data to the caller.
class GuestMemory {
 public:
  struct Operations {
    std::function<bool(GuestAddress, uint32_t, bool write)> valid;
    std::function<bool(GuestAddress, std::span<std::byte>)> read;
    std::function<bool(GuestAddress, std::span<const std::byte>)> write;
    std::function<bool(uint32_t, uint32_t)> valid_physical = {};
    std::function<bool(uint32_t, std::span<std::byte>)> read_physical = {};
  };
  explicit GuestMemory(Operations operations);
  bool Copy(GuestAddress address, uint32_t size, std::vector<std::byte>& out) const;
  // Canonical GPU physical address; never infer commitment from a virtual alias.
  bool CopyPhysical(uint32_t address, uint32_t size, std::vector<std::byte>& out) const;
  bool ReadU32(GuestAddress address, uint32_t& out) const;
  bool Write(GuestAddress address, std::span<const std::byte> bytes) const;

 private:
  bool Valid(GuestAddress address, uint64_t size, bool write) const;
  Operations operations_;
};
}  // namespace sr::native
