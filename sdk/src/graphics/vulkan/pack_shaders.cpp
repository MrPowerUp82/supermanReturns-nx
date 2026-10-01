// Offline shader pack: identification and vertex input (see pack_shaders.h).
//
// Container layout (2008 format, big-endian; XenosRecomp shader.h): 0x24-byte header
// with the signature, virtual size (+4), physical size (+8), constant table (+16) and
// shader header (+24), all offsets into the virtual part. The shader header gives the
// microcode offset (+0, into the physical part) and size (+4); a vertex shader lists
// its declaration after +36, skipping the [+24] words before it, with [+28] elements;
// a pixel shader has its outputs at +28.

#include "pack_shaders.h"

#include <algorithm>
#include <array>
#include <cstring>

#include <rex/hash.h>
#include <rex/logging.h>

namespace rex::graphics::vulkan::pack {
namespace {

// Bits of a vertex fetch instruction that Direct3D does not touch: opcode, source and
// destination registers and predicate (VertexFetchInstruction in ucode.h). The fetch
// constant, format, signedness, stride, offset and the destination swizzle come from
// the vertex declaration, and the declared fetches may be reordered.
constexpr uint32_t kFetchKept[3] = {0x0007FFFF, 0x80000000, 0x80000000};

// Reduces the declared fetches to the bits Direct3D keeps and sorts them, so a patched
// and reordered vertex shader compares equal to its original.
void CanonicalizeFetches(std::span<uint32_t> microcode, std::span<const uint16_t> positions) {
  std::array<std::array<uint32_t, 3>, 64> fetches;
  const size_t count = std::min(positions.size(), fetches.size());
  for (size_t i = 0; i < count; ++i) {
    const size_t at = size_t(positions[i]) * 3;
    for (size_t j = 0; j < 3; ++j) {
      fetches[i][j] = microcode[at + j] & kFetchKept[j];
    }
  }
  std::sort(fetches.begin(), fetches.begin() + count);
  for (size_t i = 0; i < count; ++i) {
    const size_t at = size_t(positions[i]) * 3;
    for (size_t j = 0; j < 3; ++j) {
      microcode[at + j] = fetches[i][j];
    }
  }
}

constexpr uint32_t kMaxWarnings = 48;

struct Reader {
  std::span<const uint8_t> b;
  bool Has(size_t offset, size_t size) const {
    return offset <= b.size() && size <= b.size() - offset;
  }
  uint32_t U32(size_t p) const {
    return uint32_t(b[p]) << 24 | uint32_t(b[p + 1]) << 16 | uint32_t(b[p + 2]) << 8 | b[p + 3];
  }
  uint16_t U16(size_t p) const { return uint16_t(uint32_t(b[p]) << 8 | b[p + 1]); }
};

const char* Parse(const PackShaderSource& source, Entry& e) {
  const Reader c{source.container};
  if (!c.Has(0, 0x24)) return "container too short";
  const uint32_t flags = c.U32(0);
  if ((flags & 0xFFFFFFFEu) != 0x102A1100u) return "unknown container signature";
  if (bool(flags & 1) != source.vertex) return "stage does not match the container";
  const uint32_t virtual_size = c.U32(4), physical_size = c.U32(8);
  if (uint64_t(virtual_size) + physical_size != c.b.size()) return "inconsistent container sizes";
  const uint32_t table = c.U32(16), header = c.U32(24);
  if (uint64_t(header) + (source.vertex ? 36 : 32) > virtual_size) {
    return "shader header outside the virtual part";
  }
  const uint32_t code_offset = c.U32(header), code_size = c.U32(header + 4);
  if (!code_size || (code_size % 4) || uint64_t(code_offset) + code_size > physical_size) {
    return "microcode outside the physical part";
  }
  e.vertex = source.vertex;
  e.source = &source;
  e.microcode.resize(code_size / 4);
  for (size_t i = 0; i < e.microcode.size(); ++i) {
    e.microcode[i] = c.U32(size_t(virtual_size) + code_offset + i * 4);
  }

  if (e.vertex) {
    const uint32_t skipped = c.U32(header + 24), count = c.U32(header + 28);
    const uint64_t first = uint64_t(header) + 36 + uint64_t(skipped) * 4;
    if (count > 64 || skipped > 1024 || first + uint64_t(count) * 4 > virtual_size) {
      return "vertex declaration outside the virtual part";
    }
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t value = c.U32(size_t(first) + size_t(i) * 4);
      VertexElement element;
      element.instruction = uint16_t(value & 0xFFF);
      element.usage = uint8_t((value >> 12) & 0xF);
      element.usage_index = uint8_t((value >> 16) & 0xF);
      if ((size_t(element.instruction) + 1) * 3 > e.microcode.size()) {
        return "vertex element outside the microcode";
      }
      element.reg = uint8_t((e.microcode[size_t(element.instruction) * 3] >> 12) & 0x3F);
      element.swizzle = uint16_t(e.microcode[size_t(element.instruction) * 3 + 1] & 0xFFF);
      e.elements.push_back(element);
      e.fetch_positions.push_back(element.instruction);
    }
    std::sort(e.fetch_positions.begin(), e.fetch_positions.end());
    e.fetch_positions.erase(std::unique(e.fetch_positions.begin(), e.fetch_positions.end()),
                            e.fetch_positions.end());
    CanonicalizeFetches(e.microcode, e.fetch_positions);
  } else {
    e.ps_outputs = c.U32(header + 28);
  }

