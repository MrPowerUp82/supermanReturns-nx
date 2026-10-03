#include "sr_shader_registry.h"
#include "sr_native_commands.h"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>

static void Check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

static void Write(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  Check(f.write(reinterpret_cast<const char*>(data.data()), data.size()) && f.flush(),
        "Could not write synthetic library");
}

static bool Rejected(const std::filesystem::path& path) {
  sr::native::BibliotecaShaders library;
  try { library.Cargar(path); } catch (const std::runtime_error&) { return true; }
  return false;
}

// Fabricated 2008-style containers: header (signature, virtual and physical
// sizes) followed by random bytes. No game data. The SPIR-V is only the header
// and an OpEntryPoint "main" of the matching stage, which is what the loader checks.
static std::vector<uint8_t> SyntheticLibrary(size_t count) {
  std::mt19937 random(0x53524e58);
  std::vector<sr::native::Shader> shaders;
  for (size_t i = 0; i < count; ++i) {
    sr::native::Shader s;
    const bool vertex = i % 2 == 0;
    const uint32_t virtual_size = 0x24 + 4 * uint32_t(i % 7);
    const uint32_t physical_size = 13 + 11 * uint32_t(i);  // Not a multiple of 12 on purpose.
    s.original.resize(virtual_size + physical_size);
    for (auto& byte : s.original) byte = uint8_t(random());
    const uint32_t header[3] = {0x102A1100u | (vertex ? 1u : 0u), virtual_size, physical_size};
    for (int w = 0; w < 3; ++w)
      for (int b = 0; b < 4; ++b) s.original[w * 4 + b] = uint8_t(header[w] >> (24 - 8 * b));
    s.spirv = {0x07230203, 0x00010000, 0, 8, 0,
               (5u << 16) | 15u, vertex ? 0u : 4u, 1, 0x6e69616d, 0};
    shaders.push_back(std::move(s));
  }
  // The same container twice with the same translation must collapse to one entry.
  shaders.push_back(shaders.front());
  return sr::native::EmpaquetarShaders(std::move(shaders));
}

static void LoaderNegativeChecks(const std::filesystem::path& dir, const std::vector<uint8_t>& good) {
  auto tampered = good;
  tampered.back() ^= 0x40;
  Write(dir / "tampered.srsp", tampered);
  Check(Rejected(dir / "tampered.srsp"), "Altered library body must be rejected");
  auto foreign = good;
  foreign[0] = 'N';  // Another game's signature.
  Write(dir / "foreign.srsp", foreign);
  Check(Rejected(dir / "foreign.srsp"), "Library of another game must be rejected");
  auto truncated = good;
  truncated.resize(good.size() - 3);
  Write(dir / "truncated.srsp", truncated);
  Check(Rejected(dir / "truncated.srsp"), "Truncated library must be rejected");
  auto trailing = good;
  trailing.push_back(0);
  Write(dir / "trailing.srsp", trailing);
  Check(Rejected(dir / "trailing.srsp"), "Trailing bytes after the library must be rejected");
  Check(Rejected(dir / "missing.srsp"), "Missing library must be reported");
}

static void RegistryChecks(const std::filesystem::path& path) {
  sr::native::BibliotecaShaders library;
  library.Cargar(path);
  sr::native::ShaderRegistry registry;
  Check(!registry.Identify({}), "Empty unloaded registry must reject input");
  registry.Load(path);
  Check(registry.size() == library.shaders().size(), "Entry count mismatch");
  for (const auto& entry : library.shaders()) {
    const auto* shader = registry.Identify(entry.original);
    Check(shader && shader->huella == entry.huella && shader->vertices == entry.vertices,
          "Original container not identified");
    auto padded = entry.original;
    padded.insert(padded.end(), 16, 0xCC);
    Check(registry.Identify(padded) == shader, "Trailing resource padding must be allowed");
    Check(!registry.Identify(std::span(entry.original).first(entry.original.size() - 1)),
          "Truncated container must be rejected");
    padded[0] ^= 1;
    Check(!registry.Identify(padded), "Unknown container must be rejected");
    registry.RememberEngineResource(0x81234560, shader);
    Check(registry.FindEngineResource(0x81234560) == shader, "Resource association failed");
    registry.RememberEngineResource(0x81234560, nullptr);
    Check(!registry.FindEngineResource(0x81234560), "Reused address retained stale shader");
    registry.RememberEngineResource(0, shader);
    Check(!registry.FindEngineResource(0), "Null resource must not be registered");
    sr::native::ShaderIdentity identity{entry.vertices, entry.original};
    registry.RememberD3DObject(0x82345600, identity);
    Check(registry.FindD3DObject(0x82345600) == shader, "D3D object identity missing");
    identity.vertex = !identity.vertex;
    registry.RememberD3DObject(0x82345600, identity);
    Check(!registry.FindD3DObject(0x82345600), "Stage mismatch retained old object identity");
    registry.RememberD3DObject(0x82345600, {entry.vertices, entry.original});
    registry.ForgetD3DObject(0x82345600);
    Check(!registry.FindD3DObject(0x82345600), "Destroyed D3D object retained stale identity");
  }
  bool refused = false;
  try { registry.Load(path); } catch (const std::logic_error&) { refused = true; }
  Check(refused, "Reload must not invalidate registered shader pointers");
  std::cout << "Registry: " << registry.size()
            << " containers identified; padding, truncation, address reuse and reload checks passed\n";
}

int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::runtime_error("Expected local shader library path or --synthetic");
    if (std::string_view(argv[1]) != "--synthetic") {
      RegistryChecks(argv[1]);
      return 0;
    }
    const auto dir = std::filesystem::temp_directory_path() /
                     ("sr-registry-test-" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(dir);
    try {
      constexpr size_t kCount = 24;
      const auto good = SyntheticLibrary(kCount);
      Write(dir / "synthetic.srsp", good);
      sr::native::BibliotecaShaders library;
      library.Cargar(dir / "synthetic.srsp");
      Check(library.shaders().size() == kCount, "Duplicate container was not collapsed");
      LoaderNegativeChecks(dir, good);
      std::cout << "Loader: tampered, foreign, truncated, trailing and missing libraries rejected\n";
      RegistryChecks(dir / "synthetic.srsp");
    } catch (...) {
      std::filesystem::remove_all(dir);
      throw;
    }
    std::filesystem::remove_all(dir);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
