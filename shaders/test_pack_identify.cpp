// Host test of the shader pack identification and vertex input
// (sdk/src/graphics/vulkan/pack_shaders.cpp), without the game or a GPU.
//
//   test_pack_identify                 synthetic containers only
//   test_pack_identify <containers>    also every *.bin of a library build
//                                      (provenance.tsv names them)
//
// The synthetic vertex shader imitates what the console's Direct3D does to a vertex
// shader before IM_LOAD: it rewrites the fetch constant, format, signedness, stride and
// offset of each declared fetch, and may reorder the fetches. Identification must
// survive that; any other change must not be identified.

#include "../sdk/src/graphics/vulkan/pack_shaders.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

std::string& FLAGS_pack_shaders_storage_() {
  static std::string value = "identify";
  return value;
}

namespace {
using rex::graphics::PackShaderSource;
namespace pack = rex::graphics::vulkan::pack;

void Check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

void PutBE(std::vector<uint8_t>& b, size_t at, uint32_t value) {
  for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(value >> (24 - 8 * i));
}

// Vertex fetch words (ucode.h VertexFetchInstruction), host order.
struct Fetch {
  uint32_t dst = 0, swizzle = 0x688, fetch_constant = 95, format = 0, is_signed = 0,
           integer = 0, stride = 0, offset = 0, mini = 0;
  void Write(uint32_t* w) const {
    w[0] = (dst << 12) | (1u << 19) | ((fetch_constant / 3) << 20) | ((fetch_constant % 3) << 25);
    w[1] = swizzle | (is_signed << 12) | (integer << 13) | (format << 16) | (mini << 30);
    w[2] = stride | (offset << 8);
  }
};

constexpr uint32_t kHeader = 0x24, kTable = 0x24, kShader = 0x60;

// A 2008 container: header, an empty constant table, the shader header with the
// declaration and the microcode as the whole physical part.
std::vector<uint8_t> Container(bool vertex, const std::vector<uint32_t>& microcode,
                               const std::vector<uint32_t>& elements) {
  const uint32_t virtual_size = kShader + 36 + 4 * uint32_t(elements.size());
  const uint32_t physical_size = 4 * uint32_t(microcode.size());
  std::vector<uint8_t> b(virtual_size + physical_size);
  PutBE(b, 0, 0x102A1100u | (vertex ? 1u : 0u));
  PutBE(b, 4, virtual_size);
  PutBE(b, 8, physical_size);
  PutBE(b, 16, kTable);
  PutBE(b, 24, kShader);
  PutBE(b, kTable + 4 + 12, 0);   // no constants
  PutBE(b, kTable + 4 + 16, 28);  // their info right after the table header
  PutBE(b, kShader + 0, 0);       // microcode offset in the physical part
  PutBE(b, kShader + 4, physical_size);
  PutBE(b, kShader + 24, 0);
  PutBE(b, kShader + 28, uint32_t(elements.size()));
  for (size_t i = 0; i < elements.size(); ++i) PutBE(b, kShader + 36 + 4 * i, elements[i]);
  for (size_t i = 0; i < microcode.size(); ++i) PutBE(b, virtual_size + 4 * i, microcode[i]);
  return b;
}

uint32_t Element(uint32_t instruction, uint32_t usage, uint32_t index) {
  return instruction | (usage << 12) | (index << 16);
}

void Synthetic() {
  // Slots 1-3 are the declared fetches (position r1, normal r2, texcoord0 r3), slot 4 ALU.
  std::vector<uint32_t> original(15);
  for (size_t i = 0; i < original.size(); ++i) original[i] = 0x10000000u + uint32_t(i) * 0x01010101u;
  Fetch position, normal, texcoord;
  position.dst = 1;
  normal.dst = 2;
  normal.swizzle = 0xE88;  // xyz_ (w not written)
  texcoord.dst = 3;
  texcoord.swizzle = 0xFC8;  // xy__
  position.Write(&original[3]);
  normal.Write(&original[6]);
  texcoord.Write(&original[9]);
  const std::vector<uint32_t> elements = {Element(1, 0, 0), Element(2, 3, 0), Element(3, 5, 0)};

  // A pixel shader of the same length must not be confused with it.
  std::vector<uint32_t> pixel_microcode = original;
  pixel_microcode[12] ^= 1;
  const std::vector<uint8_t> vertex_container = Container(true, original, elements);
  const std::vector<uint8_t> pixel_container = Container(false, pixel_microcode, {});
  std::vector<PackShaderSource> sources = {{vertex_container, {}, true},
                                           {pixel_container, {}, false}};
  pack::Library library;
  library.Load(sources);
  Check(library.size() == 2, "synthetic pack: both entries load");

  const pack::Entry* vs = library.Identify(true, 1, original);
  Check(vs && vs->vertex && vs->number == 0, "unpatched vertex shader identified");
  Check(!library.Identify(false, 2, original), "vertex microcode is not a pixel shader");
  const pack::Entry* ps = library.Identify(false, 3, pixel_microcode);
  Check(ps && !ps->vertex && ps->number == 1, "pixel shader identified");

  // Direct3D's patch: one interleaved stream in fetch constant 0, 32-byte vertices;
  // the normal and texcoord fetches swapped places.
  std::vector<uint32_t> patched = original;
  Fetch p_position = position, p_normal = normal, p_texcoord = texcoord;
  p_position.fetch_constant = 0;
  p_position.format = 57;  // k_32_32_32_FLOAT
  p_position.stride = 8;
  p_normal.fetch_constant = 0;
  p_normal.format = 26;  // k_16_16_16_16
  p_normal.is_signed = 1;
  p_normal.stride = 8;
  p_normal.offset = 3;
  p_normal.swizzle = 0xE88;
  p_texcoord.fetch_constant = 0;
  p_texcoord.format = 31;  // k_16_16_FLOAT
  p_texcoord.stride = 8;
  p_texcoord.offset = 5;
  p_texcoord.swizzle = 0xFC1;  // yx__: Direct3D swapped the components
  p_position.Write(&patched[3]);
  p_texcoord.Write(&patched[6]);  // reordered
  p_normal.Write(&patched[9]);
  Check(library.Identify(true, 4, patched) == vs, "patched vertex shader identified");

  pack::VertexInput input;
  const char* failure = pack::BuildVertexInput(*vs, patched, input);
  if (failure) throw std::runtime_error(failure);
  Check(input.streams.size() == 1 && input.streams[0].fetch_constant == 0 &&
            input.streams[0].stride == 32 && input.streams[0].base == 0,
        "one interleaved stream of 32 bytes");
  Check(input.attributes.size() == 3, "three attributes");
  const auto find = [&](uint32_t location) -> const pack::VertexAttribute& {
    for (const auto& a : input.attributes)
      if (a.location == location) return a;
    throw std::runtime_error("attribute missing");
  };
  Check(find(0).format == VK_FORMAT_R32G32B32_SFLOAT && find(0).offset == 0, "position");
  Check(find(1).format == VK_FORMAT_R16G16B16A16_SNORM && find(1).offset == 12, "normal");
  Check(find(4).format == VK_FORMAT_R16G16_SFLOAT && find(4).offset == 20, "texcoord");
  // r3.xy = input.xy originally; the patched fetch writes r3.x = data.y, r3.y = data.x.
  Check(input.remap[4] == (0xFFFu & ~0x3Fu) + (1u << 0) + (0u << 3), "texcoord remap");

  // Non-interleaved data under one fetch constant: the texcoords after 100 vertices.
  std::vector<uint32_t> split = patched;
  Fetch s_texcoord = p_texcoord;
  s_texcoord.offset = 8 * 100;
  s_texcoord.Write(&split[6]);
  Check(library.Identify(true, 5, split) == vs, "split vertex shader identified");
  failure = pack::BuildVertexInput(*vs, split, input);
  if (failure) throw std::runtime_error(failure);
  Check(input.streams.size() == 2 && input.streams[1].base == 3200 && find(4).offset == 0 &&
            find(4).binding == 1,
        "attribute past the stride gets its own stream");

  // Anything else that changes is a different shader.
  std::vector<uint32_t> other = patched;
  other[13] ^= 0x100;
  Check(!library.Identify(true, 6, other), "changed ALU word is not identified");
  std::vector<uint32_t> other_register = patched;
  other_register[3] ^= 1u << 12;  // position fetch now writes r0
  Check(!library.Identify(true, 7, other_register), "changed fetch register is not identified");
  std::printf("synthetic: OK\n");
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary);
  Check(bool(f), "cannot open a container");
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

// Every distinct container of a library build (the packer keeps one copy of identical
// ones) must be identified by its own microcode, except where containers share the
// microcode but not the translation: those must stay unidentified.
void RealPack(const std::filesystem::path& containers_directory) {
  const std::filesystem::path spirv_directory = containers_directory.parent_path() / "spirv";
  std::ifstream list(containers_directory / "provenance.tsv");
  Check(bool(list), "no provenance.tsv in the containers directory");
  std::vector<std::vector<uint8_t>> containers;
  std::vector<std::vector<uint32_t>> translations;
  std::string line;
  std::getline(list, line);  // header
  while (std::getline(list, line)) {
    const std::string name = line.substr(0, line.find('\t'));
    if (name.empty()) continue;
    std::vector<uint8_t> container = ReadFile(containers_directory / name);
    if (std::find(containers.begin(), containers.end(), container) != containers.end()) continue;
    const std::vector<uint8_t> spirv_bytes =
        ReadFile(spirv_directory / (name.substr(0, name.rfind('.')) + ".spv"));
    std::vector<uint32_t> spirv(spirv_bytes.size() / 4);
    std::memcpy(spirv.data(), spirv_bytes.data(), spirv.size() * 4);
    containers.push_back(std::move(container));
    translations.push_back(std::move(spirv));
  }
  std::vector<PackShaderSource> sources;
  for (size_t i = 0; i < containers.size(); ++i) {
    sources.push_back({containers[i], translations[i], bool(containers[i][3] & 1)});
  }
  pack::Library library;
  library.Load(sources);
  Check(library.size() == sources.size(), "every container parses");
  size_t identified = 0, unidentified = 0;
  for (size_t i = 0; i < sources.size(); ++i) {
    const auto& c = containers[i];
    const auto u32 = [&](size_t p) {
      return uint32_t(c[p]) << 24 | uint32_t(c[p + 1]) << 16 | uint32_t(c[p + 2]) << 8 | c[p + 3];
    };
    const uint32_t virtual_size = u32(4), header = u32(24);
    const uint32_t code_offset = u32(header), code_size = u32(header + 4);
    std::vector<uint32_t> microcode(code_size / 4);
    for (size_t w = 0; w < microcode.size(); ++w) microcode[w] = u32(virtual_size + code_offset + 4 * w);
    const bool is_vertex = sources[i].vertex;
    const pack::Entry* entry = library.Identify(is_vertex, uint64_t(i) + 1000, microcode);
    if (!entry) {
      // Allowed only when another container has the same microcode and other SPIR-V.
      bool shared = false;
      for (size_t j = 0; j < sources.size(); ++j) {
        shared |= j != i && sources[j].vertex == is_vertex && translations[j] != translations[i];
      }
      Check(shared, "a library container is identified");
      ++unidentified;
      continue;
    }
    Check(entry->vertex == is_vertex, "identified with its stage");
    Check(std::equal(entry->source->spirv.begin(), entry->source->spirv.end(),
                     translations[i].begin(), translations[i].end()),
          "identified with its own translation");
    ++identified;
  }
  const auto& vs = library.stats(true);
  const auto& ps = library.stats(false);
  std::printf("pack: %zu distinct containers; %zu identified with their translation, %zu left to "
              "Xenos (same microcode, other SPIR-V); ambiguous: %llu vertex, %llu pixel\n",
              sources.size(), identified, unidentified, (unsigned long long)vs.ambiguous,
              (unsigned long long)ps.ambiguous);
}
}  // namespace

int main(int argc, char** argv) {
  try {
    Synthetic();
    if (argc > 1) RealPack(argv[1]);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
  return 0;
}
