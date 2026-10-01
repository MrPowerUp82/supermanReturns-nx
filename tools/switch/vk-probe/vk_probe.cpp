// Game-free Vulkan/NVK bring-up probe for Superman Returns NX.
//
// It links the same static Mesa NVK driver as the game and walks the Vulkan
// bring-up one step at a time: instance, physical device, logical device
// (channel + ZCULL bind + first context-state submit), empty submit, buffer
// copy and fill, image clear, plain and depth-tested draws, swapchain present
// and teardown. Each step is
// written and flushed to the SD card *before* it runs, so when an emulator
// dies the last "BEGIN" line names the operation that triggered it.
//
// No Superman Returns code or data is contained or loaded here.
//
// Optional configuration: sdmc:/switch/superman-returns-nx/vk-probe.cfg
//   env NAME=VALUE      set a driver environment variable before vkCreateInstance
//                       (e.g. env NVK_DEBUG=push_sync, env NVK_SWITCH_MAPPED_COMPLETION=false)
//   stop_after STEP     stop cleanly after the named step (e.g. stop_after device)
//   skip STEP           skip an optional step: copy, fill, draw, depth, wsi
//   frames N            frames presented in the wsi step (default 120)
//   guest_memory MODE   before Vulkan, reproduce the game's 360 memory layout with
//                       the same SVC sequence as sdk/src/core/guest_memory_switch.cpp:
//                       "reserve" (window only), "eager" (window, views and the
//                       512 MiB physical precommit aliased into every view, as
//                       switch_eager_memory does on emulators). Default: off.
#include <switch.h>

#include <fcntl.h>
#include <malloc.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define VK_USE_PLATFORM_VI_NN
#include <vulkan/vulkan.h>

extern "C" {
u32 __nx_applet_type = AppletType_Application;
}

