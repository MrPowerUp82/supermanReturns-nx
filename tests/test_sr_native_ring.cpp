#include "sr_native_ring.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <map>
#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

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
static void ParserTests() {
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

using namespace rex::graphics;
using namespace rex::graphics::xenos;
static std::vector<std::byte> Packet(uint32_t op, std::initializer_list<uint32_t> payload,
                                     bool predicate = false) {
  auto result = BE({0xc0000000u | (uint32_t(payload.size() - 1) << 16) | (op << 8) |
                    uint32_t(predicate)});
  auto data = BE(payload);
  result.insert(result.end(), data.begin(), data.end());
  return result;
}
struct Fixture {
  std::map<uint32_t, uint32_t> regs, memory;
  std::map<uint32_t, std::vector<std::byte>> indirects;
  std::vector<uint32_t> interrupts;
  unsigned writes = 0, reads = 0, pauses = 0, presents = 0;
  bool stop = false, fail_write = false;
  Services services() {
    Services s;
    s.read_register = [this](uint32_t a, uint32_t& v) { ++reads; v = regs[a]; return true; };
    s.write_register = [this](uint32_t a, uint32_t v) {
      if (fail_write) return false;
      ++writes; regs[a] = v; return true;
    };
    s.read_memory = [this](uint32_t a, uint32_t& v) { ++reads; v = memory[a]; return true; };
    s.write_memory = [this](uint32_t a, uint32_t v) {
      if (fail_write) return false;
      ++writes; memory[a] = v; return true;
    };
    s.read_indirect = [this](uint32_t a, uint32_t n, std::vector<std::byte>& out) {
      auto it = indirects.find(a);
      if (it == indirects.end()) return false;
      out = it->second;
      return out.size() == size_t(n) * 4;
    };
    s.interrupt = [this](uint32_t cpu) { interrupts.push_back(cpu); return true; };
    s.present = [this](uint32_t a, uint32_t w, uint32_t h) {
      assert(a == 0x1000 && w == 1280 && h == 720); ++presents; return true;
    };
    s.cancelled = [this] { return stop; };
    s.finish_native_work = [] { return PacketResult::kConsumed; }; // Fixture owns no GPU work.
    s.pause_wait = [this] { ++pauses; };
    return s;
  }
};
static PacketResult Run(RingExecutor& executor, const std::vector<std::byte>& bytes) {
  Cursor cursor{bytes, 0, uint32_t(bytes.size() / 4), 0};
  auto result = executor.ProcessNext(cursor);
  assert(cursor.position == (result == PacketResult::kConsumed ? cursor.end : 0));
  return result;
}
static void WaitTests() {
  assert(!CompareWait(0, 1, 1, ~0u));
  for (uint32_t op = 1; op < 7; ++op) {
    const bool expected[] = {false, true, true, false, true, false, false};
    assert(CompareWait(op, 0x102, 3, 0xff) == expected[op]);
  }
  assert(CompareWait(3, 0x1234, 0x34, 0xff));
  assert(!CompareWait(3, 0x1234, 0x1234, 0xff));
  assert(CompareWait(7, 0, 1, 0));
  Fixture f;
  auto s = f.services();
  unsigned reports = 0;
  s.pause_wait = [&] { if (++f.pauses == 1001) f.memory[0x103] = 1; };
  s.report_wait = [&](uint32_t info, uint32_t addr, uint32_t ref, uint32_t mask) {
    assert(info == 0x13 && addr == 0x103 && ref == 1 && mask == ~0u); ++reports;
  };
  RingExecutor e(s);
  assert(Run(e, Packet(PM4_WAIT_REG_MEM, {0x13, 0x103, 1, ~0u, 0x100})) == PacketResult::kConsumed);
  assert(f.pauses == 1001 && reports == 1001 && f.writes == 0);
  f.pauses = 0; f.memory.clear();
  s.pause_wait = [&] { if (++f.pauses == 1001) f.stop = true; };
  RingExecutor cancelled(s);
  assert(Run(cancelled, Packet(PM4_WAIT_REG_MEM, {0x13, 0x103, 1, ~0u, 0x100})) == PacketResult::kCancelled);
  assert(f.pauses == 1001 && f.memory.at(0x103) == 0 && cancelled.counters().packets == 0);
  f.stop = false; f.regs[XE_GPU_REG_COHER_STATUS_HOST] = 0x80000000;
  auto coher_services=f.services();
  unsigned dirty_reports=0;
  coher_services.report_wait=[&](uint32_t info,uint32_t address,uint32_t ref,uint32_t mask) {
    assert(info==3 && address==XE_GPU_REG_COHER_STATUS_HOST && ref==0 && mask==~0u);
    ++dirty_reports;
  };
  RingExecutor coher(coher_services);
  assert(Run(coher, Packet(PM4_WAIT_REG_MEM, {3, XE_GPU_REG_COHER_STATUS_HOST, 0, ~0u, 0})) == PacketResult::kBlocked);
  assert(f.regs[XE_GPU_REG_COHER_STATUS_HOST] == 0x80000000 && f.writes == 0);
  assert(dirty_reports==1);
  auto startup_services=f.services();
  unsigned coher_probes=0;
  startup_services.startup_vertex_coherence=[&](uint32_t status,uint32_t base,uint32_t size) {
    assert(status==0x81000000 && base==0x1f4d0000 && size==0x700000);
    if (++coher_probes==1) return false;
    f.regs[XE_GPU_REG_COHER_STATUS_HOST]=0;return true;
  };
  f.regs[XE_GPU_REG_COHER_STATUS_HOST]=0x81000000;
  f.regs[XE_GPU_REG_COHER_BASE_HOST]=0x1f4d0000;
  f.regs[XE_GPU_REG_COHER_SIZE_HOST]=0x700000;
  RingExecutor startup(startup_services);
  auto startup_packet=Packet(PM4_WAIT_REG_MEM,{3,XE_GPU_REG_COHER_STATUS_HOST,0,0x80000000,0});
  Cursor startup_cursor{startup_packet,0,uint32_t(startup_packet.size()/4),0};
  assert(startup.ProcessNext(startup_cursor)==PacketResult::kBlocked && startup_cursor.position==0);
  assert(f.regs[XE_GPU_REG_COHER_STATUS_HOST]==0x81000000);
  assert(startup.ProcessNext(startup_cursor)==PacketResult::kConsumed && coher_probes==2);
  f.regs[XE_GPU_REG_COHER_STATUS_HOST]=0x80010000; // A destination write is not a startup VC invalidate.
  assert(Run(startup,startup_packet)==PacketResult::kBlocked && coher_probes==2);
  // Complete the suspended wait before publishing another packet to this executor.
  f.regs[XE_GPU_REG_COHER_STATUS_HOST]=0x81000000;
  assert(Run(startup,startup_packet)==PacketResult::kConsumed && coher_probes==3);
  assert(Run(startup,Packet(PM4_DRAW_INDX_2,{0x30088}))==PacketResult::kConsumed);
  f.regs[XE_GPU_REG_COHER_STATUS_HOST]=0x81000000;
  assert(Run(startup,startup_packet)==PacketResult::kBlocked && coher_probes==3);
}
static void EffectTests() {
  Fixture f; RingExecutor e(f.services());
  auto consumed = [&](uint32_t op, std::initializer_list<uint32_t> p) {
    auto result = Run(e, Packet(op, p));
    if (result != PacketResult::kConsumed) std::fprintf(stderr, "opcode %02x returned %u\n", op, unsigned(result));
    assert(result == PacketResult::kConsumed);
  };
  assert(Run(e, BE({0x00018020, 3, 4})) == PacketResult::kConsumed);
  assert(f.regs[0x20] == 4);
  assert(Run(e, BE({0x40001001, 8, 9})) == PacketResult::kConsumed);
  assert(f.regs[1] == 8 && f.regs[2] == 9);
  f.regs[4] = 0xff; f.regs[5] = 0x0f; f.regs[6] = 0x80;
  consumed(PM4_REG_RMW, {0xc0000004, 5, 6}); assert(f.regs[4] == 0x8f);
  consumed(PM4_REG_RMW, {4, 0xf, 0x20}); assert(f.regs[4] == 0x2f);
  consumed(PM4_REG_TO_MEM, {4, 0x103}); assert(f.memory[0x103] == 0x2f);
  consumed(PM4_MEM_WRITE, {0x201, 0x12345678, 0xabcdef01});
  assert(f.memory[0x201] == 0x12345678 && f.memory[0x205] == 0xabcdef01);
  consumed(PM4_MEM_WRITE, {0x200});
  consumed(PM4_COND_WRITE, {0x113, 0x103, 0x2f, ~0u, 0x302, 99}); assert(f.memory[0x302] == 99);
  consumed(PM4_COND_WRITE, {3, 4, 0x2f, ~0u, 7, 88}); assert(f.regs[7] == 88);
  consumed(PM4_COND_WRITE, {0, 4, 0x2f, ~0u, 7, 0}); assert(f.regs[7] == 88);
  consumed(PM4_EVENT_WRITE, {0x43}); assert(f.regs[XE_GPU_REG_VGT_EVENT_INITIATOR] == 3);
  consumed(PM4_EVENT_WRITE_SHD, {0x44, 0x402, 22}); assert(f.memory[0x402] == 22);
  const uint32_t bases[] = {XE_GPU_REG_SHADER_CONSTANT_000_X, XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0,
    XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031, XE_GPU_REG_SHADER_CONSTANT_LOOP_00, XE_GPU_REG_RB_SURFACE_INFO};
  // Register constants use the SDK's register window base (RB_SURFACE_INFO is offset zero).
  for (uint32_t type = 0; type != 5; ++type) {
    consumed(PM4_SET_CONSTANT, {(type << 16) | 1, 0x1234});
    assert(f.regs[bases[type] + 1] == 0x1234);
  }
  consumed(PM4_SET_CONSTANT2, {0x123, 7}); assert(f.regs[0x123] == 7);
  consumed(PM4_SET_SHADER_CONSTANTS, {0x124, 8}); assert(f.regs[0x124] == 8);
  f.memory[0x502] = 77;
  consumed(PM4_LOAD_ALU_CONSTANT, {0x500, 2, 1}); assert(f.regs[XE_GPU_REG_SHADER_CONSTANT_000_X + 2] == 77);
  consumed(PM4_INTERRUPT, {0x25}); assert((f.interrupts == std::vector<uint32_t>{0, 2, 5}));
  consumed(PM4_XE_SWAP, {0x53574150, 0x1000, 1280, 720}); assert(f.presents == 1);
  consumed(PM4_DRAW_INDX_2, {2u << 6}); assert(e.counters().draws_omitted == 1);
  consumed(PM4_DRAW_INDX, {0, 0, 0x1000, 10}); assert(e.counters().draws_omitted == 2);
  f.regs[XE_GPU_REG_RB_MODECONTROL] = uint32_t(EdramMode::kCopy);
  RingExecutor copy(f.services());
  assert(Run(copy, Packet(PM4_DRAW_INDX_2, {2u << 6})) == PacketResult::kBlocked);
  f.regs[XE_GPU_REG_RB_MODECONTROL] = 0;
  consumed(PM4_IM_LOAD_IMMEDIATE, {0, 0});
  consumed(PM4_IM_LOAD, {0x600, 0});
  for (auto op : {PM4_NOP, PM4_ME_INIT, PM4_INVALIDATE_STATE, PM4_CONTEXT_UPDATE}) consumed(op, {0});
}
static void BlockedAndMalformedTests() {
  Fixture f;
  auto run = [&](uint32_t op, std::initializer_list<uint32_t> p) {
    RingExecutor e(f.services()); return Run(e, Packet(op, p));
  };
  for (auto op : {PM4_EVENT_WRITE_EXT, PM4_EVENT_WRITE_ZPD, PM4_VIZ_QUERY}) {
    assert(run(op, {0, 0}) == PacketResult::kBlocked);
    assert(f.writes == 0);
  }
  auto without_completion=f.services();without_completion.finish_native_work={};
  RingExecutor no_fence(without_completion),no_idle(without_completion);
  assert(Run(no_fence,Packet(PM4_EVENT_WRITE_SHD,{0x80000000,0x400,22}))==PacketResult::kBlocked);
  assert(Run(no_idle,Packet(PM4_WAIT_FOR_IDLE,{0}))==PacketResult::kBlocked);
  assert(run(PM4_EVENT_WRITE, {0, 0x100}) == PacketResult::kBlocked);
  RingExecutor e(f.services());
  assert(Run(e, Packet(0x7f, {0})) == PacketResult::kBlocked);
  assert(e.counters().last_blocked_opcode == 0x7f && e.counters().last_blocked_address == 0);
  for (auto [op, n] : std::initializer_list<std::pair<uint32_t, unsigned>>{
      {PM4_REG_RMW, 3}, {PM4_REG_TO_MEM, 2}, {PM4_COND_WRITE, 6},
      {PM4_WAIT_REG_MEM, 5}, {PM4_EVENT_WRITE_SHD, 3}, {PM4_EVENT_WRITE_EXT, 2},
      {PM4_INDIRECT_BUFFER, 2}, {PM4_INDIRECT_BUFFER_PFD, 2}, {PM4_SET_BIN_MASK, 2},
      {PM4_SET_BIN_SELECT, 2}, {PM4_LOAD_ALU_CONSTANT, 3}, {PM4_IM_LOAD, 2},
      {PM4_IM_LOAD_IMMEDIATE, 2}, {PM4_XE_SWAP, 4}, {PM4_DRAW_INDX, 2}}) {
    auto data = Packet(op, {0});
    if (n > 2) {
      data = BE({0xc0000000u | ((n - 2) << 16) | (op << 8)});
      auto padding = BE({0});
      for (unsigned i = 0; i != n - 1; ++i) data.insert(data.end(), padding.begin(), padding.end());
    }
    RingExecutor malformed(f.services());
    assert(Run(malformed, data) == PacketResult::kInvalid);
  }
  assert(run(PM4_DRAW_INDX_2, {0}) == PacketResult::kInvalid);
  assert(run(PM4_IM_LOAD_IMMEDIATE, {0, 2, 1}) == PacketResult::kInvalid);
  assert(run(PM4_IM_LOAD, {2, 0}) == PacketResult::kInvalid);
  assert(run(PM4_IM_LOAD, {0, 0x10000}) == PacketResult::kInvalid);
  assert(run(PM4_MEM_WRITE, {0x1ffffffd, 1, 2}) == PacketResult::kInvalid);
  assert(f.writes == 0);
}
static void PredicateTests() {
  Fixture f; RingExecutor e(f.services());
  for (auto op : {PM4_SET_BIN_MASK_LO, PM4_SET_BIN_MASK_HI, PM4_SET_BIN_SELECT_LO, PM4_SET_BIN_SELECT_HI})
    assert(Run(e, Packet(op, {0})) == PacketResult::kConsumed);
  assert(Run(e, Packet(PM4_MEM_WRITE, {0x100, 1}, true)) == PacketResult::kConsumed);
  assert(f.writes == 0);
  assert(Run(e, Packet(PM4_SET_BIN_MASK, {1, 0})) == PacketResult::kConsumed);
  assert(Run(e, Packet(PM4_SET_BIN_SELECT, {1, 0})) == PacketResult::kConsumed);
  assert(Run(e, Packet(PM4_MEM_WRITE, {0x100, 1}, true)) == PacketResult::kConsumed);
  assert(f.writes == 1);
  assert(Run(e, Packet(PM4_XE_SWAP, {0x53574150, 0x1000, 1280, 720}, true)) == PacketResult::kConsumed);
  assert(f.presents == 0);
  auto set = [&](uint32_t op, std::initializer_list<uint32_t> values) {
    assert(Run(e, Packet(op, values)) == PacketResult::kConsumed);
  };
  auto predicate = [&](bool passes) {
    const auto before = f.writes;
    assert(Run(e, Packet(PM4_MEM_WRITE, {0x100, 1}, true)) == PacketResult::kConsumed);
    assert(f.writes == before + unsigned(passes));
  };
  // Cross combined and split forms with asymmetric halves. A swapped word order
  // or a split write that destroys the other half changes observable effects.
  set(PM4_SET_BIN_MASK, {2, 1});
  set(PM4_SET_BIN_SELECT_LO, {1}); set(PM4_SET_BIN_SELECT_HI, {4}); predicate(true);
  set(PM4_SET_BIN_SELECT_LO, {0}); predicate(false);
  set(PM4_SET_BIN_SELECT_HI, {2}); predicate(true);
  set(PM4_SET_BIN_MASK_LO, {0}); predicate(true);
  set(PM4_SET_BIN_MASK_HI, {0}); predicate(false);
  set(PM4_SET_BIN_MASK_HI, {4}); set(PM4_SET_BIN_MASK_LO, {8});
  set(PM4_SET_BIN_SELECT, {16, 8}); predicate(true);
  set(PM4_SET_BIN_MASK_LO, {0}); predicate(false);
  set(PM4_SET_BIN_MASK_HI, {16}); predicate(true);
}
static void DrawModeTests() {
  for (uint32_t mode = 0; mode != 8; ++mode) {
    for (bool loaded : {false, true}) {
      for (bool dma : {false, true}) {
        Fixture f; auto services = f.services();
        unsigned proofs = 0;
        services.shader_is_memory_safe = [&](uint32_t, std::span<const uint32_t>) {
          ++proofs; return true;
        };
        RingExecutor e(services);
        if (loaded) assert(Run(e, Packet(PM4_IM_LOAD_IMMEDIATE, {0, 0})) == PacketResult::kConsumed);
        f.regs[XE_GPU_REG_RB_MODECONTROL] = 0x100 | mode;  // Unrelated bits do not change the mode.
        const bool ordinary = mode == uint32_t(EdramMode::kNoOperation) ||
                              mode == uint32_t(EdramMode::kColorDepth) ||
                              mode == uint32_t(EdramMode::kDepthOnly);
        auto draw = dma ? Packet(PM4_DRAW_INDX, {0, 0, 0x1000, 10}) :
                          Packet(PM4_DRAW_INDX_2, {2u << 6});
        Cursor cursor{draw, 0, uint32_t(draw.size() / 4), 0};
        assert(e.ProcessNext(cursor) == (ordinary ? PacketResult::kConsumed : PacketResult::kBlocked));
        assert(cursor.position == (ordinary ? cursor.end : 0));
        assert(f.writes == (ordinary ? (dma ? 3u : 1u) : 0u));
        assert(e.counters().draws_omitted == unsigned(ordinary));
        assert(proofs == unsigned(ordinary && loaded));
      }
    }
  }
}
static void IndirectTests() {
  Fixture f;
  f.indirects[0x1000] = Packet(PM4_MEM_WRITE, {0x103, 42});
  auto tail = Packet(PM4_DRAW_INDX_2, {2u << 6});
  f.indirects[0x1000].insert(f.indirects[0x1000].end(), tail.begin(), tail.end());
  auto root = Packet(PM4_INDIRECT_BUFFER, {0x1000, 5});
  Cursor cursor{root, 0, uint32_t(root.size() / 4), 0};
  f.regs[XE_GPU_REG_RB_MODECONTROL] = uint32_t(EdramMode::kCopy);
  RingExecutor e(f.services());
  assert(e.ProcessNext(cursor) == PacketResult::kBlocked && cursor.position == 0);
  assert(e.ProcessNext(cursor) == PacketResult::kBlocked && cursor.position == 0);
  assert(f.writes == 1 && f.memory[0x103] == 42 && e.counters().indirects == 1);
  f.regs[XE_GPU_REG_RB_MODECONTROL] = 0;
  assert(e.ProcessNext(cursor) == PacketResult::kConsumed && cursor.position == cursor.end);
  assert(f.writes == 2 && e.counters().packets == 3 && e.counters().draws_omitted == 1);
  f.indirects[0x1000] = Packet(PM4_INDIRECT_BUFFER, {0x2000, 3});
  f.indirects[0x2000] = Packet(PM4_INDIRECT_BUFFER, {0x1000, 3});
  RingExecutor cycle(f.services());
  assert(Run(cycle, Packet(PM4_INDIRECT_BUFFER, {0x1000, 3})) == PacketResult::kInvalid);
  for (unsigned i = 1; i <= 5; ++i) f.indirects[i * 0x1000] = Packet(PM4_INDIRECT_BUFFER, {(i + 1) * 0x1000, 3});
  RingExecutor depth(f.services());
  assert(Run(depth, Packet(PM4_INDIRECT_BUFFER, {0x1000, 3})) == PacketResult::kInvalid);
  f.indirects[0x7000] = BE({0xc0011000, 0});
  RingExecutor truncated(f.services());
  assert(Run(truncated, Packet(PM4_INDIRECT_BUFFER, {0x7000, 2})) == PacketResult::kInvalid);
  for (auto [address, words] : {std::pair(0x1ffffffcu, 2u), std::pair(0x7001u, 2u), std::pair(0x7000u, 0x100000u)}) {
    RingExecutor range(f.services());
    assert(Run(range, Packet(PM4_INDIRECT_BUFFER, {address, words})) == PacketResult::kInvalid);
  }
  // Four active frames are legal. Completion returns each parent exactly once.
  f.indirects[0x4000] = Packet(PM4_NOP, {0});
  f.indirects[0x3000] = Packet(PM4_INDIRECT_BUFFER_PFD, {0x4000, 2});
  f.indirects[0x2000] = Packet(PM4_INDIRECT_BUFFER, {0x3000, 3});
  f.indirects[0x1000] = Packet(PM4_INDIRECT_BUFFER, {0x2000, 3});
  RingExecutor legal(f.services());
  assert(Run(legal, Packet(PM4_INDIRECT_BUFFER, {0xa0001000, 3})) == PacketResult::kConsumed);
  assert(legal.counters().indirects == 4 && legal.counters().packets == 5);
  // A successful callback with an incorrect byte count is malformed, not complete.
  auto services = f.services();
  services.read_indirect = [](uint32_t, uint32_t, std::vector<std::byte>& out) { out = BE({0}); return true; };
  RingExecutor short_read(services);
  assert(Run(short_read, Packet(PM4_INDIRECT_BUFFER, {0x7000, 2})) == PacketResult::kInvalid);
  // Never publish an indirect parent when cancellation interrupts a later wait.
  f.indirects[0x1000] = Packet(PM4_MEM_WRITE, {0x104, 11});
  auto wait = Packet(PM4_WAIT_REG_MEM, {3, 10, 1, ~0u, 0});
  f.indirects[0x1000].insert(f.indirects[0x1000].end(), wait.begin(), wait.end());
  services = f.services(); services.pause_wait = [&] { f.stop = true; };
  auto indirect = Packet(PM4_INDIRECT_BUFFER, {0x1000, 9});
  Cursor cancelled_cursor{indirect, 0, uint32_t(indirect.size() / 4), 0};
  RingExecutor cancelled(services);
  assert(cancelled.ProcessNext(cancelled_cursor) == PacketResult::kCancelled && cancelled_cursor.position == 0);
  const auto writes_before_resume = f.writes;
  f.stop = false; f.regs[10] = 1;
  assert(cancelled.ProcessNext(cancelled_cursor) == PacketResult::kConsumed);
  assert(f.writes == writes_before_resume && f.memory[0x104] == 11);
}
static void ShaderSafetyTests() {
  auto load = Packet(PM4_IM_LOAD_IMMEDIATE, {0, 3, 0x11223344, 0x55667788, 0x99aabbcc});
  auto draw = Packet(PM4_DRAW_INDX_2, {2u << 6});
  for (int safety : {-1, 0, 1}) {
    Fixture f; auto services = f.services();
    unsigned calls = 0;
    if (safety != -1) services.shader_is_memory_safe = [&](uint32_t stage, std::span<const uint32_t> code) {
      ++calls; assert(stage == 0 && code.size() == 3 && code[0] == 0x11223344 && code[2] == 0x99aabbcc);
      return safety == 1;
    };
    RingExecutor e(services);
    assert(Run(e, load) == PacketResult::kConsumed);
    assert(Run(e, draw) == (safety == 1 ? PacketResult::kConsumed : PacketResult::kBlocked));
    assert(e.counters().draws_omitted == unsigned(safety == 1));
    assert(calls == unsigned(safety != -1));
  }
  Fixture f; auto services = f.services();
  f.memory[0x602] = 0x10203040; f.memory[0x606] = 0x50607080;
  services.shader_is_memory_safe = [](uint32_t stage, std::span<const uint32_t> code) {
    assert(stage == 1 && code.size() == 2 && code[0] == 0x10203040 && code[1] == 0x50607080); return true;
  };
  RingExecutor pointer(services);
  assert(Run(pointer, Packet(PM4_IM_LOAD, {0x601, 2})) == PacketResult::kConsumed);
  assert(Run(pointer, draw) == PacketResult::kConsumed);
  f.regs[XE_GPU_REG_RB_MODECONTROL] = uint32_t(EdramMode::kCopy);
  assert(Run(pointer, draw) == PacketResult::kBlocked);
}
static void ReplayTests() {
  // A later failed memory write cannot replay the earlier committed write.
  Fixture f; auto s = f.services();
  s.write_memory = [&](uint32_t a, uint32_t v) {
    if (a == 0x104 && !f.fail_write) return false;
    ++f.writes; f.memory[a] = v; return true;
  };
  auto packet = Packet(PM4_MEM_WRITE, {0x100, 1, 2});
  Cursor c{packet, 0, uint32_t(packet.size() / 4), 0};
  RingExecutor e(s);
  assert(e.ProcessNext(c) == PacketResult::kBlocked && c.position == 0 && f.writes == 1);
  assert(e.ProcessNext(c) == PacketResult::kBlocked && c.position == 0 && f.writes == 1);
  f.fail_write = true;
  assert(e.ProcessNext(c) == PacketResult::kConsumed && f.writes == 2);
  // SHD register write and each interrupt destination are also committed once.
  f.writes = 0; f.fail_write = false; packet = Packet(PM4_EVENT_WRITE_SHD, {2, 0x104, 55});
  c = {packet, 0, uint32_t(packet.size() / 4), 0}; RingExecutor shd(s);
  assert(shd.ProcessNext(c) == PacketResult::kBlocked && f.writes == 1);
  f.fail_write = true;
  assert(shd.ProcessNext(c) == PacketResult::kConsumed && f.writes == 2 && f.memory[0x104] == 55);
  bool ready = false;
  s.interrupt = [&](uint32_t cpu) {
    if (cpu == 2 && !ready) return false;
    f.interrupts.push_back(cpu); return true;
  };
  packet = Packet(PM4_INTERRUPT, {5}); c = {packet, 0, uint32_t(packet.size() / 4), 0};
  RingExecutor interrupt(s);
  assert(interrupt.ProcessNext(c) == PacketResult::kBlocked);
  assert(interrupt.ProcessNext(c) == PacketResult::kBlocked && f.interrupts.size() == 1);
  ready = true; assert(interrupt.ProcessNext(c) == PacketResult::kConsumed);
  assert((f.interrupts == std::vector<uint32_t>{0, 2}) && interrupt.counters().interrupts == 2);
  // Snapshot RMW input once: a suspended write does not reevaluate changing inputs.
  f.regs[4] = 1; f.fail_write = true;
  packet = Packet(PM4_REG_RMW, {4, ~0u, 2}); c = {packet, 0, uint32_t(packet.size() / 4), 0};
  RingExecutor rmw(f.services()); assert(rmw.ProcessNext(c) == PacketResult::kBlocked);
  f.regs[4] = 4; f.fail_write = false;
  assert(rmw.ProcessNext(c) == PacketResult::kConsumed && f.regs[4] == 3);
  // COND_WRITE similarly latches the initial condition across a failed effect.
  f.regs[4] = 3; f.fail_write = true;
  packet = Packet(PM4_COND_WRITE, {3, 4, 3, ~0u, 5, 9}); c = {packet, 0, uint32_t(packet.size() / 4), 0};
  RingExecutor conditional(f.services()); assert(conditional.ProcessNext(c) == PacketResult::kBlocked);
  f.regs[4] = 0; f.fail_write = false;
  assert(conditional.ProcessNext(c) == PacketResult::kConsumed && f.regs[5] == 9);
  // No failed presentation is counted as displayed or sent twice as a request.
  bool present_ok = false; unsigned attempts = 0;
  s = f.services(); s.present = [&](uint32_t, uint32_t, uint32_t) { ++attempts; return present_ok; };
  packet = Packet(PM4_XE_SWAP, {kSwapSignature, 0x1000, 1280, 720}); c = {packet, 0, uint32_t(packet.size() / 4), 0};
  RingExecutor swap(s);
  assert(swap.ProcessNext(c) == PacketResult::kBlocked && swap.counters().swap_requests == 1);
  assert(swap.ProcessNext(c) == PacketResult::kBlocked && swap.counters().refresh_completed == 0);
  present_ok = true;
  assert(swap.ProcessNext(c) == PacketResult::kConsumed && swap.counters().swap_requests == 1 && attempts == 3);
  assert(swap.counters().refresh_completed == 1);  // Only the completed refresh is counted.
  // New ring generation must never inherit partial writes from an old packet.
  f.fail_write = true;
  auto old = Packet(PM4_MEM_WRITE, {0x100, 1}), replacement = old;
  Cursor old_cursor{old, 0, uint32_t(old.size() / 4), 0}; RingExecutor generation(f.services());
  assert(generation.ProcessNext(old_cursor) == PacketResult::kBlocked);
  Cursor replacement_cursor{replacement, 0, uint32_t(replacement.size() / 4), 0};
  assert(generation.ProcessNext(replacement_cursor) == PacketResult::kInvalid && replacement_cursor.position == 0);
}
static void NativeFenceTests() {
  Fixture missing;auto absent=missing.services();absent.finish_native_work={};
  RingExecutor no_completion(absent);
  assert(Run(no_completion,Packet(PM4_EVENT_WRITE_SHD,{2,0x402,7}))==PacketResult::kBlocked);
  assert(missing.writes==0);
  Fixture f;auto services=f.services();
  bool ready=false,memory_ready=false;unsigned probes=0;
  services.finish_native_work=[&] {++probes;return ready?PacketResult::kConsumed:PacketResult::kBlocked;};
  services.write_memory=[&](uint32_t address,uint32_t value) {
    if(!memory_ready) return false;
    ++f.writes;f.memory[address]=value;return true;
  };
  RingExecutor executor(services);
  auto packet=Packet(PM4_EVENT_WRITE_SHD,{0x80000016,0x402,0xdeadbeef});
  Cursor cursor{packet,0,uint32_t(packet.size()/4),0};
  assert(executor.ProcessNext(cursor)==PacketResult::kBlocked && f.writes==0 && cursor.position==0);
  ready=true;
  assert(executor.ProcessNext(cursor)==PacketResult::kBlocked && f.writes==1 && cursor.position==0);
  memory_ready=true;
  assert(executor.ProcessNext(cursor)==PacketResult::kConsumed && f.writes==2 && probes==2);
  assert(f.memory[0x402]==0); // No completed swap, so the actual SDK swap counter is zero.
  assert(Run(executor,Packet(PM4_XE_SWAP,{kSwapSignature,0x1000,1280,720}))==PacketResult::kConsumed);
  assert(Run(executor,packet)==PacketResult::kConsumed && f.memory[0x402]==1);
  auto idle=Packet(PM4_WAIT_FOR_IDLE,{0});
  ready=false;
  assert(Run(executor,idle)==PacketResult::kBlocked);
  ready=true;
  assert(Run(executor,idle)==PacketResult::kConsumed);
}
static void NativeAssociationTests() {
  Fixture f;auto services=f.services();
  bool published=false;unsigned calls=0;std::vector<PacketStamp> observed;
  services.packet_site=[](uint32_t address,uint32_t,PacketSite& site) {
    site={7,address};return true;
  };
  services.native_packet=[&](const PacketStamp& stamp,bool execute) {
    ++calls;observed.push_back(stamp);assert(execute);
    return published?PacketResult::kConsumed:PacketResult::kBlocked;
  };
  auto draw=Packet(PM4_DRAW_INDX_2,{0x30088});
  Cursor root{draw,0,uint32_t(draw.size()/4),0,0x1000};
  RingExecutor executor(services);
  assert(executor.ProcessNext(root)==PacketResult::kBlocked && root.position==0 && f.writes==0);
  published=true;
  assert(executor.ProcessNext(root)==PacketResult::kConsumed && calls==2);
  assert(observed[0]==observed[1] && observed[0].site==PacketSite(7,0x1000));
  assert(observed[0].payload==std::vector<uint32_t>{0x30088});
  assert(executor.counters().draws_omitted==0);
  services.native_packet=[&](const PacketStamp&,bool execute) {assert(!execute);return PacketResult::kConsumed;};
  RingExecutor predicate(services);
  assert(Run(predicate,Packet(PM4_SET_BIN_MASK,{0,0}))==PacketResult::kConsumed);
  assert(Run(predicate,Packet(PM4_DRAW_INDX_2,{0x30088},true))==PacketResult::kConsumed);
  assert(predicate.counters().draws_omitted==0);
  f.indirects[0x2000]=draw;
  services.native_packet=[&](const PacketStamp& stamp,bool execute) {
    assert(execute && stamp.site==PacketSite(7,0x2000));return PacketResult::kConsumed;
  };
  RingExecutor indirect(services);
  assert(Run(indirect,Packet(PM4_INDIRECT_BUFFER,{0x2000,2}))==PacketResult::kConsumed);
  assert(indirect.counters().draws_omitted==0);
  // Once the token is accepted, a failed SDK register write cannot enqueue it twice.
  calls=0;services.native_packet=[&](const PacketStamp&,bool) {++calls;return PacketResult::kConsumed;};
  RingExecutor retry(services);Cursor retry_cursor{draw,0,2,0,0x1000};
  f.fail_write=true;
  assert(retry.ProcessNext(retry_cursor)==PacketResult::kBlocked && calls==1);
  f.fail_write=false;
  assert(retry.ProcessNext(retry_cursor)==PacketResult::kConsumed && calls==1);
  auto missing=services;missing.packet_site={};
  RingExecutor unavailable(missing);
  assert(Run(unavailable,draw)==PacketResult::kBlocked && calls==1);
  // Detect allocation replacement while an indirect snapshot is being copied.
  uint64_t epoch=7;services.packet_site=[&](uint32_t address,uint32_t,PacketSite& site) {
    site={epoch,address};return true;
  };
  auto copy=services.read_indirect;
  services.read_indirect=[&](uint32_t address,uint32_t words,std::vector<std::byte>& out) {
    const bool result=copy(address,words,out);++epoch;return result;
  };
  RingExecutor reused(services);auto indirect_packet=Packet(PM4_INDIRECT_BUFFER,{0x2000,2});
  assert(Run(reused,indirect_packet)==PacketResult::kInvalid && calls==1);
  assert(Run(reused,indirect_packet)==PacketResult::kInvalid && calls==1);
}
static void ReadOnlyAndCapacityTests() {
#if defined(__linux__)
  {
  const size_t page = size_t(sysconf(_SC_PAGESIZE));
  void* mapping = mmap(nullptr, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(mapping != MAP_FAILED);
  auto bytes = BE({0x80000000});
  auto* data = static_cast<std::byte*>(mapping) + page - 4;
  std::copy(bytes.begin(), bytes.end(), data);
  assert(mprotect(mapping, page, PROT_READ) == 0);
  assert(mprotect(static_cast<std::byte*>(mapping) + page, page, PROT_NONE) == 0);
  Cursor c{std::span<const std::byte>(data, 4), 0, 1, 0};
  PacketView p;
  assert(PeekPacket(c, p) == PacketResult::kConsumed && c.position == 0);
  Fixture f; RingExecutor e(f.services());
  assert(e.ProcessNext(c) == PacketResult::kConsumed && c.position == 1);
  assert(munmap(mapping, page * 2) == 0);
  }
#endif
  Fixture f; RingExecutor e(f.services());
  auto bytes2 = BE({0x100, 55, 0, 0xc0013d00});
  Cursor c2{bytes2, 3, 1, 3};
  assert(e.ProcessNext(c2) == PacketResult::kIncomplete && c2.position == 3 && f.writes == 0);
  c2.end = 2;
  assert(e.ProcessNext(c2) == PacketResult::kConsumed && c2.position == 2 && f.memory[0x100] == 55);
}
static void DefensiveTests() {
  Fixture f;
  auto consumed = [&](uint32_t op, std::initializer_list<uint32_t> values) {
    RingExecutor e(f.services()); assert(Run(e, Packet(op, values)) == PacketResult::kConsumed);
  };
  // Single-word handlers cannot encode a zero-length payload; truncated transport
  // is how their smaller-than-minimum case reaches the parser.
  for (auto op : {PM4_INTERRUPT, PM4_MEM_WRITE, PM4_EVENT_WRITE, PM4_EVENT_WRITE_ZPD, PM4_VIZ_QUERY,
       PM4_SET_CONSTANT, PM4_SET_CONSTANT2, PM4_SET_SHADER_CONSTANTS, PM4_SET_BIN_MASK_LO,
       PM4_SET_BIN_MASK_HI, PM4_SET_BIN_SELECT_LO, PM4_SET_BIN_SELECT_HI, PM4_DRAW_INDX_2,
       PM4_NOP, PM4_ME_INIT, PM4_INVALIDATE_STATE, PM4_CONTEXT_UPDATE, PM4_WAIT_FOR_IDLE}) {
    RingExecutor e(f.services());
    assert(Run(e, BE({0xc0000000u | (uint32_t(op) << 8)})) == PacketResult::kInvalid);
  }
  for (auto op : {PM4_SET_CONSTANT, PM4_SET_CONSTANT2, PM4_SET_SHADER_CONSTANTS}) consumed(op, {0});
  consumed(PM4_LOAD_ALU_CONSTANT, {0, 0, 0});
  consumed(PM4_DRAW_INDX, {0, 2u << 6});
  f.indirects[0] = {}; consumed(PM4_INDIRECT_BUFFER, {0, 0});
  RingExecutor unknown_predicate(f.services());
  assert(Run(unknown_predicate, Packet(PM4_SET_BIN_MASK, {0, 0})) == PacketResult::kConsumed);
  assert(Run(unknown_predicate, Packet(0x7f, {0}, true)) == PacketResult::kConsumed);
  assert(f.writes == 1);  // Only the preceding regular draw wrote its initiator.
  for (const auto& data : {Packet(PM4_DRAW_INDX_2, {3u << 6}), Packet(PM4_SET_CONSTANT, {5u << 16}),
      Packet(PM4_LOAD_ALU_CONSTANT, {1, 0, 1}), Packet(PM4_XE_SWAP, {0, 0, 0, 0})}) {
    RingExecutor e(f.services()); assert(Run(e, data) == PacketResult::kInvalid);
  }
  RingExecutor immediate(f.services());
  assert(Run(immediate, Packet(PM4_DRAW_INDX_2, {1u << 6})) == PacketResult::kBlocked);
  RingExecutor conditional_draw(f.services());
  assert(Run(conditional_draw, Packet(PM4_DRAW_INDX, {0x100, 2u << 6})) == PacketResult::kBlocked);
  RingExecutor empty_services({});
  assert(Run(empty_services, Packet(PM4_MEM_WRITE, {0x100, 1})) == PacketResult::kBlocked);
  auto s = f.services(); s.pause_wait = {};
  RingExecutor no_pause(s);
  assert(Run(no_pause, Packet(PM4_WAIT_REG_MEM, {3, 10, 1, ~0u, 0})) == PacketResult::kBlocked);
  f.stop = true; RingExecutor stopped(f.services());
  assert(Run(stopped, Packet(PM4_MEM_WRITE, {0x100, 1})) == PacketResult::kCancelled);
}
static void RefreshCounterTests() {
  RingCounters counters;
  RecordRefresh(false, counters);
  assert(counters.refresh_completed == 0);
  RecordRefresh(true, counters);
  assert(counters.refresh_completed == 1);
  RecordRefresh(false, counters);
  assert(counters.refresh_completed == 1 && counters.swap_requests == 0);
}
int main() {
  RefreshCounterTests();
  ParserTests(); WaitTests(); EffectTests(); BlockedAndMalformedTests(); PredicateTests(); IndirectTests();
  ShaderSafetyTests(); ReplayTests(); ReadOnlyAndCapacityTests();
  DefensiveTests();
  DrawModeTests(); NativeFenceTests(); NativeAssociationTests();
}
