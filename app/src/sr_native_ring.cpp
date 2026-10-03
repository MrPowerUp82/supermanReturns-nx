#include "sr_native_ring.h"

#include <algorithm>
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

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

bool CompareWait(uint32_t info, uint32_t value, uint32_t reference, uint32_t mask) {
  value &= mask;  // The SDK masks the observed value, never the reference.
  switch (info & 7) {
    case 0: return false;
    case 1: return value < reference;
    case 2: return value <= reference;
    case 3: return value == reference;
    case 4: return value != reference;
    case 5: return value >= reference;
    case 6: return value > reference;
    case 7: return true;
  }
  return false;
}

RingExecutor::RingExecutor(Services services) : services_(std::move(services)) {
  // A push must not invalidate the currently executing frame or its packet.
  indirect_stack_.reserve(4);
  active_indirects_.reserve(4);
}

bool RingExecutor::Read(bool memory, uint32_t address, uint32_t& value) {
  if (memory && !ValidPhysicalRange(address & ~3u, 4)) return false;
  const auto& callback = memory ? services_.read_memory : services_.read_register;
  return callback && callback(address, value);
}

bool RingExecutor::Write(bool memory, uint32_t address, uint32_t value) {
  const auto& callback = memory ? services_.write_memory : services_.write_register;
  return callback && callback(address, value);
}

PacketResult RingExecutor::WriteValues(bool memory, uint32_t address, uint32_t stride,
                                      std::span<const uint32_t> values) {
  for (; executing_->effect < values.size(); ++executing_->effect) {
    if (services_.cancelled && services_.cancelled()) return PacketResult::kCancelled;
    const auto i = executing_->effect;
    if (!Write(memory, address + uint32_t(i) * stride, values[i])) return PacketResult::kBlocked;
  }
  return PacketResult::kConsumed;
}

PacketResult RingExecutor::LoadPacket(const Cursor& cursor,PendingPacket& pending,uint32_t address) {
  PacketSite before;
  if(services_.native_packet && (!services_.packet_site ||
      !services_.packet_site(address,4,before))) return PacketResult::kBlocked;
  PacketView packet;
  const auto parsed=PeekPacket(cursor,packet);
  if(parsed!=PacketResult::kConsumed) return parsed;
  if(services_.native_packet) {
    PacketSite after;
    const uint32_t bytes=uint32_t((packet.payload.size()+1)*4);
    if(!before.allocation_epoch || before.physical_address!=address ||
        !services_.packet_site(address,bytes,after) || after!=before) return PacketResult::kInvalid;
    pending.stamp={before,packet.header,packet.payload};
  }
  pending.packet=std::move(packet);pending.loaded=true;
  return PacketResult::kConsumed;
}
PacketResult RingExecutor::ProcessNext(Cursor& root) {
  if (services_.cancelled && services_.cancelled()) return PacketResult::kCancelled;
  if (root_pending_.loaded) {
    if (root.bytes.data() != root_data_ || root.bytes.size() != root_size_ ||
        root.position != root_position_ || root.mask != root_mask_ ||
        root.physical_base != root_physical_base_) {
      ++counters_.invalid;
      return PacketResult::kInvalid;
    }
  } else {
    const uint64_t address=uint64_t(root.physical_base)+uint64_t(root.position)*4;
    if(address>UINT32_MAX) return PacketResult::kInvalid;
    auto result = LoadPacket(root,root_pending_,uint32_t(address));
    if (result != PacketResult::kConsumed) {
      if (result == PacketResult::kInvalid) ++counters_.invalid;
      return result;
    }
    root_pending_.loaded = true;
    root_data_ = root.bytes.data(); root_size_ = root.bytes.size();
    root_position_ = root.position; root_mask_ = root.mask;
    root_physical_base_=root.physical_base;
  }
  for (;;) {
    if (services_.cancelled && services_.cancelled()) return PacketResult::kCancelled;
    if (!indirect_stack_.empty() &&
        indirect_stack_.back().cursor.position == indirect_stack_.back().cursor.end) {
      indirect_stack_.pop_back(); active_indirects_.pop_back();
      continue;  // Commit the suspended parent only after this child finishes.
    }
    Cursor& cursor = indirect_stack_.empty() ? root : indirect_stack_.back().cursor;
    PendingPacket& pending = indirect_stack_.empty() ? root_pending_ : indirect_stack_.back().pending;
    const uint32_t address = (indirect_stack_.empty() ? root.physical_base : indirect_stack_.back().address) + cursor.position * 4;
    auto result = PacketResult::kConsumed;
    if (!pending.loaded) {
      result = LoadPacket(cursor,pending,address);
      if(result==PacketResult::kConsumed && services_.native_packet &&
          pending.stamp.site.allocation_epoch!=indirect_stack_.back().allocation_epoch) {
        pending={};result=PacketResult::kInvalid;
      }
    }
    const auto depth = uint32_t(indirect_stack_.size());
    if (result == PacketResult::kConsumed) {
      executing_ = &pending;
      result = Execute(pending.packet, depth);
      executing_ = nullptr;
    }
    if (result != PacketResult::kConsumed) {
      if (result == PacketResult::kBlocked) {
        ++counters_.blocked;
        counters_.last_blocked_opcode = (pending.packet.header >> 8) & 0x7f;
        counters_.last_blocked_address = address;
      } else if (result == PacketResult::kInvalid) ++counters_.invalid;
      return result;
    }
    if (indirect_stack_.size() != depth) continue;  // A new child is now active.
    ++counters_.packets;
    if (pending.packet.header >> 30 == 3) ++counters_.opcodes[(pending.packet.header >> 8) & 0x7f];
    CommitPacket(cursor, pending.packet);
    pending = {};
    if (!depth) return PacketResult::kConsumed;
  }
}

