#pragma once
/**
 * Shaders translated offline with XenosRecomp (a "pack"), handed by the app to the
 * Vulkan backend.
 *
 * The app owns the library file (it is made from the game's data and is not part of
 * the SDK). It registers each shader once, before the GPU system is created; the
 * Vulkan backend reads the list when it initializes and identifies the shaders the
 * guest loads with IM_LOAD against it. What the backend does with an identified
 * shader depends on the pack_shaders cvar:
 *   off      - nothing, the pack is ignored;
 *   identify - count identified shaders and draws, drawing everything with Xenos;
 *   draw     - draw with the pack SPIR-V where the draw is supported, Xenos otherwise.
 */

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <rex/cvar.h>

REXCVAR_DECLARE(std::string, pack_shaders);

namespace rex::graphics {

struct PackShaderSource {
  // The original container: 0x102A1100 (pixel) or 0x102A1101 (vertex) format, big-endian.
  std::span<const uint8_t> container;
  // Its XenosRecomp translation (shader_common.h interface), SPIR-V words.
  std::span<const uint32_t> spirv;
  bool vertex = false;
};

// Both spans must stay valid and unchanged for the rest of the process.
void SetPackShaderSources(std::vector<PackShaderSource> sources);
const std::vector<PackShaderSource>& GetPackShaderSources();

}  // namespace rex::graphics
