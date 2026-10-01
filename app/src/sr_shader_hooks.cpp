#include "sr_shader_registry.h"
#include "generated/default/superman_returns_init.h"
#include <rex/filesystem.h>
#include <rex/graphics/pack_shader_sources.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/system/kernel_state.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <unordered_set>
#include <rex/cvar.h>

namespace sr::native {
void InitializeRuntimeShaders() {
  try {
    RuntimeShaders().Load(rex::filesystem::GetExecutableFolder() / "superman_returns_shaders.srsp");
  } catch (const std::exception& error) {
    REXLOG_WARN("SR shader library unavailable: {}; draws use Xenos", error.what());
    return;
  }
  // The registry is immutable from here on, so the Vulkan backend can keep spans into it.
  std::vector<rex::graphics::PackShaderSource> sources;
  sources.reserve(RuntimeShaders().size());
  for (const Shader& shader : RuntimeShaders().shaders())
    sources.push_back({shader.original, shader.spirv, shader.vertices});
  rex::graphics::SetPackShaderSources(std::move(sources));
  REXLOG_INFO("SR shader library: {} entries loaded; pack_shaders = {}", RuntimeShaders().size(),
              REXCVAR_GET(pack_shaders));
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

// Shaders the game creates at run time (2026-10-01 console dump): 25 of the 27
// microcodes loaded at the EA logo exist nowhere in the game files, not even as
// 12-byte fragments, while the files only hold 0x102A11xx containers; the AST
// archives (BGFA1.05) evidently store them in a form the offline scanner can't read.
// So containers are captured where the game hands them over. Every hook below only
// reads and always runs the original function.
REXCVAR_DEFINE_BOOL(sr_dump_shader_containers, false, "Superman Returns",
                    "Write every shader container the game hands to the hooked functions to "
                    "shader_containers/ next to the NRO (for building the offline shader library)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace sr::native {
namespace {
std::mutex g_capture_mutex;
std::unordered_set<uint64_t> g_capture_seen;
uint32_t g_capture_known = 0;

uint64_t Fnv1a(std::span<const uint8_t> bytes) {
  uint64_t hash = 0xCBF29CE484222325ull;
  for (uint8_t b : bytes) hash = (hash ^ b) * 0x100000001B3ull;
  return hash;
}

// A whole container at a guest address, or an empty span.
std::span<const uint8_t> ContainerAt(uint32_t address, uint32_t limit = 65536) {
  const auto* header = Readable(address, 12);
  if (!header || (BE(header) & ~1u) != 0x102A1100u) return {};
  const uint64_t size = uint64_t(BE(header + 4)) + BE(header + 8);
  if (size < 36 || size > 65536 || size > limit) return {};
  const auto* bytes = Readable(address, uint32_t(size));
  return bytes ? std::span<const uint8_t>(bytes, size_t(size)) : std::span<const uint8_t>();
}

void Capture(std::span<const uint8_t> container, const char* source) {
  if (container.empty()) return;
  const uint64_t hash = Fnv1a(container);
  const bool vertex = BE(container.data()) & 1;
  {
    std::lock_guard lock(g_capture_mutex);
    if (!g_capture_seen.insert(hash).second) return;
  }
  const Shader* known = RuntimeShaders().size() ? RuntimeShaders().Identify(container) : nullptr;
  uint32_t distinct, in_library;
  {
    std::lock_guard lock(g_capture_mutex);
    g_capture_known += known ? 1 : 0;
    distinct = uint32_t(g_capture_seen.size());
    in_library = g_capture_known;
  }
  REXLOG_INFO("SR shader container {} {:016X} via {}: {} bytes, {} ({} distinct, {} in the library)",
              vertex ? "vertex" : "pixel", hash, source, container.size(),
              known ? "in the library" : "not in the library", distinct, in_library);
  if (!REXCVAR_GET(sr_dump_shader_containers)) return;
  const auto folder = rex::filesystem::GetExecutableFolder() / "shader_containers";
  std::error_code error;
  std::filesystem::create_directories(folder, error);
  const auto path = folder / fmt::format("{:016X}.{}.bin", hash, vertex ? "v" : "p");
  if (FILE* file = std::fopen(path.string().c_str(), "wb")) {
    std::fwrite(container.data(), 1, container.size(), file);
    std::fclose(file);
  } else {
    REXLOG_WARN("SR shader container: could not write {}", path.string());
  }
}

// First call of each hook, to tell "never called" from "called without containers".
void NoteCall(std::atomic<bool>& seen, const char* name, uint32_t argument) {
  if (!seen.exchange(true))
    REXLOG_INFO("SR shader hook {} first called (argument {:08X})", name, argument);
}

void CaptureSafely(uint32_t address, uint32_t limit, const char* source) {
  try {
    Capture(ContainerAt(address, limit), source);
  } catch (const std::exception& error) {
    REXLOG_WARN("SR shader container capture via {} failed: {}", source, error.what());
  }
}
}  // namespace
}  // namespace sr::native

// RenderWare resource creation: r4 points to a type-15 descriptor whose +48/+52
// fields are the shader container pointer/byte size. The function returns the
// engine resource in r3. Verified statically in this title's generated PPC body:
// sub_82383AA8 checks the 0x102A1100 signature and selects stage by bit 0.
// This is an observation hook, not either of the unconfirmed D3D constructors.
REX_HOOK_RAW(sub_82383AA8) {
  static std::atomic<bool> called{false};
  sr::native::NoteCall(called, "82383AA8", ctx.r4.u32);
  const sr::native::Shader* shader = nullptr;
  try {
    shader = sr::native::IdentifyResource(ctx.r4.u32);
  } catch (const std::exception& error) {
    REXLOG_WARN("SR shader resource identification failed: {}", error.what());
  }
  if (const auto* resource = sr::native::Readable(ctx.r4.u32, 56);
      resource && sr::native::BE(resource + 4) == 15) {
    sr::native::CaptureSafely(sr::native::BE(resource + 48), sr::native::BE(resource + 52),
                              "82383AA8");
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

// sub_820F9C78 writes the containers of the shader compiler in the executable: r4 is
// the output object, whose byte stream sits at +20 (+0 data address, +4 position, +8
// capacity, +12 bytes written). It writes the body from the current position, then
// seeks back and writes the 36-byte header there; with a null data address it only
// measures. Verified statically in this title's generated PPC body.
REX_HOOK_RAW(sub_820F9C78) {
  static std::atomic<bool> called{false};
  sr::native::NoteCall(called, "820F9C78", ctx.r4.u32);
  const uint32_t output = ctx.r4.u32;
  uint32_t start = 0;
  bool have_start = false;
  if (const auto* stream = sr::native::Readable(output + 20, 8)) {
    start = sr::native::BE(stream + 4);
    have_start = true;
  }
  __imp__sub_820F9C78(ctx, base);
  if (!have_start || int32_t(ctx.r3.u32) < 0) return;
  if (const auto* stream = sr::native::Readable(output + 20, 16)) {
    const uint32_t data = sr::native::BE(stream), written = sr::native::BE(stream + 12);
    if (data && written > start) {
      sr::native::CaptureSafely(data + start, written - start, "820F9C78");
    }
  }
}

// D3DX shader helpers that check the container signature first and return
// D3DERR_INVALIDCALL (0x8876086C) without a container. The container is the first
// argument (r3) or the second (r4), per their generated PPC bodies.
#define SR_CONTAINER_HOOK(address, reg)                           \
  REX_HOOK_RAW(sub_##address) {                                   \
    static std::atomic<bool> called{false};                       \
    sr::native::NoteCall(called, #address, ctx.reg.u32);          \
    sr::native::CaptureSafely(ctx.reg.u32, 65536, #address);      \
    __imp__sub_##address(ctx, base);                              \
  }
SR_CONTAINER_HOOK(823AE558, r3)
SR_CONTAINER_HOOK(823B2530, r3)
SR_CONTAINER_HOOK(823B2C20, r3)
SR_CONTAINER_HOOK(823FC5A8, r4)
SR_CONTAINER_HOOK(823DBB38, r4)
