#pragma once
#include <string>
#include <string_view>
#include <vulkan/vulkan.h>
namespace superman_returns::graphics::vulkan {
struct Error {
  std::string operation;
  VkResult result = VK_SUCCESS;
  std::string message;
};
bool Check(VkResult result, const char *operation, Error &);
// Presentation entry points and debug utils are optional: offscreen contexts and contexts
// adopted from the SDK (which owns the swapchain) never use them.
constexpr bool IsOptionalEntryPoint(std::string_view name) {
  return name.find("Surface") != std::string_view::npos ||
         name.find("Swapchain") != std::string_view::npos ||
         name == "vkAcquireNextImageKHR" || name == "vkQueuePresentKHR" ||
         name == "vkCreateDebugUtilsMessengerEXT" ||
         name == "vkDestroyDebugUtilsMessengerEXT";
}
struct Dispatch {
#define VK_GLOBAL(n) PFN_##n n = nullptr;
#define VK_INSTANCE(n) VK_GLOBAL(n)
#define VK_DEVICE(n) VK_GLOBAL(n)
#include "functions.inc"
#undef VK_GLOBAL
#undef VK_INSTANCE
#undef VK_DEVICE
};
class Loader {
public:
  Loader() = default;
  ~Loader();
  Loader(const Loader &) = delete;
  Loader &operator=(const Loader &) = delete;
  bool Open(Error &, std::string path = "");
  bool LoadGlobal(Dispatch &, Error &);
  bool LoadInstance(Dispatch &, VkInstance, Error &);
  bool LoadDevice(Dispatch &, VkDevice, Error &);
  PFN_vkGetInstanceProcAddr GetInstanceProcAddr() const { return proc_; }

private:
  void *module_ = nullptr;
  PFN_vkGetInstanceProcAddr proc_ = nullptr;
};
} // namespace superman_returns::graphics::vulkan
