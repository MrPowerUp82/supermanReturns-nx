// Native presentation: Vulkan binding for ClearSequence. Only route C1 of the NFSMW
// reference (Presentar/LimpiarSalida/DestruirVulkan, revision df2de32) is adapted: an opaque
// black clear of the SDK guest output image, no scene, destinations, video or gamma.

#include "sr_native_present.h"

#include <rex/logging.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/windowed_app_context.h>

#include <array>
#include <chrono>

namespace sr::native {
namespace {

using Clock = std::chrono::steady_clock;
using rex::ui::vulkan::VulkanDevice;
using rex::ui::vulkan::VulkanPresenter;

// The refresh image is a fixed 1280x720 target in this milestone: the guest swap size only
// defines the aspect ratio. This avoids the SDK's size-change tracker waits and no scene is
// drawn into it yet.
constexpr uint32_t kOutputWidth = 1280;
constexpr uint32_t kOutputHeight = 720;
constexpr uint64_t kFenceSliceNs = 50'000'000;  // 50 ms per bounded slice.
constexpr unsigned kTeardownSlices = 100;       // About 5 s before shutdown gives up on the GPU.
constexpr auto kReportInterval = std::chrono::seconds(5);

}  // namespace

struct NativePresentation::State {
  struct Framebuffer {
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    uint64_t version = 0;
  };

  std::function<bool()> cancelled;
  rex::ui::WindowedAppContext* app_context = nullptr;
  std::unique_ptr<rex::ui::vulkan::VulkanProvider> provider;
  std::unique_ptr<rex::ui::Presenter> presenter;

  VkRenderPass render_pass = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer commands = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  std::array<Framebuffer, 4> framebuffers;
  size_t next_framebuffer = 0;
  VkImageView current_view = VK_NULL_HANDLE;  // Valid only inside the refresh callback.
  std::unique_ptr<ClearSequence> sequence;
  Clock::time_point last_report{};

  const VulkanDevice* device() const { return provider ? provider->vulkan_device() : nullptr; }

  bool CreateRenderPass() {
    const VulkanDevice* d = device();
    if (!d) return false;
    VkAttachmentDescription attachment{};
    attachment.format = VulkanPresenter::kGuestOutputFormat;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;  // The contents are cleared.
    attachment.finalLayout = VulkanPresenter::kGuestOutputInternalLayout;
    const VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    // Acquire from and release to the presenter's use of the image (sampled in a fragment shader).
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
    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments = &attachment;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = uint32_t(dependencies.size());
    info.pDependencies = dependencies.data();
    if (d->functions().vkCreateRenderPass(d->device(), &info, nullptr, &render_pass) != VK_SUCCESS) {
      render_pass = VK_NULL_HANDLE;
      return false;
    }
    return true;
  }

  bool CreateCommandResources() {
    const VulkanDevice* d = device();
    if (!d) return false;
    const auto& dfn = d->functions();
    const VkDevice vk_device = d->device();
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = d->queue_family_graphics_compute();
    if (dfn.vkCreateCommandPool(vk_device, &pool_info, nullptr, &pool) != VK_SUCCESS) {
      pool = VK_NULL_HANDLE;
      return false;
    }
    VkCommandBufferAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (dfn.vkAllocateCommandBuffers(vk_device, &allocate, &commands) != VK_SUCCESS ||
        dfn.vkCreateFence(vk_device, &fence_info, nullptr, &fence) != VK_SUCCESS) {
      // Nothing was submitted yet: undo this partial creation so the caller sees all-or-nothing.
      DestroyCommandResources();
      return false;
    }
    return true;
  }

  void DestroyCommandResources() {
    const VulkanDevice* d = device();
    if (!d) return;
    const auto& dfn = d->functions();
    if (fence != VK_NULL_HANDLE) dfn.vkDestroyFence(d->device(), fence, nullptr);
    fence = VK_NULL_HANDLE;
    if (pool != VK_NULL_HANDLE) dfn.vkDestroyCommandPool(d->device(), pool, nullptr);
    pool = VK_NULL_HANDLE;  // Destroying the pool frees its command buffer.
    commands = VK_NULL_HANDLE;
  }

