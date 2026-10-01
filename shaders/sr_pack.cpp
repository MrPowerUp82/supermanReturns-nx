// Packs validated SPIR-V into the Superman Returns shader library, keyed by the
// original container (adapted from nfsmw-nx's nfsmw_empaquetar.cpp). Every
// container is validated again, and every one must be found after packing.
//
// Usage: sr_pack <containers> <validated spirv> <new output file>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>

#include "../app/src/sr_shader_library.h"
#include "sr_container.h"

namespace fs = std::filesystem;

static std::vector<uint8_t> Read(const fs::path& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f || f.tellg() < 0 || f.tellg() > 64 * 1024 * 1024)
    throw std::runtime_error("unreadable or too large input: " + path.string());
  std::vector<uint8_t> d(static_cast<size_t>(f.tellg()));
  f.seekg(0);
  if (!f.read(reinterpret_cast<char*>(d.data()), d.size())) throw std::runtime_error("short read");
  return d;
}

int main(int argc, char** argv) try {
  if (argc != 4) throw std::runtime_error("usage: sr_pack <containers> <validated spirv> <new output file>");
  const fs::path containers = argv[1], spirv = argv[2], output = argv[3];
  if (fs::exists(output)) throw std::runtime_error("output already exists");
  std::vector<fs::path> paths;
  for (const auto& e : fs::directory_iterator(containers))
    if (e.is_regular_file() && e.path().extension() == ".bin") paths.push_back(e.path());
  std::sort(paths.begin(), paths.end());
  std::vector<sr::native::Shader> shaders;
  for (const auto& path : paths) {
    sr::native::Shader s;
    s.original = Read(path);
    (void)sr::Validate(s.original);
    const auto code = Read(spirv / (path.stem().string() + ".spv"));
    if (code.size() % 4) throw std::runtime_error("misaligned SPIR-V: " + path.stem().string());
    for (size_t i = 0; i < code.size(); i += 4)
      s.spirv.push_back(uint32_t(code[i]) | uint32_t(code[i + 1]) << 8 | uint32_t(code[i + 2]) << 16 |
                        uint32_t(code[i + 3]) << 24);
    shaders.push_back(std::move(s));
  }
  const auto package = sr::native::EmpaquetarShaders(std::move(shaders));
  sr::native::BibliotecaShaders library;
  library.Cargar(package);
  for (const auto& path : paths)
    if (!library.Buscar(Read(path))) throw std::runtime_error("shader not found after packing: " + path.string());
  std::ofstream f(output, std::ios::binary);
  if (!f.write(reinterpret_cast<const char*>(package.data()), package.size()) || !f.flush())
    throw std::runtime_error("could not write the whole package");
  std::printf("%zu containers -> %zu unique shaders, %zu bytes; all retrievable\n", paths.size(),
              library.shaders().size(), package.size());
  return 0;
} catch (const std::exception& e) {
  std::fprintf(stderr, "%s\n", e.what());
  return 1;
}
