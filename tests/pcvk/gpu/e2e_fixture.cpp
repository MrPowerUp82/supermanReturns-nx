// End to end on a real Vulkan device: a synthetic guest (D3D device state, PM4 ring bytes, surface
// and texture objects) is driven through the hooked-call entry points of the front end, decoded
// into render packets and recorded by the production renderer. The frame the guest swaps is read
// back and its pixels checked. Nothing here is the game; the shaders are two lines of HLSL.
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include "context.h"
#include "fake_guest.h"
#include "game_frame.h"
#include "pcvk/graphics/guest/texture_layout.h"
#include "pcvk/graphics/guest/render_packet.h"
#include "pcvk/native_renderer/frontend.h"
#include "spirv.h"

using namespace superman_returns;
using namespace superman_returns::graphics::vulkan;
namespace guest = superman_returns::graphics::guest;
namespace shaders = superman_returns::graphics::shaders;
namespace profile = superman_returns::native::profile;
using fake::FakeGuest;

namespace {
void Require(bool ok, const std::string& what) { if (!ok) throw std::runtime_error(what); }
void Require(bool ok, const Error& e) { if (!ok) throw std::runtime_error(e.operation + ": " + e.message); }

constexpr uint32_t kSurface = 0x20000;      // render target object
constexpr uint32_t kTexture = 0x24000;      // resolve destination / frontbuffer texture object
constexpr uint32_t kVertexShader = 0x30000, kPixelShader = 0x30400;
constexpr uint32_t kTexturePhysical = 0x200000;

void SetReg(FakeGuest& g, uint32_t reg, uint32_t value) {
  for (const auto& range : profile::kRegisterShadow)
    if (reg >= range.first && reg < range.first + range.count) {
      g.Put32(fake::kDevice + range.offset + 4 * (reg - range.first), value);
      return;
    }
  throw std::runtime_error("register not in the shadow");
}

struct Result { std::vector<uint8_t> pixels; uint32_t width = 0, height = 0; };

Result ReadBack(Context& c, GameFrame& game, const std::shared_ptr<TextureResource>& image) {
  Error e;
  c.f.vkDeviceWaitIdle(c.device);
  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.queueFamilyIndex = c.graphics_family;
  VkCommandPool pool{};
  Require(c.f.vkCreateCommandPool(c.device, &pool_info, nullptr, &pool) == VK_SUCCESS, "pool");
  VkCommandBuffer command{};
  VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocate.commandPool = pool; allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocate.commandBufferCount = 1;
  Require(c.f.vkAllocateCommandBuffers(c.device, &allocate, &command) == VK_SUCCESS, "command");
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  Require(c.f.vkBeginCommandBuffer(command, &begin) == VK_SUCCESS, "begin");
  const VkDeviceSize bytes = VkDeviceSize(image->extent.width) * image->extent.height * 4;
  VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer_info.size = bytes; buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkBuffer buffer{};
  Require(c.f.vkCreateBuffer(c.device, &buffer_info, nullptr, &buffer) == VK_SUCCESS, "buffer");
  VkMemoryRequirements requirements{};
  c.f.vkGetBufferMemoryRequirements(c.device, buffer, &requirements);
  std::vector<VkMemoryPropertyFlags> flags;
  for (uint32_t i = 0; i < c.memory.memoryTypeCount; ++i) flags.push_back(c.memory.memoryTypes[i].propertyFlags);
  auto type = ChooseMemoryType(requirements.memoryTypeBits, flags, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
  Require(type.has_value(), "host visible coherent memory");
  VkMemoryAllocateInfo memory_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  memory_info.allocationSize = requirements.size; memory_info.memoryTypeIndex = *type;
  VkDeviceMemory memory{};
  Require(c.f.vkAllocateMemory(c.device, &memory_info, nullptr, &memory) == VK_SUCCESS, "memory");
  Require(c.f.vkBindBufferMemory(c.device, buffer, memory, 0) == VK_SUCCESS, "bind");
  Require(game.Renderer().Images().Transition(command, image->handle, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, ImageUsage::TransferSource(), e), e);
  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {image->extent.width, image->extent.height, 1};
  c.f.vkCmdCopyImageToBuffer(command, image->handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
  Require(c.f.vkEndCommandBuffer(command) == VK_SUCCESS, "end");
  VkFence fence{};
  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  Require(c.f.vkCreateFence(c.device, &fence_info, nullptr, &fence) == VK_SUCCESS, "fence");
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
  Require(c.f.vkQueueSubmit(c.graphics_queue, 1, &submit, fence) == VK_SUCCESS, "submit");
  Require(c.f.vkWaitForFences(c.device, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS, "wait");
  Result result;
  result.width = image->extent.width; result.height = image->extent.height;
  result.pixels.resize(bytes);
  void* mapped = nullptr;
  Require(c.f.vkMapMemory(c.device, memory, 0, bytes, 0, &mapped) == VK_SUCCESS, "map");
  std::memcpy(result.pixels.data(), mapped, bytes);
  c.f.vkUnmapMemory(c.device, memory);
  c.f.vkDestroyFence(c.device, fence, nullptr);
  c.f.vkDestroyBuffer(c.device, buffer, nullptr);
  c.f.vkFreeMemory(c.device, memory, nullptr);
  c.f.vkDestroyCommandPool(c.device, pool, nullptr);
  return result;
}

std::array<uint8_t, 4> At(const Result& r, uint32_t x, uint32_t y) {
  const size_t i = (size_t(y) * r.width + x) * 4;
  return {r.pixels[i], r.pixels[i + 1], r.pixels[i + 2], r.pixels[i + 3]};
}
}  // namespace

int main() {
  try {
    Context c;
    Error e;
    c.logger = [](const std::string& s) { std::cout << s << '\n'; };
    Require(c.CreateInstance({}, true, e), e);
    VkPhysicalDeviceFeatures features{};
    features.shaderSampledImageArrayDynamicIndexing = features.shaderStorageBufferArrayDynamicIndexing =
        features.independentBlend = features.shaderClipDistance = features.shaderCullDistance =
            features.robustBufferAccess = VK_TRUE;
    Require(c.OpenOffscreenDevice("", e, &features, false), e);

    auto vertex = std::make_shared<shaders::CompiledShader>(), pixel = std::make_shared<shaders::CompiledShader>();
    pixel->stage = shaders::ShaderStage::kPixel;
    Require(ReadSpirv(std::filesystem::path(SR_E2E_SHADER_DIR) / "vs.spv", vertex->words, e), e);
    Require(ReadSpirv(std::filesystem::path(SR_E2E_SHADER_DIR) / "ps.spv", pixel->words, e), e);
    std::mutex queue_mutex;
    GameFrame game(c, queue_mutex, [&](const guest::ShaderCapture& capture) {
      return shaders::ShaderResult{shaders::ShaderPoll::ready, capture.vertex ? vertex : pixel, {}};
    }, [](const guest::TextureCapture&, guest::LinearTexture&, std::string& why) { why = "no textures in this test"; return false; });
    game.skip_failed_draws = true;
    Require(game.Initialize({}, e), e);

    // ---- the synthetic guest -------------------------------------------------------------
    FakeGuest g;
    const uint32_t dev = fake::kDevice;
    g.Put32(dev + profile::kDevice.ring_write, fake::kRing - 4);
    g.Put32(dev + profile::kDevice.render_targets, kSurface);
    g.Put32(kSurface + 0x18, 16);                               // pitch
    g.Put32(kSurface + 0x1C, 0);                                // EDRAM base 0, format k_8_8_8_8
    g.Put32(kSurface + 0x24, (15u << 18) | (15u << 3));         // 16 x 16
    g.Put32(dev + profile::kDevice.vertex_decl, fake::kDecl);   // an empty declaration: no vertex streams
    g.Put32(fake::kDecl + 0x18, 0);
    g.Put32(dev + profile::kDevice.shader_a, kVertexShader);
    g.Put32(dev + profile::kDevice.shader_b, kPixelShader);
    // Render state shadow, as in the renderer's own fixtures: all channels, depth off.
    SetReg(g, 0x2104, 0xF);
    SetReg(g, 0x2201, 1 | (1 << 16));
    // The frontbuffer: a 16 x 16 texture the game resolves into (word 1 carries the base address).
    const uint32_t fetch = kTexture + 0x1C;
    g.Put32(fetch + 0, 2);
    g.Put32(fetch + 4, kTexturePhysical | 6);
    g.Put32(fetch + 8, 15u | (15u << 13));
    g.Put32(fetch + 12, 0);
    g.Put32(fetch + 16, 0);
    g.Put32(fetch + 20, 1u << 9);

    auto make_shader = [](bool vertex_stage) {
      auto capture = std::make_shared<guest::ShaderCapture>();
      capture->vertex = vertex_stage;
      capture->container.assign(96, vertex_stage ? 1 : 2);
      capture->hash = vertex_stage ? 1 : 2;
      return capture;
    };
    auto vs_capture = make_shader(true), ps_capture = make_shader(false);

    std::mutex sink_mutex;
    std::shared_ptr<TextureResource> presented;
    std::string sink_error;
    superman_returns::native::Frontend frontend(g, [] {
      superman_returns::native::FrontendOptions o; o.worker_lag = false; return o; }());
    frontend.SetLoggers({}, [](const std::string& s) { std::cout << "frontend warning: " << s << '\n'; });
    frontend.SetShaderLookup([&](uint32_t object) -> std::shared_ptr<const guest::ShaderCapture> {
      return object == kVertexShader ? vs_capture : object == kPixelShader ? ps_capture : nullptr;
    });
    Require(frontend.Start([&](guest::RenderPacket&& packet, std::string& diagnostic) {
      std::shared_ptr<TextureResource> image;
      Error error;
      if (!game.Enqueue(std::move(packet), image, error)) { diagnostic = error.operation + ": " + error.message; sink_error = diagnostic; return false; }
      if (image) { std::lock_guard<std::mutex> lock(sink_mutex); presented = image; }
      return true;
    }), "front end start");
    frontend.NoteGuestDevice(dev);
    frontend.SyncRing(g.mem.data(), dev);

    // The XDK writes the scissor registers into the command segment before the draw.
    uint32_t ring = fake::kRing;
    auto put_ring = [&](std::initializer_list<uint32_t> words) {
      for (uint32_t w : words) { g.Put32(ring, w); ring += 4; }
      g.Put32(dev + profile::kDevice.ring_write, ring - 4);
    };
    // Type-0 packet: two registers starting at PA_SC_WINDOW_SCISSOR_TL (0x2081): (4,4) .. (8,8).
    put_ring({0x00010000u | 0x2081u, 4u | (4u << 16), 8u | (8u << 16)});

    const float red[4] = {1, 0, 0, 1};
    frontend.Clear(g.mem.data(), 0, 0, 0x1, red, 1.0f, 0);   // color target 0 only
    frontend.DrawVertices(g.mem.data(), 4, 0, 3);            // the fullscreen triangle, scissored
    frontend.Resolve(g.mem.data(), 0x0, 0, kTexture, 0, 0, 1.0f, 0, 0, 0);
    frontend.OnSwap(g.mem.data(), kTexture, 1);
    frontend.Stop();

    Require(sink_error.empty(), "renderer: " + sink_error);
    Require(frontend.stats().decode_failures.load() == 0, "a command failed to decode");
    Require(frontend.stats().capture_failures.load() == 0, "a command failed to capture");
    Require(presented != nullptr, "the swap produced no frame");
    auto frame = ReadBack(c, game, presented);
    Require(frame.width == 16 && frame.height == 16, "frontbuffer size");
    const auto green = std::array<uint8_t, 4>{0, 255, 0, 255}, clear = std::array<uint8_t, 4>{255, 0, 0, 255};
    Require(At(frame, 5, 5) == green, "pixel (5,5) should be the drawn green");
    Require(At(frame, 1, 1) == clear, "pixel (1,1) should keep the clear color (outside the scissor)");
    Require(At(frame, 12, 12) == clear, "pixel (12,12) should keep the clear color (outside the scissor)");
    std::cout << "End to end: hooked D3D calls -> front end -> render packets -> Vulkan -> resolved frontbuffer passed"
              << "; validation_errors=" << c.validation_errors.load() << '\n';
    return c.validation_errors.load() ? 1 : 0;
  } catch (const std::exception& ex) {
    std::cerr << "ERROR " << ex.what() << '\n';
    return 1;
  }
}
