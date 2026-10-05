#include "sr_vk_present.h"

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/windowed_app_context.h>

#include <array>
#include <atomic>
#include <chrono>
#include <variant>

#include "pcvk/graphics/guest/texture_layout.h"
#include "pcvk/graphics/vulkan/composition.h"
#include "pcvk/graphics/vulkan/context.h"
#include "pcvk/graphics/vulkan/game_frame.h"

namespace sr::vk {
namespace {

namespace gv = superman_returns::graphics::vulkan;
namespace guest = superman_returns::graphics::guest;
namespace shaders = superman_returns::graphics::shaders;
using rex::ui::vulkan::VulkanDevice;
using rex::ui::vulkan::VulkanPresenter;
using Clock = std::chrono::steady_clock;

// Fixed 1280x720 guest output: the guest swap size only defines the displayed aspect ratio,
// which also keeps the SDK's size-change tracker out of the way.
constexpr uint32_t kOutputWidth = 1280;
constexpr uint32_t kOutputHeight = 720;
constexpr uint64_t kFenceSliceNs = 50'000'000;  // 50 ms per bounded slice
constexpr unsigned kTeardownSlices = 100;       // about 5 s before shutdown gives up on the GPU
constexpr auto kReportInterval = std::chrono::seconds(5);

}  // namespace

struct Presentation::State {
  struct Framebuffer {
    VkFramebuffer handle = VK_NULL_HANDLE;
    uint64_t version = 0;
  };

  std::function<bool()> cancelled;
  rex::ui::WindowedAppContext* app_context = nullptr;
  std::unique_ptr<rex::ui::vulkan::VulkanProvider> provider;
  std::unique_ptr<rex::ui::Presenter> presenter;

  gv::Context context;  // adopts the SDK device; owns nothing
  std::unique_ptr<gv::GameFrame> game;
  std::unique_ptr<gv::ResourceStore> resources;
  std::unique_ptr<gv::FrontbufferCompositor> compositor;
  bool dummies = false;
  VkRenderPass render_pass = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer command = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  std::array<Framebuffer, 4> framebuffers;
  size_t next_framebuffer = 0;
  uint64_t serial = 0;
  bool in_flight = false, lost = false, renderer_started = false;
  std::atomic<bool> cancel_requested{false};  // Cancel(): waits give up even before system shutdown starts
  PresentationStats stats;
  Clock::time_point last_report{};

  const VulkanDevice* device() const { return provider ? provider->vulkan_device() : nullptr; }

  void Report(const std::string& text) {
    const auto now = Clock::now();
    if (last_report != Clock::time_point{} && now - last_report < kReportInterval) return;
    last_report = now;
    REXLOG_WARN("[sr-vk] present: {}", text);
  }
  bool Fail(const gv::Error& e) {
    ++stats.failures;
    Report(fmt::format("{}: {} (VkResult {})", e.operation, e.message, int(e.result)));
    if (e.result == VK_ERROR_DEVICE_LOST) lost = true;
    return false;
  }

