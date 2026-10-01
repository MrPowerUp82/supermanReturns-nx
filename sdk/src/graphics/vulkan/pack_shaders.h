#pragma once
/**
 * Draws with shaders translated offline by XenosRecomp (pack_shader_sources.h).
 *
 * The guest's Direct3D still runs and still writes the PM4 ring; the Xenos command
 * processor still owns render targets, textures, resolves and primitive processing.
 * Only the shaders change: when both shaders of a draw are found in the pack and the
 * draw uses nothing the pack shaders cannot reproduce, the draw is recorded with a
 * pipeline built from the pack SPIR-V and the interface those shaders expect
 * (shader_common.h): bindless texture / sampler arrays in sets 0-3, and the vertex,
 * pixel and shared constants as uniform buffers in set 4. Anything else falls back to
 * the Xenos translation of the same draw, with a counter for the reason.
 *
 * Identification follows nfsmw-nx (StevensND/nfsmw-nx, nfsmw_nativo_shaders.cpp,
 * GPL-3.0): pixel shaders reach IM_LOAD unchanged and are matched by their whole
 * microcode. Vertex shaders are patched by Direct3D for the vertex declaration, so the
 * fetch bits it rewrites are masked on both sides before comparing; the vertex input
 * of the draw is then read from the patched fetches.
 */

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/graphics/pack_shader_sources.h>
#include <rex/ui/vulkan/api.h>

namespace rex::graphics::vulkan::pack {

enum class Mode { kOff, kIdentify, kDraw };
// From the pack_shaders cvar; an unknown value is kOff (with a warning).
Mode ModeFromCvar();

struct VertexElement {
  uint16_t instruction = 0;  // index of the vertex fetch instruction (3 dwords each)
  uint8_t usage = 0;         // D3DDECLUSAGE
  uint8_t usage_index = 0;
  // Of the original fetch: destination register and swizzle.
  uint8_t reg = 0;
  uint16_t swizzle = 0;
};

struct Sampler {
  uint16_t reg = 0;   // texture fetch constant
  uint16_t type = 0;  // D3DXPARAMETER_TYPE: 10-12 2D, 13 3D, 14 cube
};

struct Entry {
  uint32_t number = 0;  // position in the registered list, for the logs
  bool vertex = false;
  const PackShaderSource* source = nullptr;
  // The microcode as IM_LOAD carries it, host byte order. In vertex shaders the declared
  // fetches are reduced to what Direct3D keeps and sorted (CanonicalizeFetches).
  std::vector<uint32_t> microcode;
  // Instruction indices of the declared fetches, ascending and unique.
  std::vector<uint16_t> fetch_positions;
  uint64_t fingerprint = 0;  // XXH3 of microcode
  std::vector<VertexElement> elements;
  std::vector<Sampler> samplers;
  uint32_t ps_outputs = 0;  // PixelShaderOutputs: COLOR0-3 bits 0-3, DEPTH bit 4
};

struct StageStats {
  uint64_t distinct = 0;      // distinct microcode loaded
  uint64_t identified = 0;    // of them, found in the pack
  // Found more than once: the first one is used if all have the same SPIR-V,
  // otherwise the shader counts as not identified.
  uint64_t ambiguous = 0;
};

class Library {
 public:
  // Parses the registered sources. Invalid entries are skipped with a warning.
  void Load(const std::vector<PackShaderSource>& sources);
  bool loaded() const { return !entries_.empty(); }
  size_t size() const { return entries_.size(); }

  // Command processor thread only. Cached by the microcode hash.
  const Entry* Identify(bool vertex, uint64_t ucode_hash, std::span<const uint32_t> ucode);

  const StageStats& stats(bool vertex) const { return stats_[vertex ? 1 : 0]; }

 private:
  std::vector<Entry> entries_;
  // (vertex, word count) -> candidate entries.
  std::unordered_map<uint64_t, std::vector<uint32_t>> candidates_;
  std::unordered_map<uint64_t, const Entry*> cache_[2];
  std::vector<uint32_t> scratch_;
  StageStats stats_[2];
  uint32_t warnings_ = 0;
};

// Vertex input of a draw, from the pack vertex shader and the microcode Direct3D patched.
struct VertexAttribute {
  uint32_t location;
  uint32_t binding;
  VkFormat format;
  uint32_t offset;
};

struct VertexStream {
  uint32_t fetch_constant;  // 0-95
  uint32_t stride;          // bytes
  uint32_t base;            // bytes from the fetch constant address
};

constexpr uint32_t kRemapIdentity = 0xFFF;

struct VertexInput {
  std::vector<VertexAttribute> attributes;
  std::vector<VertexStream> streams;
  std::array<uint32_t, 16> remap{};  // g_InputRemap per location
  uint32_t vertex_spec = 0;          // specialization bits from the formats
  uint64_t hash = 0;
};

// Returns nullptr on success, otherwise the reason.
const char* BuildVertexInput(const Entry& vertex_shader, std::span<const uint32_t> patched,
                             VertexInput& input_out);

// Specialization constant 0 bits (shader_common.h).
constexpr uint32_t kSpecR11G11B10Normal = 1u << 0;
constexpr uint32_t kSpecAlphaTest = 1u << 1;
constexpr uint32_t kSpecConstantsUbo = 1u << 8;
constexpr uint32_t kSpecAlphaFuncShift = 16;

// Shared constants block (shader_common.h, NFSMW_COMPARTIDA_*): byte offsets.
constexpr uint32_t kSharedTexture2DIndices = 0;
constexpr uint32_t kSharedTexture3DIndices = 64;
constexpr uint32_t kSharedTextureCubeIndices = 128;
constexpr uint32_t kSharedSamplerIndices = 192;
constexpr uint32_t kSharedBooleans = 256;
constexpr uint32_t kSharedSwappedTexcoords = 260;
constexpr uint32_t kSharedHalfPixelOffset = 264;
constexpr uint32_t kSharedAlphaThreshold = 272;
constexpr uint32_t kSharedAlphaFunction = 276;
constexpr uint32_t kSharedNdcScale = 280;
constexpr uint32_t kSharedNdcOffset = 288;
constexpr uint32_t kSharedInputRemap = 296;
constexpr uint32_t kSharedInvSize = 360;
// 16 InvTamano slots after the remap codes; the shaders declare 368 bytes.
constexpr uint32_t kSharedSize = 512;
constexpr uint32_t kFloatConstantsSize = 256 * 16;
constexpr uint32_t kHeapSize = 32;  // texture fetch constants

// Why a draw went to Xenos instead of the pack. Indices into the report.
enum class Fallback : uint32_t {
  kVertexShaderUnknown,
  kPixelShaderUnknown,
  kHostVertexShaderType,
  kPrimitiveType,
  kMemexport,
  kVertexTransform,
  kClipOrKill,
  kRenderTarget,
  kPixelState,
  kTexture,
  kSampler,
  kVertexInput,
  kVertexData,
  kIndexBuffer,
  kPipeline,
  kResources,
  kCount,
};
const char* FallbackName(Fallback reason);

}  // namespace rex::graphics::vulkan::pack