  bool CreateFramebuffer(uint64_t version) {
    const VulkanDevice* d = device();
    if (!d || current_view == VK_NULL_HANDLE) return false;
    for (const Framebuffer& f : framebuffers) {
      if (f.framebuffer != VK_NULL_HANDLE && f.version == version) return true;
    }
    const auto& dfn = d->functions();
    Framebuffer& slot = framebuffers[next_framebuffer];
    next_framebuffer = (next_framebuffer + 1) % framebuffers.size();
    // The sequence never creates a framebuffer while a submission is unresolved, so the one
    // being replaced is not in use.
    if (slot.framebuffer != VK_NULL_HANDLE) dfn.vkDestroyFramebuffer(d->device(), slot.framebuffer, nullptr);
    slot = {};
    VkFramebufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = render_pass;
    info.attachmentCount = 1;
    info.pAttachments = &current_view;
    info.width = kOutputWidth;
    info.height = kOutputHeight;
    info.layers = 1;
    if (dfn.vkCreateFramebuffer(d->device(), &info, nullptr, &slot.framebuffer) != VK_SUCCESS) {
      slot.framebuffer = VK_NULL_HANDLE;
      return false;
    }
    slot.version = version;
    return true;
  }

  void DestroyFramebuffers() {
    const VulkanDevice* d = device();
    if (!d) return;
    for (Framebuffer& f : framebuffers) {
      if (f.framebuffer != VK_NULL_HANDLE) d->functions().vkDestroyFramebuffer(d->device(), f.framebuffer, nullptr);
      f = {};
    }
  }

  SubmitResult RecordAndSubmit(uint64_t version) {
    const VulkanDevice* d = device();
    if (!d) return SubmitResult::kFailed;
    const auto& dfn = d->functions();
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    for (const Framebuffer& f : framebuffers) {
      if (f.framebuffer != VK_NULL_HANDLE && f.version == version) framebuffer = f.framebuffer;
    }
    if (framebuffer == VK_NULL_HANDLE) return SubmitResult::kFailed;
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dfn.vkBeginCommandBuffer(commands, &begin) != VK_SUCCESS) return SubmitResult::kFailed;
    VkClearValue black{};  // Opaque black.
    black.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = render_pass;
    pass.framebuffer = framebuffer;
    pass.renderArea.extent = {kOutputWidth, kOutputHeight};
    pass.clearValueCount = 1;
    pass.pClearValues = &black;
    dfn.vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
    dfn.vkCmdEndRenderPass(commands);
    if (dfn.vkEndCommandBuffer(commands) != VK_SUCCESS) return SubmitResult::kFailed;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    VkResult result;
    {
      // The refresher must use graphics/compute queue 0, under the device's queue lock.
      const auto queue = d->AcquireQueue(d->queue_family_graphics_compute(), 0);
      result = dfn.vkQueueSubmit(queue.queue(), 1, &submit, fence);
    }
    if (result == VK_SUCCESS) return SubmitResult::kOk;
    return result == VK_ERROR_DEVICE_LOST ? SubmitResult::kDeviceLost : SubmitResult::kFailed;
  }

  FenceWait WaitFence() {
    const VulkanDevice* d = device();
    if (!d) return FenceWait::kFailed;
    switch (d->functions().vkWaitForFences(d->device(), 1, &fence, VK_TRUE, kFenceSliceNs)) {
      case VK_SUCCESS: return FenceWait::kComplete;
      case VK_TIMEOUT: return FenceWait::kTimedOut;
      case VK_ERROR_DEVICE_LOST: return FenceWait::kDeviceLost;
      default: return FenceWait::kFailed;
    }
  }

  void ResetForReuse() {
    const VulkanDevice* d = device();
    if (!d) return;
    d->functions().vkResetFences(d->device(), 1, &fence);
    d->functions().vkResetCommandPool(d->device(), pool, 0);
  }

  void DestroyRenderPass() {
    const VulkanDevice* d = device();
    if (d && render_pass != VK_NULL_HANDLE) d->functions().vkDestroyRenderPass(d->device(), render_pass, nullptr);
    render_pass = VK_NULL_HANDLE;
  }

  void Report(const char* what) {
    const auto now = Clock::now();
    if (last_report != Clock::time_point{} && now - last_report < kReportInterval) return;
    last_report = now;
    REXLOG_WARN("[sr-native] present: {}", what);
  }

