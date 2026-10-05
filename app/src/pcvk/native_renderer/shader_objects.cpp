#include "shader_objects.h"
#include <cstring>
#include "../graphics/shaders/container_key.h"
#include "shader_container.h"
namespace superman_returns::native {
void ShaderObjects::Remember(uint32_t object, std::shared_ptr<const graphics::guest::ShaderCapture> capture) {
  if (!object) return;
  std::lock_guard<std::mutex> lock(mutex_);
  // A reused object address must never keep the previous container.
  if (capture) objects_.insert_or_assign(object, std::move(capture));
  else objects_.erase(object);
}
void ShaderObjects::Forget(uint32_t object) {
  std::lock_guard<std::mutex> lock(mutex_);
  objects_.erase(object);
}
std::shared_ptr<const graphics::guest::ShaderCapture> ShaderObjects::Find(uint32_t object) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = objects_.find(object);
  return it == objects_.end() ? nullptr : it->second;
}
size_t ShaderObjects::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return objects_.size();
}
std::shared_ptr<const graphics::guest::ShaderCapture> CaptureShaderContainer(GuestAccess& access,
                                                                          uint32_t source,
                                                                          bool vertex) {
  if (!source || source > UINT32_MAX - 12) return nullptr;
  const uint8_t* header = access.Readable(source, 12);
  if (!header) return nullptr;
  ShaderContainerHeader parsed;
  // The creators receive the finished container; both stage flags are 0x102A110x.
  if (!ParseShaderContainerHeader(header, SIZE_MAX, parsed) || parsed.is_vertex != vertex ||
      parsed.virtual_size < 36 || parsed.total_size() > 65536)
    return nullptr;
  const uint8_t* bytes = access.Readable(source, uint32_t(parsed.total_size()));
  if (!bytes) return nullptr;
  auto capture = std::make_shared<graphics::guest::ShaderCapture>();
  capture->vertex = vertex;
  capture->container.assign(bytes, bytes + parsed.total_size());
  capture->hash = graphics::shaders::ContainerKey(capture->container);
  capture->dynamic_vertex_fetch = vertex && ShaderHasInstanceData(capture->container.data(), parsed.virtual_size);
  return capture;
}
}  // namespace superman_returns::native
