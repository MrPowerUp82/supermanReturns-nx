#include "sr_vk_runtime.h"

#include <atomic>

#include "pcvk/native_renderer/frontend.h"
#include "pcvk/native_renderer/shader_objects.h"

REXCVAR_DEFINE_BOOL(sr_vk, true, "Superman Returns",
                    "With sr_renderer=native: draw with the Vulkan renderer of the PC project (pcvk). "
                    "false keeps the milestone-1 black-clear presentation.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(sr_vk_shader_pack, "", "Superman Returns",
                      "Offline Vulkan shader pack (tools/vkshaders/build_pack.py); empty = "
                      "superman_returns_vulkan_shaders.srvk next to the NRO")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(sr_vk_pipeline_cache, "", "Superman Returns",
                      "Driver pipeline cache file of the native renderer; empty = "
                      "superman_returns_vulkan_pipelines.bin next to the NRO")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(sr_vk_worker_lag, true, "Superman Returns",
                    "Let the recording worker finish a frame while the guest builds the next one")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(sr_vk_texture_per_frame_max_kb, 4096, "Superman Returns",
                     "Textures up to this size are re-validated every guest frame (movie/UI textures "
                     "are written without any hook); larger ones less often")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(sr_vk_large_texture_recheck_frames, 30, "Superman Returns",
                     "Frames between re-validations of textures above sr_vk_texture_per_frame_max_kb")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace sr::vk {
namespace {
std::atomic<superman_returns::native::Frontend*> g_frontend{nullptr};
std::atomic<superman_returns::native::GuestAccess*> g_access{nullptr};
}  // namespace
superman_returns::native::Frontend* ActiveFrontend() { return g_frontend.load(std::memory_order_acquire); }
superman_returns::native::GuestAccess* ActiveGuestAccess() { return g_access.load(std::memory_order_acquire); }
void SetActive(superman_returns::native::Frontend* frontend, superman_returns::native::GuestAccess* access) {
  g_access.store(access, std::memory_order_release);
  g_frontend.store(frontend, std::memory_order_release);
}
superman_returns::native::ShaderObjects& ShaderObjectRegistry() {
  static superman_returns::native::ShaderObjects registry;
  return registry;
}
}  // namespace sr::vk
