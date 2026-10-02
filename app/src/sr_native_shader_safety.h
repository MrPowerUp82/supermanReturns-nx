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

// stage 0 = vertex shader, 1 = pixel shader; code is in host word order.
bool ShaderIsMemorySafe(uint32_t stage, std::span<const uint32_t> code);

// The proof walks the whole program; draws reuse the answer for identical code.
class ShaderSafetyCache {
 public:
  bool IsSafe(uint32_t stage, std::span<const uint32_t> code);
  size_t size() const;

 private:
  struct Entry {
    uint32_t stage = 0;
    std::vector<uint32_t> code;
    bool safe = false;
  };
  mutable std::mutex mutex_;
  std::unordered_map<uint64_t, std::vector<Entry>> entries_;
  size_t count_ = 0;
};

}  // namespace sr::native
