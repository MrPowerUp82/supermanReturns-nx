// Offline shader pack for the native Vulkan renderer.
//
// The PC project translates each guest shader container at run time with a patched XenosRecomp
// and DXC (VulkanShaderService). The Switch has neither, so tools/vkshaders/build_pack.py runs
// the same translation offline and stores every result blob (the "SVR3" encoding
// DecodeShaderResult reads: requirements, interface locations, SPIR-V) in one file. The file
// holds code derived from the game; it is generated locally and never committed.
//
// Layout, little-endian:
//   0   char[8]  "SRVKPK01"
//   8   u32      version (1)
//   12  u32      binding ABI (BindingContractVersion)
//   16  u32      entry count
//   20  u32      reserved (0)
//   24  u64      FNV-1a 64 of every byte from offset 32 to the end
//   32  entries  count x { u64 key, u32 container_size, u32 stage (0 VS, 1 PS),
//                          u64 blob_offset, u32 blob_size, u32 reserved }, sorted by (key, stage)
//   ... blobs
// `key` is FNV-1a 64 of the original container bytes (header + virtual + physical parts).
#pragma once
#include "binding_contract.h"
#include "container_key.h"
#include "../guest/shader_capture.h"
#include "vulkan_shader_service.h"
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>
namespace superman_returns::graphics::shaders {
inline constexpr char kShaderPackMagic[8] = {'S', 'R', 'V', 'K', 'P', 'K', '0', '1'};
inline constexpr uint32_t kShaderPackVersion = 1;
struct PackInput {
  uint64_t key = 0;
  uint32_t container_size = 0;
  ShaderStage stage = ShaderStage::kVertex;
  std::vector<uint8_t> blob;
};
// Used by the tests and by tools that already hold decoded blobs; build_pack.py writes the same layout.
std::vector<uint8_t> BuildShaderPack(std::vector<PackInput> inputs);
class ShaderPack {
public:
  // Validates structure, ABI, bounds and the payload hash. On failure the pack stays empty.
  bool Load(std::vector<uint8_t> file, std::string& error);
  size_t size() const { return entries_.size(); }
  // The owned result blob for a container, or empty when it is not in the pack.
  std::span<const uint8_t> Find(uint64_t key, uint32_t container_size, ShaderStage stage) const;
  using Visitor = std::function<void(uint64_t key, uint32_t container_size, ShaderStage, std::span<const uint8_t> blob)>;
  void ForEach(const Visitor&) const;
private:
  struct Entry { uint64_t key; uint32_t container_size, stage; uint64_t offset; uint32_t length; };
  std::vector<uint8_t> file_;
  std::vector<Entry> entries_;
};
// Thread-safe ShaderLookup over a pack: decodes each blob once, reports `unavailable` for
// containers the pack does not hold and `failed` for blobs that do not decode.
class PackShaderLookup {
public:
  explicit PackShaderLookup(std::shared_ptr<const ShaderPack> pack) : pack_(std::move(pack)) {}
  ShaderResult operator()(const guest::ShaderCapture& capture) const;
  uint64_t misses() const { std::lock_guard lock(mutex_); return misses_; }
private:
  std::shared_ptr<const ShaderPack> pack_;
  mutable std::mutex mutex_;
  mutable std::map<std::pair<uint64_t, uint32_t>, ShaderResult> cache_;
  mutable uint64_t misses_ = 0;
};
} // namespace superman_returns::graphics::shaders