namespace {
using namespace rex::graphics;
using namespace rex::graphics::xenos;
// The register window base is defined by the first register in each SDK range.
uint32_t ConstantBase(uint32_t offset_type) {
  switch ((offset_type >> 16) & 0xff) {
    case 0: return XE_GPU_REG_SHADER_CONSTANT_000_X;
    case 1: return XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
    case 2: return XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031;
    case 3: return XE_GPU_REG_SHADER_CONSTANT_LOOP_00;
    case 4: return XE_GPU_REG_RB_SURFACE_INFO;
    default: return UINT32_MAX;
  }
}
bool MemoryExtent(uint32_t address, size_t words) {
  return ValidPhysicalRange(address & ~3u, uint64_t(words) * 4);
}
}  // namespace

void RecordRefresh(bool ok, RingCounters& counters) {
  if (ok) ++counters.refresh_completed;
}

PacketResult RingExecutor::Execute(const PacketView& packet, uint32_t depth) {
  using enum PacketResult;
  const auto& p = packet.payload;
  const auto type = packet.header >> 30;
  if (!packet.header || type == 2) return kConsumed;
  if (type == 0) {
    const uint32_t base = packet.header & 0x7fff;
    return WriteValues(false, base, (packet.header & 0x8000) ? 0 : 1, p);
  }
  if (type == 1) {
    for (; executing_->effect < 2; ++executing_->effect) {
      if (services_.cancelled && services_.cancelled()) return kCancelled;
      const auto i = executing_->effect;
      if (!Write(false, (packet.header >> (i * 11)) & 0x7ff, p[i])) return kBlocked;
    }
    return kConsumed;
  }
  const uint32_t op = (packet.header >> 8) & 0x7f;
  // Predication precedes the handler, including format checks, as in the SDK.
  if ((packet.header & 1) && (!(bin_select_ & bin_mask_) || op == PM4_XE_SWAP)) {
    if(services_.native_packet && (op==PM4_DRAW_INDX || op==PM4_DRAW_INDX_2 || op==PM4_XE_SWAP))
      return services_.native_packet(executing_->stamp,false);
    return kConsumed;
  }
  switch (op) {
    case PM4_NOP: case PM4_ME_INIT: case PM4_INVALIDATE_STATE:
      return kConsumed;
    case PM4_CONTEXT_UPDATE:
      return p[0] == 0 ? kConsumed : kBlocked;
    case PM4_INTERRUPT:
      for (; executing_->effect < 6; ++executing_->effect) {
        const uint32_t cpu = uint32_t(executing_->effect);
        if (!(p[0] & (1u << cpu))) continue;
        if (services_.cancelled && services_.cancelled()) return kCancelled;
        if (!services_.interrupt || !services_.interrupt(cpu)) return kBlocked;
        ++counters_.interrupts;
      }
      return kConsumed;
    case PM4_XE_SWAP:
      if (p.size() < 4 || p[0] != kSwapSignature) return kInvalid;
      if(services_.native_packet) {
        const auto accepted=services_.native_packet(executing_->stamp,true);
        if(accepted==kConsumed) ++counters_.native_work_accepted;
        return accepted;
      }
      if (!executing_->prepared) { ++counters_.swap_requests; executing_->prepared = true; }
      {
        const bool refreshed = services_.present && services_.present(p[1], p[2], p[3]);
        RecordRefresh(refreshed, counters_);
        if (!refreshed) return kBlocked;
      }
      return kConsumed;
    case PM4_INDIRECT_BUFFER: case PM4_INDIRECT_BUFFER_PFD: {
      if (p.size() < 2 || p[0] & 3 || p[1] & ~0xfffffu || depth >= 4 ||
          !ValidPhysicalRange(p[0], uint64_t(p[1]) * 4)) return kInvalid;
      if (executing_->child_started) return kConsumed;
      const uint32_t address = p[0] & 0x1fffffff;
      const auto key = std::pair(address, p[1]);
      if (std::find(active_indirects_.begin(), active_indirects_.end(), key) != active_indirects_.end()) return kInvalid;
      IndirectFrame frame;
      frame.address = address;
      PacketSite before;
      if(services_.native_packet && (!services_.packet_site ||
          !services_.packet_site(address,p[1]*4,before))) return kBlocked;
      if (!services_.read_indirect || !services_.read_indirect(address, p[1], frame.storage)) return kBlocked;
      if (frame.storage.size() != uint64_t(p[1]) * 4) return kInvalid;
      if(services_.native_packet) {
        PacketSite after;
        if(!before.allocation_epoch || before.physical_address!=address ||
            !services_.packet_site(address,p[1]*4,after) || after!=before) return kInvalid;
        frame.allocation_epoch=before.allocation_epoch;
      }
      frame.cursor = {frame.storage, 0, p[1], 0};
      executing_->child_started = true;
      active_indirects_.push_back(key); indirect_stack_.push_back(std::move(frame));
      ++counters_.indirects;
      return kConsumed;
    }
    case PM4_WAIT_REG_MEM: {
      if (p.size() < 5) return kInvalid;
      for (;;) {
        if (services_.cancelled && services_.cancelled()) return kCancelled;
        uint32_t value = 0;
        if (!Read(p[0] & 0x10, p[1], value)) {
          if (services_.report_wait) services_.report_wait(p[0],p[1],p[2],p[3]);
          return kBlocked;
        }
        if (!(p[0]&0x10) && p[1]==XE_GPU_REG_COHER_STATUS_HOST && (value&0x80000000u)) {
          uint32_t base=0,size=0;
          if (value!=0x81000000 || counters_.draws_omitted || !services_.startup_vertex_coherence ||
              !Read(false,XE_GPU_REG_COHER_BASE_HOST,base) || !Read(false,XE_GPU_REG_COHER_SIZE_HOST,size) ||
              !services_.startup_vertex_coherence(value,base,size) ||
              !Read(false,p[1],value) || (value&0x80000000u)) {
            if (services_.report_wait) services_.report_wait(p[0],p[1],p[2],p[3]);
            return kBlocked;
          }
        }
        if (CompareWait(p[0], value, p[2], p[3])) return kConsumed;
        if (services_.report_wait) services_.report_wait(p[0], p[1], p[2], p[3]);
        if (!services_.pause_wait) return kBlocked;
        services_.pause_wait();  // Only a real comparison can complete this wait.
      }
    }
    case PM4_REG_RMW: {
      if (p.size() < 3) return kInvalid;
      if (!executing_->prepared) {
        uint32_t value, and_mask = p[1], or_mask = p[2];
        if (!Read(false, p[0] & 0x1fff, value) ||
            ((p[0] & 0x80000000u) && !Read(false, p[1] & 0x1fff, and_mask)) ||
            ((p[0] & 0x40000000u) && !Read(false, p[2] & 0x1fff, or_mask))) return kBlocked;
        executing_->values = {(value & and_mask) | or_mask}; executing_->prepared = true;
      }
      return WriteValues(false, p[0] & 0x1fff, 0, executing_->values);
    }
    case PM4_REG_TO_MEM: {
      if (p.size() < 2 || !MemoryExtent(p[1], 1)) return kInvalid;
      if (!executing_->prepared) {
        uint32_t value;
        if (!Read(false, p[0], value)) return kBlocked;
        executing_->values = {value}; executing_->prepared = true;
      }
      return WriteValues(true, p[1], 0, executing_->values);
    }
    case PM4_MEM_WRITE:
      if (!MemoryExtent(p[0], p.size() - 1)) return kInvalid;
      return WriteValues(true, p[0], 4, std::span(p).subspan(1));
    case PM4_COND_WRITE: {
      if (p.size() < 6 || ((p[0] & 0x100) && !MemoryExtent(p[4], 1))) return kInvalid;
      if (!executing_->prepared) {
        uint32_t value;
        if (!Read(p[0] & 0x10, p[1], value)) return kBlocked;
        if (CompareWait(p[0], value, p[2], p[3])) executing_->values = {p[5]};
        executing_->prepared = true;
      }
      return WriteValues(p[0] & 0x100, p[4], 0, executing_->values);
    }
    case PM4_EVENT_WRITE:
      if (p.size() != 1) return kBlocked;  // An event with a memory payload needs real GPU completion.
      return Write(false, XE_GPU_REG_VGT_EVENT_INITIATOR, p[0] & 0x3f) ? kConsumed : kBlocked;
    case PM4_EVENT_WRITE_SHD:
      if (p.size() < 3 || !MemoryExtent(p[1], 1)) return kInvalid;
      if (!executing_->prepared) {
        if (!services_.finish_native_work) return kBlocked;
        const auto finished=services_.finish_native_work();
        if(finished!=kConsumed) return finished;
        // The SDK counter increments on XE_SWAP, not on vblank or omitted draws.
        // Only completed presenter swaps contribute here; retries keep this value.
        executing_->values={p[0]&0x80000000u?uint32_t(counters_.refresh_completed):p[2]};
        executing_->prepared=true;
      }
      if (!executing_->effect) {
        if (!Write(false, XE_GPU_REG_VGT_EVENT_INITIATOR, p[0] & 0x3f)) return kBlocked;
        ++executing_->effect;
      }
      return Write(true, p[1], executing_->values[0]) ? kConsumed : kBlocked;
    case PM4_EVENT_WRITE_EXT:
      return p.size() < 2 ? kInvalid : kBlocked;
    case PM4_WAIT_FOR_IDLE:
      return services_.finish_native_work?services_.finish_native_work():kBlocked;
    case PM4_EVENT_WRITE_ZPD: case PM4_VIZ_QUERY:
      return kBlocked;
    case PM4_SET_CONSTANT: case PM4_SET_CONSTANT2: case PM4_SET_SHADER_CONSTANTS: {
      const uint32_t base = op == PM4_SET_CONSTANT ? ConstantBase(p[0]) : 0;
      if (base == UINT32_MAX) return kInvalid;
      const uint32_t index = base + (p[0] & (op == PM4_SET_CONSTANT ? 0x7ff : 0xffff));
      return WriteValues(false, index, 1, std::span(p).subspan(1));
    }
    case PM4_LOAD_ALU_CONSTANT: {
      if (p.size() < 3) return kInvalid;
      const uint32_t address = p[0] & 0x3fffffff, words = p[2] & 0xfff;
      const uint32_t base = ConstantBase(p[1]);
      if (base == UINT32_MAX || address & 3 || !MemoryExtent(address, words)) return kInvalid;
      if (!executing_->prepared) {
        std::vector<uint32_t> values(words);
        for (uint32_t i = 0; i < words; ++i) {
          if (services_.cancelled && services_.cancelled()) return kCancelled;
          if (!Read(true, (address + i * 4) | uint32_t(Endian::k8in32), values[i])) return kBlocked;
        }
        executing_->values = std::move(values); executing_->prepared = true;
      }
      return WriteValues(false, base + (p[1] & 0x7ff), 1, executing_->values);
    }
    case PM4_SET_BIN_MASK_LO: bin_mask_ = (bin_mask_ & 0xffffffff00000000ull) | p[0]; return kConsumed;
    case PM4_SET_BIN_MASK_HI: bin_mask_ = (bin_mask_ & 0xffffffffull) | (uint64_t(p[0]) << 32); return kConsumed;
    case PM4_SET_BIN_SELECT_LO: bin_select_ = (bin_select_ & 0xffffffff00000000ull) | p[0]; return kConsumed;
    case PM4_SET_BIN_SELECT_HI: bin_select_ = (bin_select_ & 0xffffffffull) | (uint64_t(p[0]) << 32); return kConsumed;
    case PM4_SET_BIN_MASK: case PM4_SET_BIN_SELECT:
      if (p.size() < 2) return kInvalid;
      (op == PM4_SET_BIN_MASK ? bin_mask_ : bin_select_) = (uint64_t(p[0]) << 32) | p[1];
      return kConsumed;
    case PM4_IM_LOAD: case PM4_IM_LOAD_IMMEDIATE: {
      if (p.size() < 2 || p[1] >> 16) return kInvalid;
      const uint32_t shader_type = op == PM4_IM_LOAD ? p[0] & 3 : p[0];
      const uint32_t words = p[1] & 0xffff;
      if (shader_type > uint32_t(ShaderType::kPixel)) return kInvalid;
      std::vector<uint32_t> code;
      if (op == PM4_IM_LOAD_IMMEDIATE) {
        if (words > p.size() - 2) return kInvalid;
        code.assign(p.begin() + 2, p.begin() + 2 + words);
      } else {
        const uint32_t address = p[0] & ~3u;
        if (!MemoryExtent(address, words)) return kInvalid;
        code.resize(words);
        for (uint32_t i = 0; i < words; ++i) {
          if (services_.cancelled && services_.cancelled()) return kCancelled;
          if (!Read(true, (address + i * 4) | uint32_t(Endian::k8in32), code[i])) return kBlocked;
        }
      }
      shaders_[shader_type] = std::move(code); shader_loaded_[shader_type] = true;
      return kConsumed;
    }
    case PM4_DRAW_INDX: case PM4_DRAW_INDX_2: {
      const size_t start = op == PM4_DRAW_INDX ? 1 : 0;
      if (p.size() <= start) return kInvalid;
      const uint32_t source = (p[start] >> 6) & 3;
      if (source == uint32_t(SourceSelect::kDMA) && p.size() < start + 3) return kInvalid;
      if (source == 3) return kInvalid;
      if (source == uint32_t(SourceSelect::kImmediate)) return kBlocked;
      uint32_t mode;
      if (!Read(false, XE_GPU_REG_RB_MODECONTROL, mode)) return kBlocked;
      switch (static_cast<EdramMode>(mode & 7)) {
        case EdramMode::kNoOperation:
        case EdramMode::kColorDepth:
        case EdramMode::kDepthOnly:
          break;
        case EdramMode::kCopy:
          if(!services_.native_packet) return kBlocked;
          break; // A captured resolve token belongs to the native resource backend.
        default: return kBlocked;  // Copy and undefined modes have no harmless draw interpretation.
      }
      if (start && p[0] & 0x100) return kBlocked;
      for (uint32_t stage = 0; stage != 2; ++stage) {
        if (shader_loaded_[stage] && (!services_.shader_is_memory_safe ||
            !services_.shader_is_memory_safe(stage, shaders_[stage]))) {
          ++counters_.draws_shader_blocked; return kBlocked;
        }
      }
      if(services_.native_packet && !executing_->native_accepted) {
        const auto accepted=services_.native_packet(executing_->stamp,true);
        if(accepted!=kConsumed) return accepted;
        executing_->native_accepted=true;
        ++counters_.native_work_accepted;
      }
      const uint32_t regs[] = {XE_GPU_REG_VGT_DRAW_INITIATOR, XE_GPU_REG_VGT_DMA_BASE, XE_GPU_REG_VGT_DMA_SIZE};
      const size_t count = source == uint32_t(SourceSelect::kDMA) ? 3 : 1;
      for (; executing_->effect < count; ++executing_->effect) {
        if (services_.cancelled && services_.cancelled()) return kCancelled;
        const auto i = executing_->effect;
        if (!Write(false, regs[i], p[start + i])) return kBlocked;
      }
      if(!services_.native_packet) ++counters_.draws_omitted;
      return kConsumed;
    }
    default: return kBlocked;
  }
}
}  // namespace sr::native
