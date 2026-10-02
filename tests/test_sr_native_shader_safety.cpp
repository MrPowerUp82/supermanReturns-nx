#include "sr_native_shader_safety.h"

#include <cassert>
#include <cstdint>
#include <vector>

using namespace sr::native;

namespace {
constexpr uint32_t kNop = 0, kExec = 1, kExecEnd = 2, kCondExec = 3, kCondExecEnd = 4, kLoopStart = 7, kLoopEnd = 8,
                   kCondCall = 9, kReturn = 10, kCondJmp = 11, kAlloc = 12, kMarkVsFetchDone = 15;

// One control-flow instruction: 32 + 16 bits.
struct Cf {
  uint32_t d0 = 0, d1 = 0;
};
Cf Opcode(uint32_t opcode, uint32_t d0 = 0, uint32_t d1_low = 0) { return {d0, d1_low | (opcode << 12)}; }
Cf Exec(uint32_t opcode, uint32_t address, uint32_t count, uint32_t sequence = 0) {
  return Opcode(opcode, address | (count << 12) | (sequence << 16));
}
Cf AllocOf(uint32_t type, uint32_t size = 1) { return Opcode(kAlloc, size, type << 9); }

void Pack(std::vector<uint32_t>& code, Cf a, Cf b) {
  code.push_back(a.d0);
  code.push_back((a.d1 & 0xffff) | (b.d0 << 16));
  code.push_back((b.d0 >> 16) | (b.d1 << 16));
}
uint32_t Alu(uint32_t dest, bool export_data, uint32_t vector_mask = 0xf, uint32_t dest_relative = 0) {
  return dest | (dest_relative << 6) | (uint32_t(export_data) << 15) | (vector_mask << 16);
}
void AddInstruction(std::vector<uint32_t>& code, uint32_t w0, uint32_t w1 = 0, uint32_t w2 = 0) {
  code.insert(code.end(), {w0, w1, w2});
}

// Control flow occupies the first instruction slot (two instructions), followed by `instructions`.
std::vector<uint32_t> Program(Cf a, Cf b, std::initializer_list<uint32_t> alu_dests, bool export_data) {
  std::vector<uint32_t> code;
  Pack(code, a, b);
  for (uint32_t dest : alu_dests) AddInstruction(code, Alu(dest, export_data));
  return code;
}
}  // namespace

static void Accepts() {
  // VS position export, ALU clause ending the program.
  assert(ShaderIsMemorySafe(0, Program(Exec(kExecEnd, 1, 1), Opcode(kNop), {62}, true)));
  // VS interpolator and point size.
  assert(ShaderIsMemorySafe(0, Program(Exec(kExecEnd, 1, 2), Opcode(kNop), {5, 63}, true)));
  // PS colour 0 and depth.
  assert(ShaderIsMemorySafe(1, Program(Exec(kExecEnd, 1, 2), Opcode(kNop), {0, 61}, true)));
  // A non-export ALU writes only a temporary register.
  assert(ShaderIsMemorySafe(1, Program(Exec(kExecEnd, 1, 1), Opcode(kNop), {33}, false)));
  // Allocations for position and interpolators.
  assert(ShaderIsMemorySafe(0, Program(AllocOf(1), Exec(kExecEnd, 1, 1), {62}, true)));
  assert(ShaderIsMemorySafe(0, Program(Opcode(kMarkVsFetchDone), Exec(kExecEnd, 1, 1), {62}, true)));
}

