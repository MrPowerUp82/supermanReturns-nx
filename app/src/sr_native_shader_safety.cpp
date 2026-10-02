#include "sr_native_shader_safety.h"

#include <algorithm>
#include <cstring>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include <rex/graphics/format/ucode.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace sr::native {
namespace {
using namespace rex::graphics::ucode;

constexpr uint32_t kMaxInstructions = 4096;  // 12-bit exec addresses.
constexpr uint32_t kMaxExecCount = 6;        // 12 sequence bits, two per instruction.
constexpr uint32_t kMaxCacheEntries = 4096;

// Bits of the 32 + 16 bit control-flow encoding that the SDK leaves unnamed. A program
// that sets one is encoded in a way this proof does not understand.
struct Reserved {
  uint32_t d0 = 0, d1 = 0;
};
bool ReservedFor(ControlFlowOpcode opcode, Reserved& out) {
  switch (opcode) {
    case ControlFlowOpcode::kNop: out = {0xFFFFFFFFu, 0x0FFFu}; return true;
    case ControlFlowOpcode::kExec:
    case ControlFlowOpcode::kExecEnd: out = {0, 0x05FCu}; return true;
    case ControlFlowOpcode::kCondExec:
    case ControlFlowOpcode::kCondExecEnd:
    case ControlFlowOpcode::kCondExecPredClean:
    case ControlFlowOpcode::kCondExecPredCleanEnd: out = {0, 0}; return true;
    case ControlFlowOpcode::kCondExecPred:
    case ControlFlowOpcode::kCondExecPredEnd: out = {0, 0x01FCu}; return true;
    case ControlFlowOpcode::kLoopStart: out = {0xFFE0C000u, 0x07FFu}; return true;
    case ControlFlowOpcode::kLoopEnd: out = {0xFFC0E000u, 0x03FFu}; return true;
    case ControlFlowOpcode::kCondCall: out = {0xFFFF8000u, 0x3u}; return true;
    case ControlFlowOpcode::kReturn: out = {0xFFFFFFFFu, 0x07FFu}; return true;
    case ControlFlowOpcode::kCondJmp: out = {0xFFFF8000u, 0x1u}; return true;
    case ControlFlowOpcode::kAlloc: out = {0xFFFFFFF8u, 0x08FFu}; return true;
    // No effect by definition (a hint that vertex fetches are done) and no layout in the SDK.
    case ControlFlowOpcode::kMarkVsFetchDone: out = {0, 0}; return true;
  }
  return false;
}

struct Clause {
  uint32_t address, count, sequence;
};

bool ExportDestinationAllowed(uint32_t stage, uint32_t dest) {
  if (stage == 0) return dest <= 15 || dest == uint32_t(ExportRegister::kVSPosition) ||
                         dest == uint32_t(ExportRegister::kVSPointSizeEdgeFlagKillVertex);
  return dest <= 3 || dest == uint32_t(ExportRegister::kPSDepth);
}

bool FetchOpcodeKnown(FetchOpcode opcode) {
  switch (opcode) {
    case FetchOpcode::kVertexFetch:
    case FetchOpcode::kTextureFetch:
    case FetchOpcode::kGetTextureBorderColorFrac:
    case FetchOpcode::kGetTextureComputedLod:
    case FetchOpcode::kGetTextureGradients:
    case FetchOpcode::kGetTextureWeights:
    case FetchOpcode::kSetTextureLod:
    case FetchOpcode::kSetTextureGradientsHorz:
    case FetchOpcode::kSetTextureGradientsVert: return true;
  }
  return false;
}

// Returns nullptr when the clause is safe, otherwise a static reason.
const char* ClauseUnsafe(uint32_t stage, std::span<const uint32_t> code, const Clause& clause) {
  const uint32_t total = uint32_t(code.size() / 3);
  if (clause.count > kMaxExecCount) return "exec clause count above 6";
  if (clause.address + clause.count > total) return "exec clause beyond the code";
  for (uint32_t i = 0; i < clause.count; ++i) {
    const uint32_t* words = code.data() + size_t(clause.address + i) * 3;
    if ((clause.sequence >> (2 * i)) & 1) {
      FetchInstruction fetch;
      std::memcpy(static_cast<void*>(&fetch), words, sizeof(fetch));
      if (!FetchOpcodeKnown(fetch.opcode())) return "unknown fetch opcode";
    } else {
      AluInstruction alu;
      std::memcpy(static_cast<void*>(&alu), words, sizeof(alu));
      if (!alu.is_export()) continue;
      // Exports go to the vector destination, which cannot be relative: anything else
      // (eA/eM0-4 memory export, or a destination of the other stage) is not provable.
      if (alu.is_vector_dest_relative()) return "relative export destination";
      if (!ExportDestinationAllowed(stage, alu.vector_dest())) return "export destination is not a register of this stage (memory export)";
    }
  }
  return nullptr;
}

uint64_t Hash(uint32_t stage, std::span<const uint32_t> code) {
  uint64_t hash = 0xcbf29ce484222325ull ^ stage;
  for (uint32_t word : code) hash = (hash ^ word) * 0x100000001b3ull;
  return hash ^ code.size();
}
}  // namespace

