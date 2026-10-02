#pragma once

// Native presentation: an opaque black clear of the SDK guest output image, driven by
// PM4_XE_SWAP. ClearSequence is the SDK-independent resource/lifecycle core (host-tested
// with fake callbacks); NativePresentation binds it to the SDK's Vulkan provider/presenter.

#include <cstdint>
#include <functional>
#include <memory>

namespace rex::ui {
class GraphicsProvider;
class Presenter;
class WindowedAppContext;
}  // namespace rex::ui

namespace sr::native {

enum class FenceWait { kComplete, kTimedOut, kDeviceLost, kFailed };
enum class SubmitResult { kOk, kFailed, kDeviceLost };
enum class ClearResult { kPresented, kFailed, kCancelled, kDeviceLost };

// Each operation maps to Vulkan calls in NativePresentation and to fakes in tests.
// create_* callbacks must be idempotent for what they already created. destroy_* are
// only invoked for objects whose creation was reported successful, never while a
// submission that may use them is unresolved.
struct ClearOps {
  std::function<bool()> create_render_pass;
  std::function<bool()> create_command_resources;  // Pool, command buffer and fence.
  std::function<bool(uint64_t)> create_framebuffer;  // For one guest output image version.
  std::function<SubmitResult()> record_and_submit;
  std::function<FenceWait()> wait_fence;  // One bounded slice.
  std::function<void()> reset_for_reuse;  // Fence and pool, after a completed submission.
  std::function<void()> destroy_framebuffers;
  std::function<void()> destroy_command_resources;
  std::function<void()> destroy_render_pass;
  std::function<bool()> cancelled;
  std::function<void(const char*)> report;
};

class ClearSequence {
 public:
  explicit ClearSequence(ClearOps ops) : ops_(std::move(ops)) {}
  ClearSequence(const ClearSequence&) = delete;
  ClearSequence& operator=(const ClearSequence&) = delete;

  // One refresh. A failed or cancelled clear is never reported as presented, and nothing
  // referenced by an unresolved submission is reset or reused.
  ClearResult Clear(uint64_t image_version) {
    if (lost_) return ClearResult::kDeviceLost;
    if (in_flight_) {
      const ClearResult drained = Drain();
      if (drained != ClearResult::kPresented) return drained;
    }
    if (!pass_) {
      if (!ops_.create_render_pass()) return ClearResult::kFailed;
      pass_ = true;
    }
    if (!resources_) {
      if (!ops_.create_command_resources()) return ClearResult::kFailed;
      resources_ = true;
    }
    if (!ops_.create_framebuffer(image_version)) return ClearResult::kFailed;
    framebuffers_ = true;
    switch (ops_.record_and_submit()) {
      case SubmitResult::kOk: break;
      case SubmitResult::kFailed: return ClearResult::kFailed;
      case SubmitResult::kDeviceLost:
        lost_ = true;
        return ClearResult::kDeviceLost;
    }
    in_flight_ = true;
    return Drain();
  }

  // Idempotent. Waits for an unresolved submission for at most max_slices fence slices;
  // if it is still unresolved the resources are kept and false is returned.
  bool Teardown(unsigned max_slices) {
    if (in_flight_) {
      for (unsigned slice = 0; slice < max_slices && in_flight_; ++slice) {
        switch (ops_.wait_fence()) {
          case FenceWait::kComplete: in_flight_ = false; break;
          case FenceWait::kDeviceLost:
            lost_ = true;
            in_flight_ = false;  // A lost device no longer executes work.
            break;
          case FenceWait::kTimedOut:
          case FenceWait::kFailed: break;
        }
      }
      if (in_flight_) return false;
    }
    if (framebuffers_) ops_.destroy_framebuffers();
    framebuffers_ = false;
    if (resources_) ops_.destroy_command_resources();
    resources_ = false;
    if (pass_) ops_.destroy_render_pass();
    pass_ = false;
    return true;
  }

  bool in_flight() const { return in_flight_; }
  bool device_lost() const { return lost_; }

 private:
  // Waits for the submitted clear. Timeouts are reported and retried until cancelled.
  ClearResult Drain() {
    for (;;) {
      if (ops_.cancelled && ops_.cancelled()) return ClearResult::kCancelled;
      switch (ops_.wait_fence()) {
        case FenceWait::kComplete:
          in_flight_ = false;
          ops_.reset_for_reuse();
          return ClearResult::kPresented;
        case FenceWait::kTimedOut:
          if (ops_.report) ops_.report("fence wait timed out");
          break;
        case FenceWait::kDeviceLost:
          lost_ = true;
          in_flight_ = false;
          return ClearResult::kDeviceLost;
        case FenceWait::kFailed:
          return ClearResult::kFailed;  // Completion unknown: keep everything.
      }
    }
  }

  ClearOps ops_;
  bool pass_ = false, resources_ = false, framebuffers_ = false;
  bool in_flight_ = false, lost_ = false;
};

// Owns the Vulkan provider, the SDK presenter and the clear resources.
// Creation and destruction of the presenter happen on the UI thread.
class NativePresentation {
 public:
  // cancelled lets system shutdown interrupt fence polling before workers are joined.
  explicit NativePresentation(std::function<bool()> cancelled = {});
  ~NativePresentation();
  NativePresentation(const NativePresentation&) = delete;
  NativePresentation& operator=(const NativePresentation&) = delete;

  bool Initialize(rex::ui::WindowedAppContext* app_context);
  // Clears the guest output to opaque black; true only when the clear completed on the
  // GPU. width/height are the guest swap size and define the display aspect ratio only.
  bool PresentClear(uint32_t width, uint32_t height);
  // Stops using the device. Returns false when GPU work could not be drained in time;
  // the owning objects are then retained rather than destroyed under the GPU.
  bool Shutdown();

  rex::ui::GraphicsProvider* provider() const;
  rex::ui::Presenter* presenter() const;
  // Successful surface paints (presented or suboptimal), including repeated and UI paints.
  // Not the game's frame rate. 0 before the presenter exists.
  uint64_t surface_paints() const;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace sr::native