namespace {

const char* const kDir = "sdmc:/switch/superman-returns-nx";
const char* const kLog = "sdmc:/switch/superman-returns-nx/vk-probe.log";
const char* const kCfg = "sdmc:/switch/superman-returns-nx/vk-probe.cfg";

const uint32_t kVertSpv[] = {
#include "probe.vert.inc"
};
const uint32_t kFragSpv[] = {
#include "probe.frag.inc"
};

FILE* g_log = nullptr;
std::string g_stop_after;
std::vector<std::string> g_skip;
uint32_t g_frames = 120;
std::string g_guest_memory;
int g_failures = 0;

void Log(const char* format, ...) {
  char line[1024];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  if (g_log) {
    fputs(line, g_log);
    fputc('\n', g_log);
    fflush(g_log);
    fsync(fileno(g_log));
  }
  svcOutputDebugString(line, strlen(line));
}

// Every step is announced and flushed before it runs.
void Begin(const char* step) { Log("BEGIN %s", step); }
bool End(const char* step, VkResult result) {
  Log("END %s: %s (%d)", step, result == VK_SUCCESS ? "ok" : "FAILED", int(result));
  if (result != VK_SUCCESS) ++g_failures;
  return result == VK_SUCCESS;
}
bool Check(const char* step, bool ok, const char* detail) {
  Log("END %s: %s%s%s", step, ok ? "ok" : "FAILED", detail ? " - " : "", detail ? detail : "");
  if (!ok) ++g_failures;
  return ok;
}
bool StopAfter(const char* step) {
  if (g_stop_after == step) {
    Log("stop_after %s reached", step);
    return true;
  }
  return false;
}
bool Skipped(const char* step) {
  for (const auto& s : g_skip)
    if (s == step) {
      Log("SKIP %s (configuration)", step);
      return true;
    }
  return false;
}

void ReadConfig() {
  FILE* cfg = fopen(kCfg, "r");
  if (!cfg) {
    Log("config: %s not present, defaults used", kCfg);
    return;
  }
  char line[512];
  while (fgets(line, sizeof(line), cfg)) {
    std::string text(line);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
      text.pop_back();
    if (text.empty() || text[0] == '#') continue;
    const auto space = text.find(' ');
    const std::string key = text.substr(0, space);
    const std::string value = space == std::string::npos ? "" : text.substr(space + 1);
    if (key == "env") {
      const auto eq = value.find('=');
      if (eq != std::string::npos) {
        setenv(value.substr(0, eq).c_str(), value.substr(eq + 1).c_str(), 1);
        Log("config: env %s", value.c_str());
      }
    } else if (key == "stop_after") {
      g_stop_after = value;
      Log("config: stop_after %s", value.c_str());
    } else if (key == "skip") {
      g_skip.push_back(value);
      Log("config: skip %s", value.c_str());
    } else if (key == "frames") {
      g_frames = uint32_t(strtoul(value.c_str(), nullptr, 10));
      Log("config: frames %u", g_frames);
    } else if (key == "guest_memory") {
      g_guest_memory = value;
      Log("config: guest_memory %s", value.c_str());
    } else {
      Log("config: unknown line ignored: %s", text.c_str());
    }
  }
  fclose(cfg);
}

void LogPlatform() {
  u64 value = 0;
  if (R_SUCCEEDED(svcGetInfo(&value, InfoType_AslrRegionSize, CUR_PROCESS_HANDLE, 0)))
    Log("platform: ASLR size %016llx (39-bit: %s)", (unsigned long long)value,
        value >= (1ull << 38) ? "yes" : "no");
  Log("platform: own process handle from loader: %s",
      envGetOwnProcessHandle() == INVALID_HANDLE ? "no (direct launch)" : "yes (hbloader)");
  if (R_SUCCEEDED(svcGetInfo(&value, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0)))
    Log("platform: total memory %llu MiB", (unsigned long long)(value >> 20));
}

// ---------------------------------------------------------------------------
// Optional guest memory stage. Same layout and SVCs as the game, no game code:
// a 4.5 GiB window (Memory::Initialize's 0x120000000 mapping), the nine views
// of xmemory.cpp's map_info, and committed chunks that are heap backing moved
// to code memory (MapProcessCodeMemory), opened RW and aliased into each view
// that covers them (MapProcessMemory). The mappings stay alive while Vulkan
// starts, as in the game, where Runtime::Setup initializes memory first.

struct ProcessHandleRequest {
  Handle server = INVALID_HANDLE;
  Handle process = INVALID_HANDLE;
  Result receive_result = 0;
};

void ReceiveProcessHandle(void* opaque) {
  auto& request = *static_cast<ProcessHandleRequest*>(opaque);
  hipcMakeRequest(armGetTls(), HipcMetadata{});
  s32 index = 0;
  request.receive_result =
      svcReplyAndReceive(&index, &request.server, 1, INVALID_HANDLE, 2000000000ull);
  if (R_SUCCEEDED(request.receive_result)) {
    const auto received = hipcParseRequest(armGetTls());
    if (received.meta.num_copy_handles == 1) request.process = received.data.copy_handles[0];
  }
  svcCloseHandle(request.server);
}

// The loader's handle, or a real handle obtained through a local IPC copy of
// the pseudo-handle (guest_memory_switch.cpp Proc()).
Handle OwnProcess() {
  if (envGetOwnProcessHandle() != INVALID_HANDLE) return envGetOwnProcessHandle();
  ProcessHandleRequest request;
  Handle client = INVALID_HANDLE;
  if (R_FAILED(svcCreateSession(&request.server, &client, 0, 0))) return INVALID_HANDLE;
  Thread receiver{};
  if (R_SUCCEEDED(threadCreate(&receiver, ReceiveProcessHandle, &request, nullptr, 0x4000, 0x2C, -2)) &&
      R_SUCCEEDED(threadStart(&receiver))) {
    HipcMetadata metadata{};
    metadata.type = CmifCommandType_Request;
    metadata.num_copy_handles = 1;
    auto outgoing = hipcMakeRequest(armGetTls(), metadata);
    outgoing.copy_handles[0] = CUR_PROCESS_HANDLE;
    svcSendSyncRequest(client);
    threadWaitForExit(&receiver);
    threadClose(&receiver);
  } else {
    svcCloseHandle(request.server);
  }
  svcCloseHandle(client);
  return request.process;
}

struct GuestView {
  uint64_t guest_start, guest_end, file_offset;
};
// xmemory.cpp map_info.
const GuestView kViews[] = {
    {0x00000000, 0x3FFFFFFF, 0x000000000}, {0x40000000, 0x7EFFFFFF, 0x040000000},
    {0x7F000000, 0x7FFFFFFF, 0x100000000}, {0x80000000, 0x8FFFFFFF, 0x080000000},
    {0x90000000, 0x9FFFFFFF, 0x080000000}, {0xA0000000, 0xBFFFFFFF, 0x100000000},
    {0xC0000000, 0xDFFFFFFF, 0x100000000}, {0xE0000000, 0xFFFFFFFF, 0x100001000},
    {0x100000000, 0x11FFFFFFF, 0x100000000},
};

struct GuestMemory {
  Handle process = INVALID_HANDLE;
  uint8_t* base = nullptr;
  size_t size = 0;
  VirtmemReservation* reservation = nullptr;
  struct Alias { uint8_t* at; uint8_t* src; size_t len; };
  struct Chunk { void* backing; uint8_t* shadow; size_t offset, len; std::vector<Alias> aliases; };
  std::vector<Chunk> chunks;
  size_t aliased = 0;
};
GuestMemory g_gm;

bool CommitGuest(size_t offset, size_t len) {
  const size_t align = len >= 0x200000 ? 0x200000 : 0x1000;
  void* backing = memalign(align, len);
  if (!backing) return Check("guest_memory commit", false, "memalign failed");
  memset(backing, 0, len);
  virtmemLock();
  auto* shadow = static_cast<uint8_t*>(virtmemFindCodeMemory(len, align));
  Result rc = shadow ? svcMapProcessCodeMemory(g_gm.process, u64(shadow), u64(backing), len)
                     : MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
  virtmemUnlock();
  Log("  commit 0x%zx+0x%zx: MapProcessCodeMemory %p <- %p: 0x%x", offset, len, shadow,
      backing, rc);
  if (R_FAILED(rc)) {
    free(backing);
    return false;
  }
  // Registered now so ReleaseGuestMemory undoes it even if a later step fails.
  g_gm.chunks.push_back({backing, shadow, offset, len, {}});
  GuestMemory::Chunk& c = g_gm.chunks.back();
  rc = svcSetProcessMemoryPermission(g_gm.process, u64(shadow), len, Perm_Rw);
  Log("  SetProcessMemoryPermission RW: 0x%x", rc);
  if (R_FAILED(rc)) return false;
  for (const auto& v : kViews) {
    const uint64_t view_len = v.guest_end - v.guest_start + 1;
    const uint64_t lo = offset > v.file_offset ? offset : v.file_offset;
    const uint64_t hi = (offset + len) < (v.file_offset + view_len) ? offset + len : v.file_offset + view_len;
    if (lo >= hi) continue;
    uint8_t* at = g_gm.base + v.guest_start + (lo - v.file_offset);
    uint8_t* src = shadow + (lo - offset);
    rc = svcMapProcessMemory(at, g_gm.process, u64(src), hi - lo);
    Log("  alias view %08llx: MapProcessMemory %p <- %p (0x%llx bytes): 0x%x",
        (unsigned long long)v.guest_start, at, src, (unsigned long long)(hi - lo), rc);
    if (R_FAILED(rc)) return false;
    c.aliases.push_back({at, src, size_t(hi - lo)});
    g_gm.aliased += hi - lo;
  }
  return true;
}

bool StepGuestMemory() {
  if (g_guest_memory.empty() || g_guest_memory == "off") return true;
  Begin("guest_memory");
  g_gm.process = OwnProcess();
  Log("  own process handle: 0x%x", g_gm.process);
  if (g_gm.process == INVALID_HANDLE) return Check("guest_memory", false, "no process handle");
  g_gm.size = 0x120000000ull + 0x10000;
  virtmemLock();
  g_gm.base = static_cast<uint8_t*>(virtmemFindAslr(g_gm.size, 0x200000));
  if (g_gm.base) g_gm.reservation = virtmemAddReservation(g_gm.base, g_gm.size);
  virtmemUnlock();
  Log("  window %p, 0x%zx bytes", g_gm.base, g_gm.size);
  if (!g_gm.base || !g_gm.reservation) return Check("guest_memory", false, "window reservation");
  if (g_guest_memory == "reserve") return Check("guest_memory", true, "window only");
  if (g_guest_memory != "eager") return Check("guest_memory", false, "unknown mode");
  // Memory::Initialize: 64 KiB at 0, 16 MiB at 0xC0000000 (physical 0), then the
  // eager precommit of the remaining 512 MiB physical heap.
  bool ok = CommitGuest(0, 0x10000) && CommitGuest(0x100000000ull, 0x1000000) &&
            CommitGuest(0x101000000ull, 0x20000000 - 0x1000000);
  if (ok) {
    // Coherence across aliases: write through the physical-raw view, read
    // through the 0xA0000000 view of the same physical page.
    auto* raw = reinterpret_cast<volatile uint32_t*>(g_gm.base + 0x100000000ull + 0x2000000);
    auto* a0 = reinterpret_cast<volatile uint32_t*>(g_gm.base + 0xA0000000ull + 0x2000000);
    *raw = 0x5352584e;
    ok = *a0 == 0x5352584e;
    Log("  alias coherence: %s", ok ? "yes" : "NO");
  }
  char detail[96];
  snprintf(detail, sizeof(detail), "%zu chunks, %zu MiB aliased", g_gm.chunks.size(),
           g_gm.aliased >> 20);
  return Check("guest_memory", ok, detail);
}

void ReleaseGuestMemory() {
  if (!g_gm.base) return;
  for (auto& c : g_gm.chunks) {
    for (const auto& a : c.aliases) svcUnmapProcessMemory(a.at, g_gm.process, u64(a.src), a.len);
    svcUnmapProcessCodeMemory(g_gm.process, u64(c.shadow), u64(c.backing), c.len);
    free(c.backing);
  }
  g_gm.chunks.clear();
  if (g_gm.reservation) {
    virtmemLock();
    virtmemRemoveReservation(g_gm.reservation);
    virtmemUnlock();
  }
  g_gm.base = nullptr;
}

struct Gpu {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t family = 0;
  VkPhysicalDeviceMemoryProperties memory{};
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
};

uint32_t FindMemory(const Gpu& gpu, uint32_t bits, VkMemoryPropertyFlags want) {
  for (uint32_t i = 0; i < gpu.memory.memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (gpu.memory.memoryTypes[i].propertyFlags & want) == want) return i;
  return UINT32_MAX;
}

struct HostBuffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void* map = nullptr;
  VkDeviceSize size = 0;
};