  bool CreateRenderPass(gv::Error& e) {
    VkAttachmentDescription attachment{};
    attachment.format = VulkanPresenter::kGuestOutputFormat;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // the composition covers the image
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VulkanPresenter::kGuestOutputInternalLayout;
    const VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    std::array<VkSubpassDependency, 2> dependencies{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VulkanPresenter::kGuestOutputInternalStageMask;
    dependencies[0].srcAccessMask = VulkanPresenter::kGuestOutputInternalAccessMask;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstStageMask = VulkanPresenter::kGuestOutputInternalStageMask;
    dependencies[1].dstAccessMask = VulkanPresenter::kGuestOutputInternalAccessMask;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = 1;
    info.pAttachments = &attachment;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = uint32_t(dependencies.size());
    info.pDependencies = dependencies.data();
    return gv::Check(context.f.vkCreateRenderPass(context.device, &info, nullptr, &render_pass),
                     "Guest output render pass", e);
  }

  bool CreateCommandResources(gv::Error& e) {
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.queueFamilyIndex = context.graphics_family;
    if (!gv::Check(context.f.vkCreateCommandPool(context.device, &pool_info, nullptr, &pool),
                   "Composition command pool", e))
      return false;
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    return gv::Check(context.f.vkAllocateCommandBuffers(context.device, &allocate, &command),
                     "Composition command buffer", e) &&
           gv::Check(context.f.vkCreateFence(context.device, &fence_info, nullptr, &fence),
                     "Composition fence", e);
  }

  // The refresh image is recreated by the presenter only when its size changes, so the
  // framebuffer for each image version is cached (and replaced when its view goes away).
  VkFramebuffer FramebufferFor(VkImageView view, uint64_t version, gv::Error& e) {
    for (const Framebuffer& f : framebuffers)
      if (f.handle != VK_NULL_HANDLE && f.version == version) return f.handle;
    Framebuffer& slot = framebuffers[next_framebuffer];
    next_framebuffer = (next_framebuffer + 1) % framebuffers.size();
    // Nothing is in flight here (the previous frame's fence was waited on).
    if (slot.handle != VK_NULL_HANDLE) context.f.vkDestroyFramebuffer(context.device, slot.handle, nullptr);
    slot = {};
    VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    info.renderPass = render_pass;
    info.attachmentCount = 1;
    info.pAttachments = &view;
    info.width = kOutputWidth;
    info.height = kOutputHeight;
    info.layers = 1;
    if (!gv::Check(context.f.vkCreateFramebuffer(context.device, &info, nullptr, &slot.handle),
                   "Guest output framebuffer", e)) {
      slot.handle = VK_NULL_HANDLE;
      return VK_NULL_HANDLE;
    }
    slot.version = version;
    return slot.handle;
  }

  enum class Wait { kDone, kCancelled, kLost, kFailed };
  // `honor_cancel` is false during teardown: the GPU must be drained even after Cancel().
  Wait WaitFence(unsigned max_slices, bool honor_cancel = true) {
    for (unsigned slice = 0; slice < max_slices; ++slice) {
      switch (context.f.vkWaitForFences(context.device, 1, &fence, VK_TRUE, kFenceSliceNs)) {
        case VK_SUCCESS: return Wait::kDone;
        case VK_TIMEOUT: break;
        case VK_ERROR_DEVICE_LOST: return Wait::kLost;
        default: return Wait::kFailed;
      }
      if (honor_cancel && (cancel_requested.load() || (cancelled && cancelled()))) return Wait::kCancelled;
    }
    return Wait::kCancelled;
  }

  // Composes `source` into the presenter's guest-output image. Runs inside the refresh
  // callback, which must use graphics/compute queue 0 under the device's queue lock.
  bool Compose(const std::shared_ptr<gv::TextureResource>& source,
               const std::array<uint32_t, 256>& gamma, bool gamma_enabled,
               VulkanPresenter::VulkanGuestOutputRefreshContext& output) {
    gv::Error e;
    const VkFramebuffer framebuffer = FramebufferFor(output.image_view(), output.image_version(), e);
    if (framebuffer == VK_NULL_HANDLE) return Fail(e);
    if (!gv::Check(context.f.vkResetCommandPool(context.device, pool, 0), "Reset composition pool", e) ||
        !gv::Check(context.f.vkResetFences(context.device, 1, &fence), "Reset composition fence", e))
      return Fail(e);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!gv::Check(context.f.vkBeginCommandBuffer(command, &begin), "Begin composition", e)) return Fail(e);
    const uint64_t frame_serial = ++serial;
    if (!resources->BeginSubmission(command, frame_serial, e)) return Fail(e);
    if (!dummies) {
      if (!resources->CreateDummies(e)) return Fail(e);
      dummies = true;
    }
    gv::TargetPass pass;
    pass.owns_handles = false;
    pass.render_pass = render_pass;
    pass.framebuffer = framebuffer;
    pass.extent = {kOutputWidth, kOutputHeight};
    pass.color_count = 1;
    pass.formats[0] = VulkanPresenter::kGuestOutputFormat;
    auto composition = compositor->Prepare(command, pass, source, *resources, game->Renderer().Images(),
                                           gamma, gamma_enabled, kOutputWidth, kOutputHeight, e);
    if (!composition) return Fail(e);
    VkRenderPassBeginInfo pass_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass_begin.renderPass = render_pass;
    pass_begin.framebuffer = framebuffer;
    pass_begin.renderArea.extent = {kOutputWidth, kOutputHeight};
    context.f.vkCmdBeginRenderPass(command, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    compositor->Record(command, *composition);
    context.f.vkCmdEndRenderPass(command);
    if (!gv::Check(context.f.vkEndCommandBuffer(command), "End composition", e)) return Fail(e);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    VkResult result;
    {
      const auto queue = device()->AcquireQueue(device()->queue_family_graphics_compute(), 0);
      result = context.f.vkQueueSubmit(queue.queue(), 1, &submit, fence);
    }
    if (!gv::Check(result, "Submit composition", e)) return Fail(e);
    in_flight = true;
    switch (WaitFence(unsigned(-1))) {
      case Wait::kDone:
        in_flight = false;
        resources->Retire(frame_serial);
        compositor->Retire(frame_serial);
        ++stats.composed;
        return true;
      case Wait::kLost:
        lost = true;
        e = {"Composition completion", VK_ERROR_DEVICE_LOST, "Vulkan device lost"};
        return Fail(e);
      case Wait::kCancelled: return false;
      case Wait::kFailed: break;
    }
    e = {"Composition completion", VK_ERROR_UNKNOWN, "fence wait failed"};
    return Fail(e);
  }

  bool Present(const std::shared_ptr<gv::TextureResource>& source, const std::array<uint32_t, 256>& gamma,
               bool gamma_enabled) {
    bool composed = false;
    presenter->RefreshGuestOutput(
        kOutputWidth, kOutputHeight, source->extent.width, source->extent.height,
        [&](rex::ui::Presenter::GuestOutputRefreshContext& base) {
          auto& output = static_cast<VulkanPresenter::VulkanGuestOutputRefreshContext&>(base);
          composed = Compose(source, gamma, gamma_enabled, output);
          if (composed) base.SetIs8bpc(true);
          else if (lost) rex::ui::Presenter::FatalErrorHostGpuLossCallback(true, false);
          return composed;
        });
    return composed;
  }

  bool Teardown() {
    if (in_flight) {
      // A submission is still unresolved: nothing it uses may be destroyed.
      if (WaitFence(kTeardownSlices, false) != Wait::kDone && !lost) return false;
      in_flight = false;
    }
    if (device()) context.f.vkDeviceWaitIdle(context.device);
    compositor.reset();
    resources.reset();
    game.reset();
    for (Framebuffer& f : framebuffers) {
      if (f.handle != VK_NULL_HANDLE) context.f.vkDestroyFramebuffer(context.device, f.handle, nullptr);
      f = {};
    }
    if (fence != VK_NULL_HANDLE) context.f.vkDestroyFence(context.device, fence, nullptr);
    if (pool != VK_NULL_HANDLE) context.f.vkDestroyCommandPool(context.device, pool, nullptr);
    if (render_pass != VK_NULL_HANDLE) context.f.vkDestroyRenderPass(context.device, render_pass, nullptr);
    fence = VK_NULL_HANDLE;
    pool = VK_NULL_HANDLE;
    command = VK_NULL_HANDLE;
    render_pass = VK_NULL_HANDLE;
    return true;
  }
};

Presentation::Presentation(std::function<bool()> cancelled) : state_(new State) {
  state_->cancelled = std::move(cancelled);
}

Presentation::~Presentation() {
  if (state_ && !Shutdown()) {
    REXLOG_ERROR("[sr-vk] presentation shutdown could not drain GPU work; resources retained");
    (void)state_.release();
  }
}

bool Presentation::Initialize(rex::ui::WindowedAppContext* app_context) {
  State& s = *state_;
  if (s.presenter) return true;
  s.app_context = app_context;
  if (!s.provider) {
    s.provider = rex::ui::vulkan::VulkanProvider::Create(true, true);
    if (!s.provider) {
      REXLOG_ERROR("[sr-vk] unable to create the Vulkan device");
      return false;
    }
  }
  auto create = [&s]() {
    s.presenter = s.provider->CreatePresenter();
    // Painting stays on the UI thread: the renderer thread must never block on the surface.
    if (s.presenter) s.presenter->SetPaintFromUIThreadOnly(true);
  };
  if (app_context) app_context->CallInUIThreadSynchronous(create);
  else create();
  if (!s.presenter) {
    REXLOG_ERROR("[sr-vk] unable to create the presenter");
    s.provider.reset();
    return false;
  }
  return true;
}

bool Presentation::StartRenderer(ShaderLookup lookup, const std::filesystem::path& driver_cache,
                                 std::string& error) {
  State& s = *state_;
  auto fail = [&](const gv::Error& e) {
    error = fmt::format("{}: {} (VkResult {})", e.operation, e.message, int(e.result));
    return false;
  };
  if (s.renderer_started) return true;
  const VulkanDevice* device = s.device();
  if (!device) {
    error = "no Vulkan device";
    return false;
  }
  const auto& p = device->properties();
  // The game renderer's shader ABI (sr-vulkan-buffers-v1) needs these; they are enabled when
  // the SDK device is created with vulkan_native_shader_features.
  if (!p.independentBlend || !p.shaderSampledImageArrayDynamicIndexing ||
      !p.shaderStorageBufferArrayDynamicIndexing || !p.shaderClipDistance || !p.shaderCullDistance ||
      !p.samplerMirrorClampToEdge) {
    error = fmt::format(
        "the Vulkan device lacks features the native renderer needs (independentBlend={}, "
        "sampledImageArrayDynamicIndexing={}, storageBufferArrayDynamicIndexing={}, clipDistance={}, "
        "cullDistance={}, samplerMirrorClampToEdge={}); set vulkan_native_shader_features=true",
        p.independentBlend, p.shaderSampledImageArrayDynamicIndexing, p.shaderStorageBufferArrayDynamicIndexing,
        p.shaderClipDistance, p.shaderCullDistance, p.samplerMirrorClampToEdge);
    return false;
  }
  const auto& instance = *device->vulkan_instance();
  gv::Context::ExternalDevice external;
  external.get_instance_proc = instance.functions().vkGetInstanceProcAddr;
  external.get_device_proc = instance.functions().vkGetDeviceProcAddr;
  external.instance = instance.instance();
  external.physical = device->physical_device();
  external.device = device->device();
  external.graphics_family = device->queue_family_graphics_compute();
  external.graphics_queue = device->queue_families()[external.graphics_family].queues[0]->queue;
  external.enabled_features.robustBufferAccess = p.robustBufferAccess;
  external.enabled_features.independentBlend = p.independentBlend;
  external.enabled_features.shaderClipDistance = p.shaderClipDistance;
  external.enabled_features.shaderCullDistance = p.shaderCullDistance;
  external.enabled_features.shaderSampledImageArrayDynamicIndexing = p.shaderSampledImageArrayDynamicIndexing;
  external.enabled_features.shaderStorageBufferArrayDynamicIndexing = p.shaderStorageBufferArrayDynamicIndexing;
  external.mirror_clamp_enabled = p.samplerMirrorClampToEdge;
  s.context.logger = [](const std::string& line) { REXLOG_INFO("[sr-vk] {}", line); };
  gv::Error e;
  if (!s.context.Adopt(external, e)) return fail(e);

  // The SDK guards its queues with a recursive mutex; the game renderer submits under the same one.
  auto& queue_mutex = device->queue_families()[external.graphics_family].queues[0]->mutex;
  gv::QueueLock queue_lock([&queue_mutex] { queue_mutex.lock(); }, [&queue_mutex] { queue_mutex.unlock(); });
  gv::TextureDecoder decoder = [](const guest::TextureCapture& capture, guest::LinearTexture& result,
                                  std::string& reason) {
    return guest::DecodeTextureLayout(capture.fetch, capture.memory, result, reason);
  };
  s.game = std::make_unique<gv::GameFrame>(s.context, std::move(queue_lock), std::move(lookup), std::move(decoder));
  s.game->skip_failed_draws = true;  // a draw the renderer cannot express costs that draw, not the session
  if (!s.game->Initialize(driver_cache, e)) return fail(e);
  s.resources = std::make_unique<gv::ResourceStore>(s.context);
  s.compositor = std::make_unique<gv::FrontbufferCompositor>(s.context);
  if (!s.compositor->Initialize(e) || !s.CreateRenderPass(e) || !s.CreateCommandResources(e)) return fail(e);
  s.renderer_started = true;
  REXLOG_INFO("[sr-vk] native Vulkan renderer ready on {}", s.context.selected.name);
  return true;
}

bool Presentation::Submit(guest::RenderPacket&& packet, std::string& diagnostic) {
  State& s = *state_;
  if (!s.game || s.lost) {
    diagnostic = s.lost ? "Vulkan device lost" : "renderer not started";
    return false;
  }
  std::shared_ptr<const std::array<uint32_t, 256>> gamma;
  bool gamma_enabled = false;
  if (const auto* swap = std::get_if<guest::SwapPacket>(&packet)) {
    gamma = swap->gamma;
    gamma_enabled = swap->gamma_enabled;
  }
  gv::Error e;
  std::shared_ptr<gv::TextureResource> image;
  if (!s.game->Enqueue(std::move(packet), image, e)) {
    diagnostic = e.operation + ": " + e.message;
    return false;
  }
  if (!image) return true;  // not a swap
  ++s.stats.frames;
  if (!s.presenter) {
    diagnostic = "no presenter";
    return false;
  }
  static const std::array<uint32_t, 256> kNoGamma{};
  // A composition that fails is counted and reported; only a lost device ends the renderer.
  s.Present(image, gamma ? *gamma : kNoGamma, gamma_enabled);
  if (s.lost) {
    diagnostic = "Vulkan device lost";
    return false;
  }
  return true;
}

void Presentation::Cancel() {
  if (!state_) return;
  state_->cancel_requested.store(true);
  if (state_->game) state_->game->Cancel();
}

bool Presentation::Shutdown() {
  if (!state_) return true;
  State& s = *state_;
  if (s.game) s.game->Cancel();
  if (!s.Teardown()) return false;
  if (s.presenter) {
    if (s.app_context) s.app_context->CallInUIThreadSynchronous([&s]() { s.presenter.reset(); });
    s.presenter.reset();  // the presenter's destructor awaits its own submissions
  }
  s.provider.reset();
  return true;
}

rex::ui::GraphicsProvider* Presentation::provider() const { return state_ ? state_->provider.get() : nullptr; }
rex::ui::Presenter* Presentation::presenter() const { return state_ ? state_->presenter.get() : nullptr; }
uint64_t Presentation::surface_paints() const {
  return state_ && state_->presenter ? state_->presenter->surface_paints() : 0;
}
const PresentationStats& Presentation::stats() const { return state_->stats; }

}  // namespace sr::vk
