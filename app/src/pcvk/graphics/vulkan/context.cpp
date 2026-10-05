#include "context.h"
#include "query.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <set>
namespace superman_returns::graphics::vulkan {
void Context::Log(const std::string &s) const {
  if (logger)
    logger(s);
  else
    std::cerr << s << '\n';
}
VKAPI_ATTR VkBool32 VKAPI_CALL
Context::Debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
               VkDebugUtilsMessageTypeFlagsEXT,
               const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
  auto &c = *static_cast<Context *>(user);
  bool error = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
  if (error)
    c.validation_errors.fetch_add(1);
  try {
    c.Log(std::string(error ? "validation ERROR: " : "validation WARNING: ") +
          (data && data->pMessage ? data->pMessage : "unknown"));
  } catch (...) {
    // Diagnostics must never unwind across the Vulkan callback boundary.
    // Error accounting above remains available even if logging fails.
  }
  return VK_FALSE;
}
Context::~Context() {
  if (adopted_)
    return;  // Instance, device and queue belong to the adopting owner.
  if (device) {
    if (f.vkDeviceWaitIdle)
      f.vkDeviceWaitIdle(device);
    if (f.vkDestroyDevice)
      f.vkDestroyDevice(device, nullptr);
  }
  if (surface && f.vkDestroySurfaceKHR)
    f.vkDestroySurfaceKHR(instance, surface, nullptr);
  if (messenger_ && f.vkDestroyDebugUtilsMessengerEXT)
    f.vkDestroyDebugUtilsMessengerEXT(instance, messenger_, nullptr);
  if (instance && f.vkDestroyInstance)
    f.vkDestroyInstance(instance, nullptr);
}
bool Context::CreateInstance(std::span<const char *> platform_extensions,
                             bool validation, Error &e) {
  if (instance) {
    e = {"CreateInstance", VK_ERROR_INITIALIZATION_FAILED,
         "Context already initialized"};
    return false;
  }
  if (!injected_ && (!loader_.Open(e) || !loader_.LoadGlobal(f, e)))
    return false;
  uint32_t version = VK_API_VERSION_1_0;
  if (f.vkEnumerateInstanceVersion &&
      !Check(f.vkEnumerateInstanceVersion(&version), "EnumerateInstanceVersion",
             e))
    return false;
  if (version < VK_API_VERSION_1_1) {
    e = {"Instance version", VK_ERROR_INCOMPATIBLE_DRIVER,
         "Vulkan 1.1 required"};
    return false;
  }
  std::vector<VkExtensionProperties> available;
  if (!Query<VkExtensionProperties>(
          [&](auto *n, auto *p) {
            return f.vkEnumerateInstanceExtensionProperties(nullptr, n, p);
          },
          available, "Instance extensions", e))
    return false;
  auto has = [&](const char *s) {
    return std::any_of(available.begin(), available.end(), [&](auto &x) {
      return !std::strcmp(x.extensionName, s);
    });
  };
  std::vector<const char *> extensions(platform_extensions.begin(),
                                       platform_extensions.end());
  for (auto name : extensions)
    if (!has(name)) {
      e = {"Instance extensions", VK_ERROR_EXTENSION_NOT_PRESENT,
           std::string("Required extension unavailable: ") + name};
      return false;
    }
  std::vector<VkLayerProperties> layers;
  if (!Query<VkLayerProperties>(
          [&](auto *n, auto *p) {
            return f.vkEnumerateInstanceLayerProperties(n, p);
          },
          layers, "Instance layers", e))
    return false;
  const char *layer = "VK_LAYER_KHRONOS_validation";
  validation_active = validation && has(VK_EXT_DEBUG_UTILS_EXTENSION_NAME) &&
                      std::any_of(layers.begin(), layers.end(), [&](auto &l) {
                        return !std::strcmp(l.layerName, layer);
                      });
  if (validation && !validation_active)
    Log("Validation layers/debug-utils unavailable; GPU validation not "
        "executed");
  VkDebugUtilsMessengerCreateInfoEXT debug{
      VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
  debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
  debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
  debug.pfnUserCallback = Debug;
  debug.pUserData = this;
  if (validation_active)
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "Superman Vulkan Foundation";
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ci.pApplicationInfo = &app;
  ci.enabledExtensionCount = uint32_t(extensions.size());
  ci.ppEnabledExtensionNames = extensions.data();
  if (validation_active) {
    ci.enabledLayerCount = 1;
    ci.ppEnabledLayerNames = &layer;
    ci.pNext = &debug;
  }
  if (!Check(f.vkCreateInstance(&ci, nullptr, &instance), "CreateInstance", e))
    return false;
  if (!injected_ && !loader_.LoadInstance(f, instance, e))
    return false;
  if (validation_active) {
    if (!f.vkCreateDebugUtilsMessengerEXT ||
        !f.vkDestroyDebugUtilsMessengerEXT) {
      e = {"DebugUtils", VK_ERROR_EXTENSION_NOT_PRESENT,
           "Validation callback unavailable"};
      return false;
    }
    if (!Check(f.vkCreateDebugUtilsMessengerEXT(instance, &debug, nullptr,
                                                &messenger_),
               "Debug messenger", e))
      return false;
  }
  return true;
}
bool Context::EnumerateCandidates(VkSurfaceKHR target,
                                  std::vector<DeviceCandidate> &candidates,
                                  Error &e) {
  if (!Query<VkPhysicalDevice>(
          [&](auto *n, auto *p) {
            return f.vkEnumeratePhysicalDevices(instance, n, p);
          },
          devices_, "Physical devices", e))
    return false;
  candidates.clear();
  for (auto d : devices_) {
    VkPhysicalDeviceIDProperties id{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 props{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props.pNext = &id;
    f.vkGetPhysicalDeviceProperties2(d, &props);
    DeviceCandidate c;
    c.name = props.properties.deviceName;
    c.api_version = props.properties.apiVersion;
    c.type = props.properties.deviceType;
    constexpr char hex[] = "0123456789abcdef";
    for (auto v : id.deviceUUID) {
      c.uuid += hex[v >> 4];
      c.uuid += hex[v & 15];
    }
    std::vector<VkExtensionProperties> exts;
    if (!Query<VkExtensionProperties>(
            [&](auto *n, auto *p) {
              return f.vkEnumerateDeviceExtensionProperties(d, nullptr, n, p);
            },
            exts, "Device extensions", e))
      return false;
    c.swapchain = std::any_of(exts.begin(), exts.end(), [](auto &v) {
      return !std::strcmp(v.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    });
    uint32_t count = 0;
    f.vkGetPhysicalDeviceQueueFamilyProperties(d, &count, nullptr);
    std::vector<VkQueueFamilyProperties> q(count);
    f.vkGetPhysicalDeviceQueueFamilyProperties(d, &count, q.data());
    q.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
      VkBool32 present = VK_FALSE;
      if (target &&
          !Check(f.vkGetPhysicalDeviceSurfaceSupportKHR(d, i, target, &present),
                 "Surface support", e))
        return false;
      c.queues.push_back(
          {q[i].queueCount ? q[i].queueFlags : 0, bool(present)});
    }
    candidates.push_back(std::move(c));
  }
  return true;
}
bool Context::OpenDevice(VkSurfaceKHR owned, std::string_view uuid, Error &e,
                         const VkPhysicalDeviceFeatures *requested_features,bool require_mirror_clamp) {
  if (device || surface) {
    e = {"OpenDevice", VK_ERROR_INITIALIZATION_FAILED,
         "Device already initialized"};
    return false;
  }
  surface = owned;
  if (!surface) {
    e = {"Surface", VK_ERROR_SURFACE_LOST_KHR, "Presentation surface required"};
    return false;
  }
  std::vector<DeviceCandidate> candidates;
  if (!EnumerateCandidates(surface, candidates, e))
    return false;
  auto choice = SelectDevice(candidates, uuid);
  if (!choice.index) {
    e = {"GPU selection", VK_ERROR_FEATURE_NOT_PRESENT, choice.error};
    return false;
  }
  return CreateSelectedDevice(candidates[*choice.index], devices_[*choice.index],
                              choice.graphics, choice.present, true,
                              requested_features, require_mirror_clamp, e);
}
bool Context::OpenOffscreenDevice(std::string_view uuid, Error &e,
                                  const VkPhysicalDeviceFeatures *requested_features,
                                  bool require_mirror_clamp) {
  if (device || surface) {
    e = {"OpenOffscreenDevice", VK_ERROR_INITIALIZATION_FAILED,
         "Device already initialized"};
    return false;
  }
  std::vector<DeviceCandidate> candidates;
  if (!EnumerateCandidates(VK_NULL_HANDLE, candidates, e))
    return false;
  auto choice = SelectOffscreenDevice(candidates, uuid);
  if (!choice.index) {
    e = {"GPU selection", VK_ERROR_FEATURE_NOT_PRESENT, choice.error};
    return false;
  }
  return CreateSelectedDevice(candidates[*choice.index], devices_[*choice.index],
                              choice.graphics, choice.present, false,
                              requested_features, require_mirror_clamp, e);
}
bool Context::CreateSelectedDevice(const DeviceCandidate &candidate,
                                   VkPhysicalDevice physical_device,
                                   uint32_t graphics, uint32_t present,
                                   bool with_swapchain,
                                   const VkPhysicalDeviceFeatures *requested_features,
                                   bool require_mirror_clamp, Error &e) {
  selected = candidate;
  physical = physical_device;
  graphics_family = graphics;
  present_family = present;
  float priority = 1;
  std::vector<VkDeviceQueueCreateInfo> queues;
  for (auto family : std::set<uint32_t>{graphics_family, present_family}) {
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = family;
    q.queueCount = 1;
    q.pQueuePriorities = &priority;
    queues.push_back(q);
  }
  std::vector<const char*> extensions;
  if (with_swapchain)
    extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  if(require_mirror_clamp) {
    std::vector<VkExtensionProperties> available;
    if(!Query<VkExtensionProperties>([&](uint32_t* count,VkExtensionProperties* values) {
      return f.vkEnumerateDeviceExtensionProperties(physical,nullptr,count,values);
    },available,"Device extensions",e)) return false;
    if(std::none_of(available.begin(),available.end(),[](const auto& v) {return std::strcmp(v.extensionName,VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME)==0;})) {
      e={"Device extension",VK_ERROR_EXTENSION_NOT_PRESENT,"VK_KHR_sampler_mirror_clamp_to_edge required by game samplers"};return false;
    }
    extensions.push_back(VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);
  }
  VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  ci.queueCreateInfoCount = uint32_t(queues.size());
  ci.pQueueCreateInfos = queues.data();
  ci.enabledExtensionCount = uint32_t(extensions.size());
  ci.ppEnabledExtensionNames = extensions.data();
  ci.pEnabledFeatures = requested_features;

  if (!Check(f.vkCreateDevice(physical, &ci, nullptr, &device), "CreateDevice",
             e))
    return false;
  enabled_features=requested_features?*requested_features:VkPhysicalDeviceFeatures{};
  mirror_clamp_enabled=require_mirror_clamp;
  if (!injected_ && !loader_.LoadDevice(f, device, e))
    return false;
  f.vkGetDeviceQueue(device, graphics_family, 0, &graphics_queue);
  f.vkGetDeviceQueue(device, present_family, 0, &present_queue);
  VkPhysicalDeviceProperties2 props{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  f.vkGetPhysicalDeviceProperties2(physical, &props);
  properties = props.properties;
  f.vkGetPhysicalDeviceMemoryProperties(physical, &memory);
  Log("GPU: " + selected.name + " UUID=" + selected.uuid +
      " API=" + std::to_string(VK_VERSION_MAJOR(properties.apiVersion)) + "." +
      std::to_string(VK_VERSION_MINOR(properties.apiVersion)) +
      " driver=" + std::to_string(properties.driverVersion));
  Log(std::string("Features: Vulkan 1.1, ") + (with_swapchain ? "swapchain" : "offscreen") +
      ", graphics=" + std::to_string(graphics_family) +
      " present=" + std::to_string(present_family) +
      (requested_features ? "; explicit optional device features requested"
                          : "; no optional device features enabled"));
  return true;
}
bool Context::Adopt(const ExternalDevice &external, Error &e) {
  if (device || instance) {
    e = {"Adopt", VK_ERROR_INITIALIZATION_FAILED, "Context already initialized"};
    return false;
  }
  if (!external.get_instance_proc || !external.get_device_proc || !external.instance ||
      !external.physical || !external.device || !external.graphics_queue) {
    e = {"Adopt", VK_ERROR_INITIALIZATION_FAILED, "Incomplete external Vulkan device"};
    return false;
  }
  adopted_ = true;
  instance = external.instance;
  physical = external.physical;
  device = external.device;
  graphics_queue = present_queue = external.graphics_queue;
  graphics_family = present_family = external.graphics_family;
  external_get_instance_proc_ = external.get_instance_proc;
  // Instance and device entry points are resolved through the owner's loader.
  f.vkGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
      external.get_instance_proc(instance, "vkGetDeviceProcAddr"));
  if (!f.vkGetDeviceProcAddr) {
    e = {"Adopt", VK_ERROR_INITIALIZATION_FAILED, "vkGetDeviceProcAddr unavailable"};
    return false;
  }
#define VK_GLOBAL(n)
#define VK_INSTANCE(n)                                                         \
  f.n = reinterpret_cast<PFN_##n>(external.get_instance_proc(instance, #n));
#define VK_DEVICE(n)
#include "functions.inc"
#undef VK_GLOBAL
#undef VK_INSTANCE
#undef VK_DEVICE
#define VK_GLOBAL(n)
#define VK_INSTANCE(n)
#define VK_DEVICE(n)                                                           \
  f.n = reinterpret_cast<PFN_##n>(external.get_device_proc(device, #n));       \
  if (!f.n && !IsOptionalEntryPoint(#n)) {                                     \
    e = {#n, VK_ERROR_INITIALIZATION_FAILED,                                   \
         "Required Vulkan device function unavailable"};                       \
    return false;                                                              \
  }
#include "functions.inc"
#undef VK_GLOBAL
#undef VK_INSTANCE
#undef VK_DEVICE
  enabled_features = external.enabled_features;
  mirror_clamp_enabled = external.mirror_clamp_enabled;
  VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  f.vkGetPhysicalDeviceProperties2(physical, &props);
  properties = props.properties;
  f.vkGetPhysicalDeviceMemoryProperties(physical, &memory);
  selected.name = properties.deviceName;
  selected.api_version = properties.apiVersion;
  selected.type = properties.deviceType;
  Log("GPU (adopted): " + selected.name +
      " API=" + std::to_string(VK_VERSION_MAJOR(properties.apiVersion)) + "." +
      std::to_string(VK_VERSION_MINOR(properties.apiVersion)) +
      " driver=" + std::to_string(properties.driverVersion));
  return true;
}
} // namespace superman_returns::graphics::vulkan