  // Constant table: the samplers.
  if (!table || !c.Has(size_t(table) + 4, 28)) return "no constant table";
  const size_t base = size_t(table) + 4;
  const uint32_t constants = c.U32(base + 12), info = c.U32(base + 16);
  if (constants > 1024 || !c.Has(base + info, size_t(constants) * 20)) {
    return "constant table outside the container";
  }
  for (uint32_t i = 0; i < constants; ++i) {
    const size_t p = base + info + size_t(i) * 20;
    if (c.U16(p + 4) != 3) continue;  // RegisterSet::Sampler
    Sampler sampler;
    sampler.reg = c.U16(p + 6);
    const uint32_t type_info = c.U32(p + 12);
    sampler.type = c.Has(base + type_info, 4) ? c.U16(base + type_info + 2) : 0;
    e.samplers.push_back(sampler);
  }

  e.fingerprint = XXH3_64bits(e.microcode.data(), e.microcode.size() * sizeof(uint32_t));
  return nullptr;
}

uint64_t CandidateKey(bool vertex, size_t words) {
  return (uint64_t(vertex) << 32) | uint64_t(words);
}

// Vertex input locations (XenosRecomp USAGE_LOCATIONS).
int32_t LocationOfUsage(uint8_t usage, uint8_t index) {
  switch (usage) {
    case 0:  // position
      return index == 0 ? 0 : (index == 1 ? 15 : -1);
    case 3:  // normal
      return index == 0 ? 1 : -1;
    case 6:  // tangent
      return index == 0 ? 2 : -1;
    case 7:  // binormal
      return index == 0 ? 3 : -1;
    case 5:  // texcoord
      return index < 4 ? 4 + index : (index < 8 ? 12 + (index - 4) : -1);
    case 10:  // color
      return index == 0 ? 8 : (index == 1 ? 11 : -1);
    case 2:  // blendindices
      return index == 0 ? 9 : -1;
    case 1:  // blendweight
      return index == 0 ? 10 : -1;
    default:
      return -1;
  }
}

// XenosRecomp USAGE_TYPES: only BLENDINDICES is uint4.
bool IsIntegerInput(uint8_t usage) {
  return usage == 2;
}

// Components a fetch swizzle writes (the ones that are not 7), as a 4-bit mask.
uint32_t WriteMask(uint32_t swizzle) {
  uint32_t mask = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (((swizzle >> (i * 3)) & 0x7) != 7) {
      mask |= 1u << i;
    }
  }
  return mask;
}

// g_InputRemap code: the SPIR-V writes r[i] = input[original[i]] while the fetch patched
// by Direct3D writes r[i] = data[patched[i]] (or 0 / 1). For each written component,
// the host input at original[i] must come from patched[i]. 7 = the same component.
uint32_t RemapCode(uint32_t original, uint32_t patched) {
  uint32_t code = kRemapIdentity;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t o = (original >> (i * 3)) & 0x7;
    const uint32_t d = (patched >> (i * 3)) & 0x7;
    if (o <= 3 && d != 7) {
      code = (code & ~(uint32_t(0x7) << (o * 3))) | (d << (o * 3));
    }
  }
  return code;
}

