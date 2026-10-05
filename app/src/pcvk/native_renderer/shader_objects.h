// Guest shader object -> the original container, captured when the XDK creates the object
// (before Direct3D patches its copy at bind time). The front end attaches these captures to
// the draws that use the object; the pack lookup identifies them by content.
#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include "../graphics/guest/shader_capture.h"
#include "frontend.h"
namespace superman_returns::native {
class ShaderObjects {
 public:
  void Remember(uint32_t object, std::shared_ptr<const graphics::guest::ShaderCapture> capture);
  void Forget(uint32_t object);
  std::shared_ptr<const graphics::guest::ShaderCapture> Find(uint32_t object) const;
  size_t size() const;
 private:
  mutable std::mutex mutex_;
  std::unordered_map<uint32_t, std::shared_ptr<const graphics::guest::ShaderCapture>> objects_;
};
// Reads the finished container the XDK passes to its shader creators (flags, virtual size,
// physical size, then both parts) and validates it. Null when `source` is not a container of
// the expected stage.
std::shared_ptr<const graphics::guest::ShaderCapture> CaptureShaderContainer(GuestAccess& access,
                                                                          uint32_t source,
                                                                          bool vertex);
}  // namespace superman_returns::native