VkResult CreateHostBuffer(Gpu& gpu, VkDeviceSize size, HostBuffer& out) {
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = size;
  info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VkResult r = vkCreateBuffer(gpu.device, &info, nullptr, &out.buffer);
  if (r != VK_SUCCESS) return r;
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(gpu.device, out.buffer, &req);
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = FindMemory(gpu, req.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (alloc.memoryTypeIndex == UINT32_MAX) return VK_ERROR_FEATURE_NOT_PRESENT;
  r = vkAllocateMemory(gpu.device, &alloc, nullptr, &out.memory);
  if (r != VK_SUCCESS) return r;
  r = vkBindBufferMemory(gpu.device, out.buffer, out.memory, 0);
  if (r != VK_SUCCESS) return r;
  out.size = size;
  return vkMapMemory(gpu.device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.map);
}

void DestroyHostBuffer(Gpu& gpu, HostBuffer& b) {
  if (b.map) vkUnmapMemory(gpu.device, b.memory);
  if (b.buffer) vkDestroyBuffer(gpu.device, b.buffer, nullptr);
  if (b.memory) vkFreeMemory(gpu.device, b.memory, nullptr);
  b = HostBuffer{};
}

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
};

VkResult CreateImage(Gpu& gpu, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                     uint32_t width, uint32_t height, Image& out) {
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {width, height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = usage;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkResult r = vkCreateImage(gpu.device, &info, nullptr, &out.image);
  if (r != VK_SUCCESS) return r;
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(gpu.device, out.image, &req);
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = FindMemory(gpu, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (alloc.memoryTypeIndex == UINT32_MAX)
    alloc.memoryTypeIndex = FindMemory(gpu, req.memoryTypeBits, 0);
  r = vkAllocateMemory(gpu.device, &alloc, nullptr, &out.memory);
  if (r != VK_SUCCESS) return r;
  r = vkBindImageMemory(gpu.device, out.image, out.memory, 0);
  if (r != VK_SUCCESS) return r;
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = out.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = format;
  view.subresourceRange = {aspect, 0, 1, 0, 1};
  return vkCreateImageView(gpu.device, &view, nullptr, &out.view);
}

void DestroyImage(Gpu& gpu, Image& img) {
  if (img.view) vkDestroyImageView(gpu.device, img.view, nullptr);
  if (img.image) vkDestroyImage(gpu.device, img.image, nullptr);
  if (img.memory) vkFreeMemory(gpu.device, img.memory, nullptr);
  img = Image{};
}

void Barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from,
             VkImageLayout to) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {aspect, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       0, 0, nullptr, 0, nullptr, 1, &b);
}

