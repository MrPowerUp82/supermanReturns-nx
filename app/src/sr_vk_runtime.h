#pragma once
// Process-wide handles the D3D hooks use to reach the native Vulkan renderer. The hooks are
// compiled into every build; they only do work while a front end is installed (sr_renderer =
// native with the Vulkan renderer enabled), otherwise each one just calls the original.
#include <rex/cvar.h>
#include <string>

namespace superman_returns::native {
class Frontend;
class GuestAccess;
class ShaderObjects;
}  // namespace superman_returns::native

namespace sr::vk {
// The active front end, or nullptr. Set before guest threads run, cleared before it is destroyed.
superman_returns::native::Frontend* ActiveFrontend();
superman_returns::native::GuestAccess* ActiveGuestAccess();
void SetActive(superman_returns::native::Frontend* frontend, superman_returns::native::GuestAccess* access);
// Guest shader objects created through the XDK creators (container captured at creation).
superman_returns::native::ShaderObjects& ShaderObjectRegistry();
}  // namespace sr::vk

REXCVAR_DECLARE(bool, sr_vk);
REXCVAR_DECLARE(std::string, sr_vk_shader_pack);
REXCVAR_DECLARE(std::string, sr_vk_pipeline_cache);
REXCVAR_DECLARE(bool, sr_vk_worker_lag);
REXCVAR_DECLARE(int32_t, sr_vk_texture_per_frame_max_kb);
REXCVAR_DECLARE(int32_t, sr_vk_large_texture_recheck_frames);
