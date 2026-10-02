#pragma once

// Bounded proof that a loaded Xenos shader cannot write guest memory (memexport).
// A native draw is omitted in the first milestone only when this proof succeeds;
// anything the scan does not fully understand is unsafe.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace sr::native {

// stage 0 = vertex shader, 1 = pixel shader; code is in host word order. When the proof
// fails, *reason (if given) points at a static string naming why; on success it is nullptr.
bool ShaderIsMemorySafe(uint32_t stage, std::span<const uint32_t> code, const char** reason = nullptr);

// The proof walks the whole program; draws reuse the answer for identical code.
class ShaderSafetyCache {
 public:
  struct Verdict {
    const char* reason = nullptr;  // Why the proof failed (static string), nullptr when safe.
    bool fresh = false;            // True only the first time this code was evaluated.
  };
  bool IsSafe(uint32_t stage, std::span<const uint32_t> code, Verdict* verdict = nullptr);
  size_t size() const;

 private:
  struct Entry {
    uint32_t stage = 0;
    std::vector<uint32_t> code;
    bool safe = false;
    const char* reason = nullptr;
  };
  mutable std::mutex mutex_;
  std::unordered_map<uint64_t, std::vector<Entry>> entries_;
  size_t count_ = 0;
};

}  // namespace sr::native
