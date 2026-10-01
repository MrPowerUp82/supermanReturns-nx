// Extracts Superman Returns shader containers from the game files without
// modifying them. A signature match is only a candidate; it is kept only when
// sr::Validate accepts the header, constant table and microcode. Names follow
// scan order with one counter for all files (p_000123 / v_000124), so the file
// order below must never change. provenance.tsv records source and offset.
//
// Usage: sr_find_containers <game folder> <new output folder> [extra files...]
// Extra files (for example the decoded executable image) are scanned after the
// game folder, in the order given.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "sr_container.h"

namespace fs = std::filesystem;

int main(int argc, char** argv) try {
  if (argc < 3) {
    std::fprintf(stderr, "usage: sr_find_containers <game folder> <new output folder> [extra files...]\n");
    return 1;
  }
  const fs::path output(argv[2]);
  if (fs::exists(output)) throw std::runtime_error("output already exists");
  std::vector<fs::path> files;
  for (const auto& e : fs::recursive_directory_iterator(argv[1])) {
    if (!e.is_regular_file()) continue;
    std::string ext = e.path().extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == ".ast" || ext == ".xex") files.push_back(e.path());
  }
  std::sort(files.begin(), files.end());
  for (int i = 3; i < argc; ++i) files.emplace_back(argv[i]);
  fs::create_directories(output);
  std::ofstream index(output / "provenance.tsv");
  index << "file\tsource\toffset\tvirtual\tphysical\tcf_bytes\texec_instructions\n";
  size_t total = 0, rejected = 0;
  for (const auto& path : files) {
    const size_t n = fs::file_size(path);
    if (n < sr::kHeaderSize || n > (1ull << 31)) continue;
    std::vector<uint8_t> data(n);
    std::ifstream in(path, std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(data.data()), n)) throw std::runtime_error("read failed");
    size_t found = 0;
    for (size_t i = 0; i + sr::kHeaderSize <= n; ++i) {
      const uint8_t* p = data.data() + i;
      if (p[0] != 0x10 || p[1] != 0x2A || p[2] != 0x11 || p[3] > 1) continue;
      const uint32_t vs = nfsmw::Lector{data}.u32(i + 4), ps = nfsmw::Lector{data}.u32(i + 8);
      if (vs < sr::kHeaderSize || !ps || uint64_t(vs) + ps > sr::kMaxContainer ||
          uint64_t(vs) + ps > n - i) {
        ++rejected;
        continue;
      }
      std::vector<uint8_t> container(p, p + vs + ps);
      nfsmw::Flujo flow;
      try {
        flow = sr::Validate(container);
      } catch (const std::exception& error) {
        std::printf("  %s+0x%zX rejected: %s\n", path.filename().string().c_str(), i, error.what());
        ++rejected;
        continue;
      }
      char name[32];
      std::snprintf(name, sizeof(name), "%c_%06zu.bin", p[3] ? 'v' : 'p', total);
      std::ofstream copy(output / name, std::ios::binary);
      copy.write(reinterpret_cast<const char*>(container.data()), container.size());
      if (!copy) throw std::runtime_error("write failed");
      index << name << '\t' << path.filename().string() << '\t' << i << '\t' << vs << '\t' << ps << '\t'
            << flow.bytes << '\t' << flow.instrucciones << '\n';
      ++total;
      ++found;
      i += vs + ps - 1;
    }
    std::printf("%s: %zu containers\n", path.filename().string().c_str(), found);
  }
  std::printf("Total: %zu containers, %zu candidates rejected\n", total, rejected);
  return total ? 0 : 2;
} catch (const std::exception& error) {
  std::fprintf(stderr, "error: %s\n", error.what());
  return 2;
}
