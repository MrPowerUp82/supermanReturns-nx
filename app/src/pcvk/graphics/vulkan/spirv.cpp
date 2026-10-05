#include "spirv.h"
#include <fstream>
namespace superman_returns::graphics::vulkan {
bool ValidSpirv(std::span<const uint32_t> w) {
  return w.size() >= 5 && w[0] == 0x07230203 && w[1] >= 0x00010000 &&
         w[1] <= 0x00010300 && w[3] > 0 && w[4] == 0;
}
bool ReadSpirv(const std::filesystem::path &path, std::vector<uint32_t> &w,
               Error &e) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  auto size = stream.tellg();
  if (!stream || size < 20 || size % 4 || size > 16777216) {
    e = {"ReadSpirv", VK_ERROR_INITIALIZATION_FAILED,
         "Shader missing or invalid: " + path.string()};
    return false;
  }
  w.resize(size_t(size) / 4);
  stream.seekg(0);
  if (!stream.read(reinterpret_cast<char *>(w.data()), size) ||
      !ValidSpirv(w)) {
    e = {"ReadSpirv", VK_ERROR_INITIALIZATION_FAILED,
         "Invalid SPIR-V header: " + path.string()};
    return false;
  }
  return true;
}
} // namespace superman_returns::graphics::vulkan
