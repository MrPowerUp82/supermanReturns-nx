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
bool CompareWait(uint32_t info, uint32_t value, uint32_t reference, uint32_t mask);
struct Services {
  // A false effect callback must leave its target unchanged; true commits it once.
  // Register callbacks include SDK side effects (scratch writeback, dirty coherency).
  std::function<bool(uint32_t, uint32_t&)> read_register;
  std::function<bool(uint32_t, uint32_t)> write_register;
  // Memory addresses retain the SDK endian bits in the low two bits.
  std::function<bool(uint32_t, uint32_t&)> read_memory;
  std::function<bool(uint32_t, uint32_t)> write_memory;
  // Length is in dwords; return exactly length * 4 bytes in guest byte order.
  std::function<bool(uint32_t, uint32_t, std::vector<std::byte>&)> read_indirect;
  // Dispatch command-stream interrupt source 1 to this CPU index (0..5).
  std::function<bool(uint32_t)> interrupt;
  // Accept a swap request; actual display completion belongs to the presenter.
  std::function<bool(uint32_t, uint32_t, uint32_t)> present;
  // Read-only proof supplied by the adapter: stage 0=VS, 1=PS, host-order code.
  // Missing/false means a draw using this shader remains blocked (may memexport).
  std::function<bool(uint32_t, std::span<const uint32_t>)> shader_is_memory_safe;
  std::function<bool()> cancelled;
  // Short, cancellable adapter pause between false wait probes.
  std::function<void()> pause_wait;
  // info, address, reference, mask; the adapter rate-limits by elapsed time.
  std::function<void(uint32_t, uint32_t, uint32_t, uint32_t)> report_wait;
};
struct RingCounters {
  uint64_t packets = 0, indirects = 0, draws_omitted = 0;
  uint64_t swap_requests = 0, refresh_completed = 0;
  uint64_t interrupts = 0, blocked = 0, invalid = 0;
  std::array<uint64_t, 128> opcodes{};
  // Address is a ring byte offset or a physical address inside an indirect.
  uint32_t last_blocked_opcode = 0, last_blocked_address = 0;
  uint64_t draws_shader_blocked = 0;
};
class RingExecutor {
 public:
  explicit RingExecutor(Services services);
  RingExecutor(const RingExecutor&) = delete;
  RingExecutor& operator=(const RingExecutor&) = delete;
  // A suspended packet belongs to this cursor generation. Destroy/recreate the
  // executor when replacing the ring; do not reuse it with another backing span.
  PacketResult ProcessNext(Cursor&);
  const RingCounters& counters() const { return counters_; }
 private:
  struct PendingPacket {
    PacketView packet;
    bool loaded = false, child_started = false, prepared = false;
    size_t effect = 0;
    std::vector<uint32_t> values;
  };
  struct IndirectFrame {
    uint32_t address = 0;
    std::vector<std::byte> storage;
    Cursor cursor;
    PendingPacket pending;
  };
  PacketResult Execute(const PacketView&, uint32_t depth);
  bool Read(bool memory, uint32_t address, uint32_t& value);
  bool Write(bool memory, uint32_t address, uint32_t value);
  PacketResult WriteValues(bool memory, uint32_t address, uint32_t stride,
                           std::span<const uint32_t> values);
  Services services_;
  RingCounters counters_;
  uint64_t bin_mask_ = 0xffffffffull, bin_select_ = 0xffffffffull;
  std::vector<std::pair<uint32_t, uint32_t>> active_indirects_;
  std::vector<IndirectFrame> indirect_stack_;
  std::array<std::vector<uint32_t>, 2> shaders_;
  std::array<bool, 2> shader_loaded_{};
  PendingPacket root_pending_;
  PendingPacket* executing_ = nullptr;
  const std::byte* root_data_ = nullptr;
  size_t root_size_ = 0;
  uint32_t root_position_ = 0, root_mask_ = 0;
};
}  // namespace sr::native
