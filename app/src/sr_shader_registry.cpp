#include "sr_shader_registry.h"
#include <stdexcept>

namespace sr::native {
namespace {
uint32_t BE(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
}
void ShaderRegistry::Load(const std::filesystem::path& path) {
  if (loaded_) throw std::logic_error("Shader registry cannot be reloaded while guest resources exist");
  library_.Cargar(path);
  loaded_ = true;
}
const Shader* ShaderRegistry::Identify(std::span<const uint8_t> buffer) const {
  if (!loaded_ || buffer.size() < 36) return nullptr;
  if ((BE(buffer.data()) & ~1u) != 0x102A1100) return nullptr;
  const uint32_t virtual_size = BE(buffer.data() + 4), physical_size = BE(buffer.data() + 8);
  const uint64_t size = uint64_t(virtual_size) + physical_size;
  if (virtual_size < 36 || !physical_size || size > 65536 || size > buffer.size()) return nullptr;
  // Resource buffers may contain padding after the complete container.
  return library_.Buscar(buffer.first(size_t(size)));
}
void ShaderRegistry::RememberEngineResource(uint32_t resource, const Shader* shader) {
  if (!resource) return;
  std::lock_guard lock(mutex_);
  if (shader) resources_.insert_or_assign(resource, shader);
  else resources_.erase(resource);  // Reused addresses cannot retain an old association.
}
const Shader* ShaderRegistry::FindEngineResource(uint32_t resource) const {
  std::lock_guard lock(mutex_);
  auto found = resources_.find(resource);
  return found == resources_.end() ? nullptr : found->second;
}
ShaderRegistry& RuntimeShaders() {
  static ShaderRegistry registry;
  return registry;
}
}  // namespace sr::native
