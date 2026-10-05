#pragma once

// Presentation of the native Vulkan game renderer on the Switch: the ReXGlue SDK keeps the
// Vulkan provider, device and presenter (window, swapchain, UI overlay); the recorder of the
// PC project (pcvk) draws the game on that same device and composes each finished frame into
// the presenter's guest-output image.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "pcvk/graphics/guest/render_packet.h"
#include "pcvk/graphics/guest/shader_capture.h"
#include "pcvk/graphics/shaders/vulkan_shader_service.h"

namespace rex::ui {
class GraphicsProvider;
class Presenter;
class WindowedAppContext;
}  // namespace rex::ui

namespace sr::vk {

struct PresentationStats {
  std::atomic<uint64_t> frames{0}, composed{0}, failures{0};
};

class Presentation {
 public:
  // `cancelled` lets system shutdown interrupt fence waits before the workers are joined.
  explicit Presentation(std::function<bool()> cancelled = {});
  ~Presentation();
  Presentation(const Presentation&) = delete;
  Presentation& operator=(const Presentation&) = delete;

  // Creates the SDK provider and presenter. Presenter creation and destruction run on the UI
  // thread (`app_context` may be null in tests).
  bool Initialize(rex::ui::WindowedAppContext* app_context);

  // Adopts the SDK's Vulkan device for the game renderer. `shaders` resolves captured shader
  // containers (the offline pack); `driver_cache` is the pipeline-cache file.
  using ShaderLookup = std::function<superman_returns::graphics::shaders::ShaderResult(
      const superman_returns::graphics::guest::ShaderCapture&)>;
  bool StartRenderer(ShaderLookup shaders,
                     const std::filesystem::path& driver_cache, std::string& error);

  // The front end's packet sink (its worker thread). A swap packet finishes the frame and
  // composes it into the guest output. False stops the renderer.
  bool Submit(superman_returns::graphics::guest::RenderPacket&& packet, std::string& diagnostic);

  // Interrupts waits so the front end can be joined; the device stays usable for teardown.
  void Cancel();
  // Waits for the GPU and destroys everything this object created. False if the GPU could not
  // be drained in time (the objects are then retained rather than destroyed under the GPU).
  bool Shutdown();

  rex::ui::GraphicsProvider* provider() const;
  rex::ui::Presenter* presenter() const;
  uint64_t surface_paints() const;
  const PresentationStats& stats() const;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace sr::vk