// Xenos vertex format to a Vulkan attribute format. The upload swaps whole 32-bit
// words with the fetch constant's endianness, so the components are in host order.
VkFormat AttributeFormat(uint32_t format, bool integer_input, bool is_signed, bool integer,
                         bool& r11g11b10_out) {
  r11g11b10_out = false;
  const auto pick = [&](VkFormat unorm, VkFormat snorm, VkFormat uscaled, VkFormat sscaled,
                        VkFormat uint_format, VkFormat sint_format) {
    if (integer_input) return is_signed ? sint_format : uint_format;
    if (integer) return is_signed ? sscaled : uscaled;
    return is_signed ? snorm : unorm;
  };
  switch (format) {
    case 6:  // k_8_8_8_8
      return pick(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_USCALED,
                  VK_FORMAT_R8G8B8A8_SSCALED, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT);
    case 7:  // k_2_10_10_10
      return pick(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_SNORM_PACK32,
                  VK_FORMAT_A2B10G10R10_USCALED_PACK32, VK_FORMAT_A2B10G10R10_SSCALED_PACK32,
                  VK_FORMAT_A2B10G10R10_UINT_PACK32, VK_FORMAT_A2B10G10R10_SINT_PACK32);
    case 16:  // k_10_11_11: decoded by the shader itself (tfetchR11G11B10)
      if (!integer_input) break;
      r11g11b10_out = true;
      return VK_FORMAT_R32_UINT;
    case 25:  // k_16_16
      return pick(VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_USCALED,
                  VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT);
    case 26:  // k_16_16_16_16
      return pick(VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM,
                  VK_FORMAT_R16G16B16A16_USCALED, VK_FORMAT_R16G16B16A16_SSCALED,
                  VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_SINT);
    case 31:  // k_16_16_FLOAT
      return integer_input ? VK_FORMAT_UNDEFINED : VK_FORMAT_R16G16_SFLOAT;
    case 32:  // k_16_16_16_16_FLOAT
      return integer_input ? VK_FORMAT_UNDEFINED : VK_FORMAT_R16G16B16A16_SFLOAT;
    case 33:  // k_32
      return integer_input ? (is_signed ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_UINT)
                           : VK_FORMAT_UNDEFINED;
    case 34:  // k_32_32
      return integer_input ? (is_signed ? VK_FORMAT_R32G32_SINT : VK_FORMAT_R32G32_UINT)
                           : VK_FORMAT_UNDEFINED;
    case 35:  // k_32_32_32_32
      return integer_input ? (is_signed ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT)
                           : VK_FORMAT_UNDEFINED;
    // Floats: a uint4 input reinterprets the bits (asfloat in the shader).
    case 36:  // k_32_FLOAT
      return integer_input ? VK_FORMAT_R32_UINT : VK_FORMAT_R32_SFLOAT;
    case 37:  // k_32_32_FLOAT
      return integer_input ? VK_FORMAT_R32G32_UINT : VK_FORMAT_R32G32_SFLOAT;
    case 57:  // k_32_32_32_FLOAT
      return integer_input ? VK_FORMAT_R32G32B32_UINT : VK_FORMAT_R32G32B32_SFLOAT;
    case 38:  // k_32_32_32_32_FLOAT
      return integer_input ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R32G32B32A32_SFLOAT;
    default:
      break;
  }
  return VK_FORMAT_UNDEFINED;
}

}  // namespace

Mode ModeFromCvar() {
  const std::string& value = REXCVAR_GET(pack_shaders);
  if (value == "draw") return Mode::kDraw;
  if (value == "identify") return Mode::kIdentify;
  if (value != "off") {
    REXGPU_WARN("pack_shaders = '{}' is not off, identify or draw: the pack is not used", value);
  }
  return Mode::kOff;
}

void Library::Load(const std::vector<PackShaderSource>& sources) {
  entries_.clear();
  candidates_.clear();
  cache_[0].clear();
  cache_[1].clear();
  entries_.reserve(sources.size());
  uint32_t vertex = 0, pixel = 0, rejected = 0;
  for (uint32_t i = 0; i < sources.size(); ++i) {
    Entry e;
    e.number = i;
    if (const char* reason = Parse(sources[i], e)) {
      if (++rejected <= 8) {
        REXGPU_WARN("Shader pack: entry {} skipped: {}", i, reason);
      }
      continue;
    }
    (e.vertex ? vertex : pixel) += 1;
    entries_.push_back(std::move(e));
  }
  for (uint32_t i = 0; i < entries_.size(); ++i) {
    const Entry& e = entries_[i];
    candidates_[CandidateKey(e.vertex, e.microcode.size())].push_back(i);
  }
  REXGPU_INFO("Shader pack: {} vertex and {} pixel shaders ({} entries rejected)", vertex, pixel,
              rejected);
}