VkResult BeginCommands(Gpu& gpu) {
  VkResult r = vkResetCommandBuffer(gpu.cmd, 0);
  if (r != VK_SUCCESS) return r;
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  return vkBeginCommandBuffer(gpu.cmd, &begin);
}

// Submits the recorded command buffer and waits with a 5 s timeout, so a GPU
// that never signals shows up as VK_TIMEOUT instead of a silent hang.
VkResult SubmitAndWait(Gpu& gpu, const char* what) {
  VkResult r = vkEndCommandBuffer(gpu.cmd);
  if (r != VK_SUCCESS) return r;
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &gpu.cmd;
  Log("  %s: vkQueueSubmit", what);
  r = vkQueueSubmit(gpu.queue, 1, &submit, gpu.fence);
  if (r != VK_SUCCESS) return r;
  Log("  %s: vkWaitForFences", what);
  r = vkWaitForFences(gpu.device, 1, &gpu.fence, VK_TRUE, 5000000000ull);
  if (r != VK_SUCCESS) return r;
  return vkResetFences(gpu.device, 1, &gpu.fence);
}

bool StepInstance(Gpu& gpu) {
  Begin("instance");
  uint32_t api = 0;
  vkEnumerateInstanceVersion(&api);
  Log("  loader-less API version %u.%u.%u", VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api),
      VK_API_VERSION_PATCH(api));
  const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_NN_VI_SURFACE_EXTENSION_NAME};
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "superman-returns-nx-vk-probe";
  app.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  info.pApplicationInfo = &app;
  info.enabledExtensionCount = 2;
  info.ppEnabledExtensionNames = extensions;
  return End("instance", vkCreateInstance(&info, nullptr, &gpu.instance));
}

