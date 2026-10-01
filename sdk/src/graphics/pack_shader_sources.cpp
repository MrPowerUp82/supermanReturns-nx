// Offline shader pack registry (see pack_shader_sources.h). Part of rexcore so the app
// and the GPU plugin share the same list.

#include <rex/graphics/pack_shader_sources.h>

#include <utility>

REXCVAR_DEFINE_STRING(pack_shaders, "identify", "GPU",
                      "Offline-translated shader pack registered by the app: off, identify (count "
                      "the shaders and draws it covers, draw with Xenos) or draw (draw with the "
                      "pack SPIR-V where supported, Xenos otherwise)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::graphics {
namespace {
std::vector<PackShaderSource>& Sources() {
  static std::vector<PackShaderSource> sources;
  return sources;
}
}  // namespace

void SetPackShaderSources(std::vector<PackShaderSource> sources) {
  Sources() = std::move(sources);
}

const std::vector<PackShaderSource>& GetPackShaderSources() {
  return Sources();
}

}  // namespace rex::graphics