const Entry* Library::Identify(bool vertex, uint64_t ucode_hash, std::span<const uint32_t> ucode) {
  auto& cache = cache_[vertex ? 1 : 0];
  if (auto it = cache.find(ucode_hash); it != cache.end()) {
    return it->second;
  }
  StageStats& stats = stats_[vertex ? 1 : 0];
  ++stats.distinct;

  const Entry* found = nullptr;
  uint32_t matches = 0;
  bool ambiguous_translation = false;
  const auto candidates = candidates_.find(CandidateKey(vertex, ucode.size()));
  if (candidates != candidates_.end()) {
    for (uint32_t index : candidates->second) {
      const Entry& e = entries_[index];
      scratch_.assign(ucode.begin(), ucode.end());
      CanonicalizeFetches(scratch_, e.fetch_positions);
      if (XXH3_64bits(scratch_.data(), scratch_.size() * sizeof(uint32_t)) != e.fingerprint ||
          scratch_ != e.microcode) {
        continue;
      }
      ++matches;
      if (!found) {
        found = &e;
      } else if (!std::equal(found->source->spirv.begin(), found->source->spirv.end(),
                             e.source->spirv.begin(), e.source->spirv.end())) {
        // Containers with the same microcode but different translations (their
        // constant tables differ): which one the guest created is unknown.
        ambiguous_translation = true;
      }
    }
  }

  if (found && ambiguous_translation) {
    ++stats.ambiguous;
    if (warnings_ < kMaxWarnings) {
      ++warnings_;
      REXGPU_WARN("Shader pack: {} shader {:016X} matches {} entries with different SPIR-V; "
                  "drawn with Xenos",
                  vertex ? "vertex" : "pixel", ucode_hash, matches);
    }
    found = nullptr;
  } else if (found) {
    ++stats.identified;
    if (matches > 1) {
      ++stats.ambiguous;
    }
  } else if (warnings_ < kMaxWarnings) {
    ++warnings_;
    const size_t same_length = candidates != candidates_.end() ? candidates->second.size() : 0;
    // The closest container of the same length, to tell a patch the mask misses from a
    // shader that is not in the pack.
    size_t closest_differences = SIZE_MAX;
    uint32_t closest_number = 0;
    if (candidates != candidates_.end()) {
      for (uint32_t index : candidates->second) {
        const Entry& e = entries_[index];
        scratch_.assign(ucode.begin(), ucode.end());
        CanonicalizeFetches(scratch_, e.fetch_positions);
        size_t differences = 0;
        for (size_t i = 0; i < ucode.size(); ++i) {
          differences += scratch_[i] != e.microcode[i];
        }
        if (differences < closest_differences) {
          closest_differences = differences;
          closest_number = e.number;
        }
      }
    }
    if (same_length) {
      REXGPU_WARN(
          "Shader pack: {} shader {:016X} ({} words) not found; closest of the {} with that "
          "length is entry {} with {} different words",
          vertex ? "vertex" : "pixel", ucode_hash, ucode.size(), same_length, closest_number,
          closest_differences);
    } else {
      REXGPU_WARN("Shader pack: {} shader {:016X} ({} words) not found; no entry has that length",
                  vertex ? "vertex" : "pixel", ucode_hash, ucode.size());
    }
  }
  cache.emplace(ucode_hash, found);
  return found;
}