bool StepPhysicalDevice(Gpu& gpu) {
  Begin("physical_device");
  uint32_t count = 0;
  VkResult r = vkEnumeratePhysicalDevices(gpu.instance, &count, nullptr);
  if (r != VK_SUCCESS || count == 0) return End("physical_device", r != VK_SUCCESS ? r : VK_ERROR_INITIALIZATION_FAILED);
  count = 1;
  r = vkEnumeratePhysicalDevices(gpu.instance, &count, &gpu.physical);
  if (r != VK_SUCCESS && r != VK_INCOMPLETE) return End("physical_device", r);
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(gpu.physical, &props);
  Log("  device: %s, API %u.%u.%u, driver 0x%x", props.deviceName,
      VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
      VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion);
  vkGetPhysicalDeviceMemoryProperties(gpu.physical, &gpu.memory);
  for (uint32_t i = 0; i < gpu.memory.memoryHeapCount; ++i)
    Log("  heap %u: %llu MiB flags 0x%x", i,
        (unsigned long long)(gpu.memory.memoryHeaps[i].size >> 20),
        gpu.memory.memoryHeaps[i].flags);
  for (uint32_t i = 0; i < gpu.memory.memoryTypeCount; ++i)
    Log("  memory type %u: heap %u flags 0x%x", i, gpu.memory.memoryTypes[i].heapIndex,
        gpu.memory.memoryTypes[i].propertyFlags);
  uint32_t families = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical, &families, nullptr);
  std::vector<VkQueueFamilyProperties> fam(families);
  vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical, &families, fam.data());
  gpu.family = UINT32_MAX;
  for (uint32_t i = 0; i < families; ++i) {
    Log("  queue family %u: flags 0x%x count %u", i, fam[i].queueFlags, fam[i].queueCount);
    if (gpu.family == UINT32_MAX && (fam[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) gpu.family = i;
  }
  return Check("physical_device", gpu.family != UINT32_MAX, "graphics queue family");
}

// vkCreateDevice is where NVK opens the GPU channel, binds the ZCULL context
// buffer and submits the initial 3D/compute state (including MME macro uploads
// and calls). Splitting it further requires driver logs: run with
// "env NVK_DEBUG=push_sync" to make that first submit synchronous.
bool StepDevice(Gpu& gpu) {
  Begin("device");
  const float priority = 1.0f;
  VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue.queueFamilyIndex = gpu.family;
  queue.queueCount = 1;
  queue.pQueuePriorities = &priority;
  const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  v13.dynamicRendering = VK_TRUE;
  VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  info.pNext = &v13;
  info.queueCreateInfoCount = 1;
  info.pQueueCreateInfos = &queue;
  info.enabledExtensionCount = 1;
  info.ppEnabledExtensionNames = extensions;
  if (!End("device", vkCreateDevice(gpu.physical, &info, nullptr, &gpu.device))) return false;
  vkGetDeviceQueue(gpu.device, gpu.family, 0, &gpu.queue);

  Begin("command_objects");
  VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool.queueFamilyIndex = gpu.family;
  VkResult r = vkCreateCommandPool(gpu.device, &pool, nullptr, &gpu.pool);
  if (r == VK_SUCCESS) {
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = gpu.pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    r = vkAllocateCommandBuffers(gpu.device, &alloc, &gpu.cmd);
  }
  if (r == VK_SUCCESS) {
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    r = vkCreateFence(gpu.device, &fence, nullptr, &gpu.fence);
  }
  return End("command_objects", r);
}

bool StepEmptySubmit(Gpu& gpu) {
  Begin("empty_submit");
  VkResult r = BeginCommands(gpu);
  if (r == VK_SUCCESS) r = SubmitAndWait(gpu, "empty_submit");
  return End("empty_submit", r);
}

// vkCmdCopyBuffer (fill = false) or vkCmdFillBuffer (fill = true) over 1 MiB of
// host-visible memory, checked on the CPU. Kept apart because NVK implements
// them with different engines/shaders depending on NVK_COPY_ENGINE.
bool StepBufferOp(Gpu& gpu, bool fill) {
  const char* step = fill ? "fill" : "copy";
  Begin(step);
  constexpr VkDeviceSize kSize = 1 << 20;
  constexpr uint32_t kPattern = 0x53524e58u;
  HostBuffer src, dst;
  VkResult r = CreateHostBuffer(gpu, kSize, src);
  if (r == VK_SUCCESS) r = CreateHostBuffer(gpu, kSize, dst);
  if (r != VK_SUCCESS) {
    DestroyHostBuffer(gpu, src);
    DestroyHostBuffer(gpu, dst);
    return End(step, r);
  }
  auto* words = static_cast<uint32_t*>(src.map);
  for (uint32_t i = 0; i < kSize / 4; ++i) words[i] = i * 2654435761u;
  memset(dst.map, 0, kSize);
  r = BeginCommands(gpu);
  if (r == VK_SUCCESS) {
    if (fill) {
      vkCmdFillBuffer(gpu.cmd, dst.buffer, 0, kSize, kPattern);
    } else {
      VkBufferCopy copy{0, 0, kSize};
      vkCmdCopyBuffer(gpu.cmd, src.buffer, dst.buffer, 1, &copy);
    }
    r = SubmitAndWait(gpu, step);
  }
  uint32_t mismatch = UINT32_MAX;
  if (r == VK_SUCCESS) {
    const auto* out = static_cast<const uint32_t*>(dst.map);
    for (uint32_t i = 0; i < kSize / 4; ++i)
      if (out[i] != (fill ? kPattern : words[i])) {
        mismatch = i;
        break;
      }
  }
  DestroyHostBuffer(gpu, src);
  DestroyHostBuffer(gpu, dst);
  if (r != VK_SUCCESS) return End(step, r);
  char detail[96];
  if (mismatch == UINT32_MAX)
    snprintf(detail, sizeof(detail), "1 MiB compared on the CPU");
  else
    snprintf(detail, sizeof(detail), "first mismatch at word %u", mismatch);
  return Check(step, mismatch == UINT32_MAX, detail);
}

bool StepClearImage(Gpu& gpu) {
  Begin("clear_image");
  constexpr uint32_t kW = 256, kH = 256;
  Image img;
  HostBuffer readback;
  VkResult r = CreateImage(gpu, VK_FORMAT_R8G8B8A8_UNORM,
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, kW, kH, img);
  if (r == VK_SUCCESS) r = CreateHostBuffer(gpu, kW * kH * 4, readback);
  if (r == VK_SUCCESS) r = BeginCommands(gpu);
  if (r == VK_SUCCESS) {
    Barrier(gpu.cmd, img.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue color{};
    color.float32[0] = 1.0f;
    color.float32[3] = 1.0f;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(gpu.cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    Barrier(gpu.cmd, img.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {kW, kH, 1};
    vkCmdCopyImageToBuffer(gpu.cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback.buffer, 1, &copy);
    r = SubmitAndWait(gpu, "clear_image");
  }
  bool ok = r == VK_SUCCESS;
  uint32_t first = 0;
  if (ok) {
    const auto* px = static_cast<const uint32_t*>(readback.map);
    first = px[0];
    for (uint32_t i = 0; ok && i < kW * kH; ++i) ok = px[i] == 0xff0000ffu;
  }
  DestroyHostBuffer(gpu, readback);
  DestroyImage(gpu, img);
  if (r != VK_SUCCESS) return End("clear_image", r);
  char detail[96];
  snprintf(detail, sizeof(detail), "pixel 0 = 0x%08x, expected 0xff0000ff", first);
  return Check("clear_image", ok, detail);
}

VkResult CreateShader(Gpu& gpu, const uint32_t* code, size_t bytes, VkShaderModule& out) {
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = bytes;
  info.pCode = code;
  return vkCreateShaderModule(gpu.device, &info, nullptr, &out);
}

// Depth image with a ZCULL plane (on drivers that publish ZCULL geometry),
// a compiled pipeline (NAK) and a depth-tested draw (MME draw macros).
bool StepDraw(Gpu& gpu, bool with_depth) {
  const char* step = with_depth ? "depth_draw" : "draw";
  Begin(step);
  constexpr uint32_t kW = 320, kH = 180;
  const VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
  Image color, depth;
  HostBuffer readback;
  VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;

  VkResult r = CreateImage(gpu, VK_FORMAT_R8G8B8A8_UNORM,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, kW, kH, color);
  if (r == VK_SUCCESS && with_depth) {
    Log("  creating %ux%u D32 depth image (ZCULL plane when available)", kW, kH);
    r = CreateImage(gpu, depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT, kW, kH, depth);
  }
  if (r == VK_SUCCESS) r = CreateHostBuffer(gpu, kW * kH * 4, readback);
  if (r == VK_SUCCESS) r = CreateShader(gpu, kVertSpv, sizeof(kVertSpv), vs);
  if (r == VK_SUCCESS) r = CreateShader(gpu, kFragSpv, sizeof(kFragSpv), fs);
  if (r == VK_SUCCESS) {
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &push;
    r = vkCreatePipelineLayout(gpu.device, &info, nullptr, &layout);
  }
  if (r == VK_SUCCESS) {
    Log("  compiling pipeline (NAK)");
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0, 0, float(kW), float(kH), 0, 1};
    VkRect2D scissor{{0, 0}, {kW, kH}};
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.pViewports = &viewport;
    vp.scissorCount = 1;
    vp.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = with_depth;
    ds.depthWriteEnable = with_depth;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState blend_att{};
    blend_att.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend_att;
    const VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &color_format;
    rendering.depthAttachmentFormat = with_depth ? depth_format : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState = &ms;
    info.pDepthStencilState = &ds;
    info.pColorBlendState = &cb;
    info.layout = layout;
    r = vkCreateGraphicsPipelines(gpu.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
  }
  if (r == VK_SUCCESS) r = BeginCommands(gpu);
  if (r == VK_SUCCESS) {
    Barrier(gpu.cmd, color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (with_depth)
      Barrier(gpu.cmd, depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color_att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color_att.imageView = color.view;
    color_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo depth_att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth_att.imageView = depth.view;
    depth_att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth_att.clearValue.depthStencil.depth = 1.0f;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, {kW, kH}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color_att;
    rendering.pDepthAttachment = with_depth ? &depth_att : nullptr;
    vkCmdBeginRendering(gpu.cmd, &rendering);
    vkCmdBindPipeline(gpu.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    // First draw: green at depth 0.5 passes against the cleared 1.0. Second
    // draw: red at the same depth must fail LESS when depth is enabled.
    const float green[4] = {0, 1, 0, 1};
    const float red[4] = {1, 0, 0, 1};
    vkCmdPushConstants(gpu.cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, green);
    vkCmdDraw(gpu.cmd, 3, 1, 0, 0);
    if (with_depth) {
      vkCmdPushConstants(gpu.cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, red);
      vkCmdDraw(gpu.cmd, 3, 1, 0, 0);
    }
    vkCmdEndRendering(gpu.cmd);
    Barrier(gpu.cmd, color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {kW, kH, 1};
    vkCmdCopyImageToBuffer(gpu.cmd, color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback.buffer, 1, &copy);
    r = SubmitAndWait(gpu, step);
  }
  uint32_t center = 0;
  if (r == VK_SUCCESS) center = static_cast<const uint32_t*>(readback.map)[(kH / 2) * kW + kW / 2];
  if (pipeline) vkDestroyPipeline(gpu.device, pipeline, nullptr);
  if (layout) vkDestroyPipelineLayout(gpu.device, layout, nullptr);
  if (vs) vkDestroyShaderModule(gpu.device, vs, nullptr);
  if (fs) vkDestroyShaderModule(gpu.device, fs, nullptr);
  DestroyHostBuffer(gpu, readback);
  DestroyImage(gpu, depth);
  DestroyImage(gpu, color);
  if (r != VK_SUCCESS) return End(step, r);
  char detail[96];
  snprintf(detail, sizeof(detail), "center pixel 0x%08x, expected 0xff00ff00", center);
  return Check(step, center == 0xff00ff00u, detail);
}

bool StepPresent(Gpu& gpu) {
  Begin("wsi");
  NWindow* window = nwindowGetDefault();
  if (!window || !nwindowIsValid(window)) return Check("wsi", false, "no default NWindow");
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkViSurfaceCreateInfoNN surface_info{VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN};
  surface_info.window = window;
  VkResult r = vkCreateViSurfaceNN(gpu.instance, &surface_info, nullptr, &surface);
  if (r != VK_SUCCESS) return End("wsi", r);
  VkBool32 supported = VK_FALSE;
  vkGetPhysicalDeviceSurfaceSupportKHR(gpu.physical, gpu.family, surface, &supported);
  VkSurfaceCapabilitiesKHR caps{};
  r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu.physical, surface, &caps);
  Log("  surface supported %u, extent %ux%u, images %u..%u", supported, caps.currentExtent.width,
      caps.currentExtent.height, caps.minImageCount, caps.maxImageCount);
  uint32_t format_count = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(gpu.physical, surface, &format_count, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(format_count);
  vkGetPhysicalDeviceSurfaceFormatsKHR(gpu.physical, surface, &format_count, formats.data());
  if (r != VK_SUCCESS || format_count == 0) {
    vkDestroySurfaceKHR(gpu.instance, surface, nullptr);
    return End("wsi", r != VK_SUCCESS ? r : VK_ERROR_FORMAT_NOT_SUPPORTED);
  }
  VkExtent2D extent = caps.currentExtent;
  if (extent.width == UINT32_MAX) extent = {1280, 720};
  VkSwapchainCreateInfoKHR sc{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  sc.surface = surface;
  sc.minImageCount = caps.minImageCount < 2 ? 2 : caps.minImageCount;
  sc.imageFormat = formats[0].format;
  sc.imageColorSpace = formats[0].colorSpace;
  sc.imageExtent = extent;
  sc.imageArrayLayers = 1;
  sc.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  sc.preTransform = caps.currentTransform;
  sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  sc.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  sc.clipped = VK_TRUE;
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  Log("  vkCreateSwapchainKHR format %d %ux%u", int(sc.imageFormat), extent.width, extent.height);
  r = vkCreateSwapchainKHR(gpu.device, &sc, nullptr, &swapchain);
  if (r != VK_SUCCESS) {
    vkDestroySurfaceKHR(gpu.instance, surface, nullptr);
    return End("wsi", r);
  }
  uint32_t image_count = 0;
  vkGetSwapchainImagesKHR(gpu.device, swapchain, &image_count, nullptr);
  std::vector<VkImage> images(image_count);
  vkGetSwapchainImagesKHR(gpu.device, swapchain, &image_count, images.data());
  Log("  swapchain with %u images", image_count);
  VkSemaphoreCreateInfo sem_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
  vkCreateSemaphore(gpu.device, &sem_info, nullptr, &acquired);
  vkCreateSemaphore(gpu.device, &sem_info, nullptr, &rendered);
  uint32_t presented = 0;
  for (uint32_t frame = 0; frame < g_frames && r == VK_SUCCESS && appletMainLoop(); ++frame) {
    const bool verbose = frame < 3 || frame + 1 == g_frames;
    uint32_t index = 0;
    if (verbose) Log("  frame %u: acquire", frame);
    r = vkAcquireNextImageKHR(gpu.device, swapchain, 5000000000ull, acquired, VK_NULL_HANDLE, &index);
    if (r == VK_SUBOPTIMAL_KHR) r = VK_SUCCESS;
    if (r != VK_SUCCESS) break;
    r = BeginCommands(gpu);
    if (r != VK_SUCCESS) break;
    Barrier(gpu.cmd, images[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue color{};
    color.float32[0] = 0.1f;
    color.float32[1] = float(frame % 60) / 60.0f;
    color.float32[2] = 0.6f;
    color.float32[3] = 1.0f;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(gpu.cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    Barrier(gpu.cmd, images[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    r = vkEndCommandBuffer(gpu.cmd);
    if (r != VK_SUCCESS) break;
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &acquired;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &gpu.cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &rendered;
    if (verbose) Log("  frame %u: submit image %u", frame, index);
    r = vkQueueSubmit(gpu.queue, 1, &submit, gpu.fence);
    if (r != VK_SUCCESS) break;
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &rendered;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &index;
    if (verbose) Log("  frame %u: present", frame);
    r = vkQueuePresentKHR(gpu.queue, &present);
    if (r == VK_SUBOPTIMAL_KHR) r = VK_SUCCESS;
    if (r != VK_SUCCESS) break;
    r = vkWaitForFences(gpu.device, 1, &gpu.fence, VK_TRUE, 5000000000ull);
    if (r == VK_SUCCESS) r = vkResetFences(gpu.device, 1, &gpu.fence);
    if (r == VK_SUCCESS) ++presented;
  }
  Log("  presented %u of %u frames", presented, g_frames);
  vkDeviceWaitIdle(gpu.device);
  vkDestroySemaphore(gpu.device, acquired, nullptr);
  vkDestroySemaphore(gpu.device, rendered, nullptr);
  vkDestroySwapchainKHR(gpu.device, swapchain, nullptr);
  vkDestroySurfaceKHR(gpu.instance, surface, nullptr);
  return End("wsi", r);
}

void Teardown(Gpu& gpu) {
  Begin("teardown");
  if (gpu.device) {
    vkDeviceWaitIdle(gpu.device);
    if (gpu.fence) vkDestroyFence(gpu.device, gpu.fence, nullptr);
    if (gpu.pool) vkDestroyCommandPool(gpu.device, gpu.pool, nullptr);
    Log("  vkDestroyDevice");
    vkDestroyDevice(gpu.device, nullptr);
  }
  if (gpu.instance) vkDestroyInstance(gpu.instance, nullptr);
  gpu = Gpu{};
  Check("teardown", true, nullptr);
}

void Run() {
  Gpu gpu;
  // Each stage runs only if the previous one succeeded; stop_after ends early
  // but still tears down cleanly.
  if (!StepGuestMemory() || StopAfter("guest_memory")) return;
  if (!StepInstance(gpu) || StopAfter("instance")) return Teardown(gpu);
  if (!StepPhysicalDevice(gpu) || StopAfter("physical_device")) return Teardown(gpu);
  if (!StepDevice(gpu) || StopAfter("device")) return Teardown(gpu);
  if (!StepEmptySubmit(gpu) || StopAfter("empty_submit")) return Teardown(gpu);
  // Content mismatches do not stop the run; host crashes or errors do.
  if (!Skipped("copy")) StepBufferOp(gpu, false);
  if (StopAfter("copy")) return Teardown(gpu);
  if (!Skipped("fill")) StepBufferOp(gpu, true);
  if (StopAfter("fill")) return Teardown(gpu);
  StepClearImage(gpu);
  if (StopAfter("clear_image")) return Teardown(gpu);
  if (!Skipped("draw")) StepDraw(gpu, false);
  if (StopAfter("draw")) return Teardown(gpu);
  if (!Skipped("depth")) StepDraw(gpu, true);
  if (StopAfter("depth_draw")) return Teardown(gpu);
  if (!Skipped("wsi")) StepPresent(gpu);
  Teardown(gpu);
}

}  // namespace

int main(int, char**) {
  mkdir("sdmc:/switch", 0777);
  mkdir(kDir, 0777);
  g_log = fopen(kLog, "w");
  Log("Superman Returns NX Vulkan probe (no game code or data)");
  LogPlatform();
  ReadConfig();
  Run();
  ReleaseGuestMemory();
  Log("RESULT %s (%d failed step%s)", g_failures == 0 ? "PASS" : "FAIL", g_failures,
      g_failures == 1 ? "" : "s");
  if (g_log) fclose(g_log);
  return g_failures == 0 ? 0 : 1;
}
