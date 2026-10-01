#include "sr_shader_registry.h"
#include "generated/default/superman_returns_init.h"
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/system/kernel_state.h>
#include <atomic>

namespace sr::native {
void InitializeRuntimeShaders() {
  try {
    RuntimeShaders().Load(rex::filesystem::GetExecutableFolder() / "superman_returns_shaders.srsp");
    REXLOG_INFO("SR shader library: {} entries loaded; resource identification enabled; draws use Xenos",
                RuntimeShaders().size());
  } catch (const std::exception& error) {
    REXLOG_WARN("SR shader library unavailable: {}; draws use Xenos", error.what());
  }
}
namespace {
const uint8_t* Readable(uint32_t address, uint32_t size) {
  if (!address || !size || uint64_t(address) + size > UINT32_MAX) return nullptr;
  auto* state = rex::system::kernel_state();
  if (!state || !state->memory()) return nullptr;
  auto* memory = state->memory();
  auto* heap = memory->LookupHeap(address);
  if (!heap || memory->LookupHeap(address + size - 1) != heap) return nullptr;
  const auto access = heap->QueryRangeAccess(address, address + size - 1);
  if (!(uint32_t(access) & uint32_t(rex::memory::PageAccess::kReadOnly))) return nullptr;
  return memory->TranslateVirtual<const uint8_t*>(address);
}
uint32_t BE(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
const Shader* IdentifyResource(uint32_t source) {
  if (!RuntimeShaders().size()) return nullptr;
  const auto* resource = Readable(source, 56);
  if (!resource || BE(resource + 4) != 15) return nullptr;
  const uint32_t address = BE(resource + 48), size = BE(resource + 52);
  if (size < 36 || size > 65536) return nullptr;
  const auto* container = Readable(address, size);
  return container ? RuntimeShaders().Identify({container, size}) : nullptr;
}
}  // namespace
}  // namespace sr::native

// RenderWare resource creation: r4 points to a type-15 descriptor whose +48/+52
// fields are the shader container pointer/byte size. The function returns the
// engine resource in r3. Verified statically in this title's generated PPC body:
// sub_82383AA8 checks the 0x102A1100 signature and selects stage by bit 0.
// This is an observation hook, not either of the unconfirmed D3D constructors.
REX_HOOK_RAW(sub_82383AA8) {
  const sr::native::Shader* shader = nullptr;
  try { shader = sr::native::IdentifyResource(ctx.r4.u32); }
  catch (const std::exception& error) {
    REXLOG_WARN("SR shader resource identification failed: {}", error.what());
  }
  __imp__sub_82383AA8(ctx, base);
  try {
    sr::native::RuntimeShaders().RememberEngineResource(ctx.r3.u32, shader);
    static std::atomic<unsigned> reports{0};
    if (shader && ctx.r3.u32 && reports.fetch_add(1) < 16)
      REXLOG_INFO("SR shader resource {:08X}: {} container {:016X}", ctx.r3.u32,
                  shader->vertices ? "vertex" : "pixel", shader->huella);
  } catch (const std::exception& error) {
    REXLOG_WARN("SR shader resource registry failed: {}", error.what());
  }
}