  ClearOps MakeOps() {
    ClearOps ops;
    ops.create_render_pass = [this] { return CreateRenderPass(); };
    ops.create_command_resources = [this] { return CreateCommandResources(); };
    ops.create_framebuffer = [this](uint64_t version) { return CreateFramebuffer(version); };
    ops.record_and_submit = [this] { return RecordAndSubmit(version_in_use); };
    ops.wait_fence = [this] { return WaitFence(); };
    ops.reset_for_reuse = [this] { ResetForReuse(); };
    ops.destroy_framebuffers = [this] { DestroyFramebuffers(); };
    ops.destroy_command_resources = [this] { DestroyCommandResources(); };
    ops.destroy_render_pass = [this] { DestroyRenderPass(); };
    ops.cancelled = cancelled;
    ops.report = [this](const char* what) { Report(what); };
    return ops;
  }

  uint64_t version_in_use = 0;
};

NativePresentation::NativePresentation(std::function<bool()> cancelled) : state_(new State) {
  state_->cancelled = std::move(cancelled);
  state_->sequence = std::make_unique<ClearSequence>(state_->MakeOps());
}

NativePresentation::~NativePresentation() {
  if (state_ && !Shutdown()) {
    // The GPU did not finish: freeing the owners now would free memory it still uses.
    // The process is ending; leak them deliberately and say so.
    REXLOG_ERROR("[sr-native] presentation shutdown could not drain GPU work; resources retained");
    (void)state_.release();
  }
}

bool NativePresentation::Initialize(rex::ui::WindowedAppContext* app_context) {
  State& s = *state_;
  if (s.presenter) return true;
  s.app_context = app_context;
  if (!s.provider) {
    s.provider = rex::ui::vulkan::VulkanProvider::Create(true, true);
    if (!s.provider) {
      REXLOG_ERROR("[sr-native] unable to create the Vulkan device");
      return false;
    }
  }
  auto create = [&s]() {
    s.presenter = s.provider->CreatePresenter();
    // Painting stays on the UI thread: the ring thread must never block on the host surface.
    if (s.presenter) s.presenter->SetPaintFromUIThreadOnly(true);
  };
  if (app_context) {
    app_context->CallInUIThreadSynchronous(create);
  } else {
    create();
  }
  if (!s.presenter) {
    REXLOG_ERROR("[sr-native] unable to create the presenter");
    s.provider.reset();
    return false;
  }
  return true;
}

bool NativePresentation::PresentClear(uint32_t width, uint32_t height) {
  State& s = *state_;
  if (!s.presenter || !s.provider || !width || !height) return false;
  bool presented = false;
  s.presenter->RefreshGuestOutput(
      kOutputWidth, kOutputHeight, width, height,
      [&s, &presented](rex::ui::Presenter::GuestOutputRefreshContext& context) {
        auto& vulkan_context = static_cast<VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
        s.current_view = vulkan_context.image_view();
        s.version_in_use = vulkan_context.image_version();
        const ClearResult result = s.sequence->Clear(s.version_in_use);
        s.current_view = VK_NULL_HANDLE;
        switch (result) {
          case ClearResult::kPresented:
            context.SetIs8bpc(true);
            presented = true;
            return true;
          case ClearResult::kDeviceLost:
            REXLOG_ERROR("[sr-native] Vulkan device lost during the clear");
            rex::ui::Presenter::FatalErrorHostGpuLossCallback(true, false);
            return false;
          case ClearResult::kCancelled: return false;
          case ClearResult::kFailed:
            s.Report("the clear failed; the swap stays blocked");
            return false;
        }
        return false;
      });
  return presented;
}

bool NativePresentation::Shutdown() {
  if (!state_) return true;
  State& s = *state_;
  // Our objects first: the framebuffers reference views owned by the presenter.
  if (s.sequence && !s.sequence->Teardown(kTeardownSlices)) return false;
  if (s.presenter) {
    if (s.app_context) {
      s.app_context->CallInUIThreadSynchronous([&s]() { s.presenter.reset(); });
    }
    s.presenter.reset();  // The presenter's destructor awaits its own submissions.
  }
  s.provider.reset();
  return true;
}

rex::ui::GraphicsProvider* NativePresentation::provider() const {
  return state_ ? state_->provider.get() : nullptr;
}
rex::ui::Presenter* NativePresentation::presenter() const {
  return state_ ? state_->presenter.get() : nullptr;
}
uint64_t NativePresentation::surface_paints() const {
  return state_ && state_->presenter ? state_->presenter->surface_paints() : 0;
}

}  // namespace sr::native