static void RejectsMemoryExport() {
  for (uint32_t dest : {32u, 33u, 34u, 35u, 36u, 37u})
    assert(!ShaderIsMemorySafe(0, Program(Exec(kExecEnd, 1, 1), Opcode(kNop), {dest}, true)));
  // Export with a relative destination cannot be proven.
  std::vector<uint32_t> code;
  Pack(code, Exec(kExecEnd, 1, 1), Opcode(kNop));
  AddInstruction(code, Alu(62, true, 0xf, 1));
  assert(!ShaderIsMemorySafe(0, code));
  // A destination that is valid for one stage is not valid for the other.
  assert(!ShaderIsMemorySafe(0, Program(Exec(kExecEnd, 1, 1), Opcode(kNop), {61}, true)));
  assert(!ShaderIsMemorySafe(1, Program(Exec(kExecEnd, 1, 1), Opcode(kNop), {62}, true)));
  // The memory export allocation alone marks the shader as exporting.
  assert(!ShaderIsMemorySafe(0, Program(AllocOf(3), Exec(kExecEnd, 1, 1), {62}, true)));
  // Second exec clause is scanned too, not just the first.
  assert(!ShaderIsMemorySafe(
      0, Program(Exec(kExec, 1, 1), Exec(kExecEnd, 2, 1), {62, 33}, true)));
  // Unknown stages are not provable.
  assert(!ShaderIsMemorySafe(2, Program(Exec(kExecEnd, 1, 1), Opcode(kNop), {62}, true)));
}

static void RejectsMalformed() {
  assert(!ShaderIsMemorySafe(0, {}));
  assert(!ShaderIsMemorySafe(0, std::vector<uint32_t>{1, 2}));
  // Exec clause beyond the code.
  assert(!ShaderIsMemorySafe(0, Program(Exec(kExecEnd, 1, 2), Opcode(kNop), {62}, true)));
  assert(!ShaderIsMemorySafe(0, Program(Exec(kExecEnd, 9, 1), Opcode(kNop), {62}, true)));
  // Instruction data overlapping the control-flow slot.
  assert(!ShaderIsMemorySafe(0, Program(Exec(kExecEnd, 0, 1), Opcode(kNop), {62}, true)));
  // Reserved bits in an exec instruction (the encoding is not understood).
  auto reserved = Exec(kExecEnd, 1, 1);
  reserved.d1 |= 1u << 4;
  assert(!ShaderIsMemorySafe(0, Program(reserved, Opcode(kNop), {62}, true)));
  reserved = Exec(kExecEnd, 1, 1);
  reserved.d1 |= 1u << 10;
  assert(!ShaderIsMemorySafe(0, Program(reserved, Opcode(kNop), {62}, true)));
  // Reserved bits in the alloc instruction.
  auto alloc = AllocOf(1);
  alloc.d0 |= 1u << 20;
  assert(!ShaderIsMemorySafe(0, Program(alloc, Exec(kExecEnd, 1, 1), {62}, true)));
  // A nop with junk is not a nop.
  auto junk = Opcode(kNop, 0x1234);
  assert(!ShaderIsMemorySafe(0, Program(Exec(kExecEnd, 1, 1), junk, {62}, true)));
}

static void ControlFlow() {
  // Unconditional jump back into the control-flow section is bounded.
  assert(ShaderIsMemorySafe(0, Program(Opcode(kCondJmp, 1 | (1u << 13)), Exec(kExecEnd, 1, 1), {62}, true)));
  // Jump into the instruction data is refused.
  assert(!ShaderIsMemorySafe(0, Program(Opcode(kCondJmp, 5 | (1u << 13)), Exec(kExecEnd, 1, 1), {62}, true)));
  assert(!ShaderIsMemorySafe(0, Program(Opcode(kCondCall, 40), Exec(kExecEnd, 1, 1), {62}, true)));
  assert(!ShaderIsMemorySafe(0, Program(Opcode(kLoopStart, 40), Exec(kExecEnd, 1, 1), {62}, true)));
  assert(!ShaderIsMemorySafe(0, Program(Opcode(kReturn, 1), Exec(kExecEnd, 1, 1), {62}, true)));
  assert(ShaderIsMemorySafe(0, Program(Opcode(kReturn), Exec(kExecEnd, 1, 1), {62}, true)));
  // A conditional exec at address 0 overlaps the control flow.
  assert(!ShaderIsMemorySafe(0, Program(Opcode(kCondExec, 1u << 12, 0), Exec(kExecEnd, 1, 1), {62}, true)));
  // Two control-flow slot pairs: the loop targets stay inside the four slots.
  std::vector<uint32_t> code;
  Pack(code, Opcode(kLoopStart, 3), Opcode(kLoopEnd, 0));
  Pack(code, Exec(kExecEnd, 2, 1), Opcode(kNop));
  AddInstruction(code, Alu(62, true));
  assert(ShaderIsMemorySafe(0, code));
  code.clear();
  Pack(code, Opcode(kLoopStart, 4), Opcode(kLoopEnd, 0));
  Pack(code, Exec(kExecEnd, 2, 1), Opcode(kNop));
  AddInstruction(code, Alu(62, true));
  assert(!ShaderIsMemorySafe(0, code));  // Target slot 4 is outside the four control-flow slots.
}