bool ShaderIsMemorySafe(uint32_t stage, std::span<const uint32_t> code, const char** reason) {
  auto unsafe = [reason](const char* why) {
    if (reason) *reason = why;
    return false;
  };
  if (reason) *reason = nullptr;
  if (stage > 1) return unsafe("unknown shader stage");
  const uint32_t total = uint32_t(code.size() / 3);
  if (!total) return unsafe("shader smaller than one instruction");
  if (total > kMaxInstructions) return unsafe("shader larger than 4096 instructions");

  // Control flow comes first and ends where the first exec clause's instructions begin.
  // Scanning every control-flow slot (not following jumps) covers any path execution takes.
  uint32_t first_instruction = total;
  std::vector<Clause> clauses;
  std::vector<uint32_t> targets;
  bool saw_slot = false;
  ControlFlowOpcode last_opcode = ControlFlowOpcode::kNop;  // Last non-nop slot.
  for (uint32_t pair = 0; pair < first_instruction; ++pair) {
    ControlFlowInstruction cf[2];
    UnpackControlFlowInstructions(code.data() + size_t(pair) * 3, cf);
    for (const auto& instruction : cf) {
      const ControlFlowOpcode opcode = instruction.opcode();
      Reserved reserved;
      if (!ReservedFor(opcode, reserved)) return unsafe("unknown control-flow opcode");
      if ((instruction.dword_0 & reserved.d0) || (instruction.dword_1 & reserved.d1)) {
        return unsafe("reserved control-flow bits set");
      }
      if (opcode != ControlFlowOpcode::kNop) {
        saw_slot = true;
        last_opcode = opcode;
      }
      if (opcode == ControlFlowOpcode::kAlloc && instruction.alloc.alloc_type() == AllocType::kMemory) {
        return unsafe("memory export allocation");
      }
      if (IsControlFlowOpcodeExec(opcode)) {
        // Exec, cond-exec and predicated forms share the first dword layout.
        const uint32_t address = instruction.exec.address();
        const uint32_t count = instruction.exec.count();
        // An empty exec (the compiler's tail `exece`) runs nothing: its address is not used.
        if (count) {
          if (address <= pair) return unsafe("instruction data overlaps control flow");
          if (address >= total) return unsafe("exec clause beyond the code");
          first_instruction = std::min(first_instruction, address);
          clauses.push_back({address, count, instruction.exec.sequence()});
        }
      } else if (opcode == ControlFlowOpcode::kLoopStart) {
        targets.push_back(instruction.loop_start.address());
      } else if (opcode == ControlFlowOpcode::kLoopEnd) {
        targets.push_back(instruction.loop_end.address());
      } else if (opcode == ControlFlowOpcode::kCondCall) {
        targets.push_back(instruction.cond_call.address());
      } else if (opcode == ControlFlowOpcode::kCondJmp) {
        targets.push_back(instruction.cond_jmp.address());
      }
    }
  }
  // The program must end inside the scanned section: otherwise execution could fall off the
  // last control-flow slot into instruction data that no exec clause references (unchecked).
  if (!saw_slot || last_opcode != ControlFlowOpcode::kExecEnd) {
    return unsafe("control flow does not end with an unconditional exec end");
  }
  const uint32_t control_flow_slots = first_instruction * 2;
  for (uint32_t target : targets) {
    if (target >= control_flow_slots) return unsafe("branch target outside control flow");
  }
  for (const Clause& clause : clauses) {
    if (const char* why = ClauseUnsafe(stage, code, clause)) return unsafe(why);
  }
  return true;
}

bool ShaderSafetyCache::IsSafe(uint32_t stage, std::span<const uint32_t> code, Verdict* verdict) {
  const uint64_t hash = Hash(stage, code);
  std::lock_guard<std::mutex> lock(mutex_);
  auto& bucket = entries_[hash];
  for (const Entry& entry : bucket) {
    if (entry.stage == stage && entry.code.size() == code.size() &&
        std::equal(code.begin(), code.end(), entry.code.begin())) {
      if (verdict) *verdict = {entry.reason, false};
      return entry.safe;
    }
  }
  const char* reason = nullptr;
  const bool safe = ShaderIsMemorySafe(stage, code, &reason);
  Entry entry{stage, std::vector<uint32_t>(code.begin(), code.end()), safe, reason};
  if (count_ >= kMaxCacheEntries) {
    entries_.clear();
    count_ = 0;
    entries_[hash].push_back(std::move(entry));
  } else {
    bucket.push_back(std::move(entry));
  }
  ++count_;
  if (verdict) *verdict = {reason, true};
  return safe;
}

size_t ShaderSafetyCache::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return count_;
}

}  // namespace sr::native