const char* BuildVertexInput(const Entry& vs, std::span<const uint32_t> patched,
                             VertexInput& input) {
  input = VertexInput{};
  input.remap.fill(kRemapIdentity);
  if (patched.size() != vs.microcode.size()) {
    return "patched microcode length differs from the pack";
  }
  uint32_t locations_used = 0;
  for (const VertexElement& element : vs.elements) {
    const uint32_t reg = element.reg;
    const uint32_t original_swizzle = element.swizzle;
    // Direct3D may reorder the declared fetches, but not their destination registers.
    size_t q = SIZE_MAX;
    for (const uint16_t position : vs.fetch_positions) {
      const size_t i = size_t(position) * 3;
      if (((patched[i] >> 12) & 0x3F) != reg || (patched[i] & 0x1F) != 0) {
        continue;
      }
      if (q == SIZE_MAX) {
        q = i;
      }
      if (WriteMask(patched[i + 1] & 0xFFF) == WriteMask(original_swizzle)) {
        q = i;
        break;
      }
    }
    if (q == SIZE_MAX) {
      return "no patched fetch writes a declared register";
    }
    const uint32_t d0 = patched[q], d1 = patched[q + 1], d2 = patched[q + 2];
    const uint32_t fetch_constant = ((d0 >> 20) & 0x1F) * 3 + ((d0 >> 25) & 0x3);
    const uint32_t format = (d1 >> 16) & 0x3F;
    const int32_t exp_adjust = int32_t(d1 << 2) >> 26;
    const bool mini = (d1 >> 30) & 0x1;
    const uint32_t stride = mini ? 0 : (d2 & 0xFF) * 4;
    const int32_t offset = (int32_t(d2 << 1) >> 9) * 4;
    if (exp_adjust) {
      return "vertex fetch with an exponent adjustment";
    }
    if (offset < 0) {
      return "negative vertex fetch offset";
    }
    const int32_t location = LocationOfUsage(element.usage, element.usage_index);
    if (location < 0 || ((locations_used >> location) & 1)) {
      // XenosRecomp gives such an element no input either.
      continue;
    }
    bool r11g11b10 = false;
    const VkFormat vk_format = AttributeFormat(format, IsIntegerInput(element.usage),
                                               (d1 >> 12) & 0x1, (d1 >> 13) & 0x1, r11g11b10);
    if (vk_format == VK_FORMAT_UNDEFINED) {
      return "vertex format not supported";
    }
    if (r11g11b10) {
      input.vertex_spec |= kSpecR11G11B10Normal;
    }
    VertexAttribute attribute;
    attribute.location = uint32_t(location);
    attribute.format = vk_format;
    attribute.offset = uint32_t(offset);
    // The stream: found by fetch constant; its stride comes from a full fetch.
    uint32_t stream = 0;
    while (stream < input.streams.size() &&
           input.streams[stream].fetch_constant != fetch_constant) {
      ++stream;
    }
    if (stream == input.streams.size()) {
      input.streams.push_back({fetch_constant, stride, 0});
    } else if (!input.streams[stream].stride) {
      input.streams[stream].stride = stride;
    }
    attribute.binding = stream;
    input.attributes.push_back(attribute);
    input.remap[location] = RemapCode(original_swizzle, d1 & 0xFFF);
    locations_used |= uint32_t(1) << location;
  }
  for (const VertexStream& stream : input.streams) {
    if (!stream.stride) {
      return "vertex stream without a stride";
    }
  }
  // Attributes past the stride (non-interleaved data under one fetch constant) get a
  // stream of their own starting at the whole strides they skip.
  const size_t interleaved_streams = input.streams.size();
  for (VertexAttribute& attribute : input.attributes) {
    const VertexStream stream = input.streams[attribute.binding];
    if (attribute.binding >= interleaved_streams || attribute.offset < stream.stride) {
      continue;
    }
    const uint32_t base = attribute.offset / stream.stride * stream.stride;
    uint32_t split = uint32_t(interleaved_streams);
    while (split < input.streams.size() &&
           (input.streams[split].fetch_constant != stream.fetch_constant ||
            input.streams[split].base != base)) {
      ++split;
    }
    if (split == input.streams.size()) {
      input.streams.push_back({stream.fetch_constant, stream.stride, base});
    }
    attribute.binding = split;
    attribute.offset -= base;
  }
  if (input.streams.size() > 16) {
    return "too many vertex streams";
  }
  uint64_t hash = XXH3_64bits(input.attributes.data(),
                              input.attributes.size() * sizeof(VertexAttribute));
  hash = XXH3_64bits_withSeed(input.streams.data(), input.streams.size() * sizeof(VertexStream),
                              hash);
  input.hash = hash ^ input.vertex_spec;
  return nullptr;
}

const char* FallbackName(Fallback reason) {
  static const char* const kNames[] = {
      "VS fuera del pack", "PS fuera del pack", "tipo de VS anfitrion", "primitiva",
      "memexport",         "transformacion",     "recorte o kill",       "render target",
      "estado de pixel",   "textura",            "sampler",              "entrada de vertices",
      "datos de vertices", "indices",            "pipeline",             "recursos",
  };
  static_assert(std::size(kNames) == size_t(Fallback::kCount));
  return reason < Fallback::kCount ? kNames[size_t(reason)] : "?";
}

}  // namespace rex::graphics::vulkan::pack