// Execution must not fall off the last scanned control-flow slot into instruction data.
static void RequiresTerminatingExec() {
  const char* reason = nullptr;
  // exec (not End) + nop, then data whose first words decode as a control-flow slot that
  // references a memexport ALU: the old scan accepted this.
  std::vector<uint32_t> code;
  Pack(code, Exec(kExec, 1, 1), Opcode(kNop));
  AddInstruction(code, 0x1002, 0x2000, 0);   // ALU; decodes as `exece addr=2 cnt=1` if fallen into.
  AddInstruction(code, Alu(33, true));       // eM0 export.
  assert(!ShaderIsMemorySafe(0, code, &reason));
  assert(reason && *reason);
  // A conditional end may not end the program either.
  assert(!ShaderIsMemorySafe(0, Program(Exec(kCondExecEnd, 1, 1), Opcode(kNop), {62}, true)));
  // Trailing nops after the final exec end are fine.
  assert(ShaderIsMemorySafe(0, Program(Exec(kExecEnd, 1, 1), Opcode(kNop), {62}, true), &reason));
  assert(!reason || !*reason);
  // Programs with no control flow at all are not provable.
  assert(!ShaderIsMemorySafe(0, Program(Opcode(kNop), Opcode(kNop), {62}, true)));
}

// The compiler's tail `exece` can be empty and point at the end of the program.
static void AcceptsEmptyTailExec() {
  std::vector<uint32_t> code;
  Pack(code, Exec(kExec, 1, 1), Exec(kExecEnd, 2, 0));
  AddInstruction(code, Alu(62, true));
  assert(ShaderIsMemorySafe(0, code));
}

static void FetchClauses() {
  // One ALU instruction, one fetch instruction; sequence bit 2 marks instruction 1 as fetch.
  auto build = [](uint32_t fetch_opcode) {
    std::vector<uint32_t> code;
    Pack(code, Exec(kExecEnd, 1, 2, 0b0100), Opcode(kNop));
    AddInstruction(code, Alu(62, true));
    AddInstruction(code, fetch_opcode);
    return code;
  };
  assert(ShaderIsMemorySafe(0, build(0)));    // Vertex fetch.
  assert(ShaderIsMemorySafe(0, build(1)));    // Texture fetch.
  assert(ShaderIsMemorySafe(0, build(16)));
  assert(!ShaderIsMemorySafe(0, build(2)));   // Unknown opcode.
  assert(!ShaderIsMemorySafe(0, build(31)));
}

static void CacheTests() {
  ShaderSafetyCache cache;
  auto safe = Program(Exec(kExecEnd, 1, 1), Opcode(kNop), {62}, true);
  auto unsafe = Program(Exec(kExecEnd, 1, 1), Opcode(kNop), {33}, true);
  assert(cache.IsSafe(0, safe));
  assert(cache.IsSafe(0, safe));
  assert(!cache.IsSafe(0, unsafe));
  assert(!cache.IsSafe(0, unsafe));
  assert(!cache.IsSafe(1, safe));  // Stage is part of the key.
  assert(cache.size() == 3);

  ShaderSafetyCache verdicts;
  ShaderSafetyCache::Verdict verdict;
  assert(!verdicts.IsSafe(1, unsafe, &verdict));
  assert(verdict.fresh && verdict.reason && *verdict.reason);
  assert(!verdicts.IsSafe(1, unsafe, &verdict));
  assert(!verdict.fresh && verdict.reason && *verdict.reason);  // Stored answer, not new.
  assert(verdicts.IsSafe(0, safe, &verdict));
  assert(verdict.fresh);
}

int main() {
  Accepts();
  RejectsMemoryExport();
  RejectsMalformed();
  ControlFlow();
  FetchClauses();
  RequiresTerminatingExec();
  AcceptsEmptyTailExec();
  CacheTests();
  return 0;
}
