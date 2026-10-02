#pragma once
#include "sr_native_commands.h"
#include <array>
#include <span>

namespace sr::native {
class StateMirror {
 public:
  static constexpr uint32_t kRegisterCount = 0x5003;
  NativeResult Scan(const GuestMemory&, std::span<const std::byte>, PacketSite);
  void ResetSegment(uint64_t allocation_epoch);
  uint32_t Register(uint32_t index) const { return index < kRegisterCount ? registers_[index] : 0; }
  bool Written(uint32_t index) const { return index < kRegisterCount && written_[index]; }
  uint64_t VsVersion() const { return vs_version_; }
  uint64_t PsVersion() const { return ps_version_; }
  const std::vector<PacketStamp>& Stamps() const { return stamps_; }
  const std::vector<uint32_t>& ShaderCode(bool vertex) const { return code_[vertex ? 0 : 1]; }

 private:
  NativeResult ScanImpl(const GuestMemory&, std::span<const std::byte>, PacketSite,
                        std::vector<uint32_t>& indirects, uint64_t& remaining_words);
  bool Write(uint32_t, uint32_t);
  std::array<uint32_t, kRegisterCount> registers_{};
  std::array<bool, kRegisterCount> written_{};
  std::array<std::vector<uint32_t>, 2> code_;
  uint64_t epoch_ = 0, vs_version_ = 0, ps_version_ = 0;
  PacketSite last_site_{};
  std::vector<std::byte> last_bytes_;
  std::vector<PacketStamp> stamps_;
  uint32_t next_address_ = 0;
  bool has_last_ = false;
};
}  // namespace sr::native
