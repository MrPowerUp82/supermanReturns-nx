// Diagnostic NRO. This does not contain or execute Superman Returns.
#include <switch.h>
#include <cstdio>
#include <cstdarg>
#include <sys/stat.h>
#include <malloc.h>
#include <cstring>

extern "C" { u32 __nx_applet_type = AppletType_Application; }
static FILE* report;

static void Record(const char* format, ...) {
  va_list args;
  va_start(args, format);
  if (report) { vfprintf(report, format, args); fflush(report); }
  va_end(args);
  va_start(args, format);
  vprintf(format, args);
  va_end(args);
}

struct HandleProbe { Handle server, process; Result result; };
static void ReceiveHandle(void* arg) {
  auto& probe = *static_cast<HandleProbe*>(arg);
  hipcMakeRequest(armGetTls(), HipcMetadata{});
  s32 index = 0;
  probe.result = svcReplyAndReceive(&index, &probe.server, 1, INVALID_HANDLE, 2000000000ull);
  if (R_SUCCEEDED(probe.result)) {
    auto received = hipcParseRequest(armGetTls());
    if (received.meta.num_copy_handles == 1) probe.process = received.data.copy_handles[0];
  }
  svcCloseHandle(probe.server);
}

static void ProbeMemory() {
  HandleProbe probe{INVALID_HANDLE, INVALID_HANDLE, 0};
  Handle client = INVALID_HANDLE;
  Result rc = svcCreateSession(&probe.server, &client, 0, 0);
  Record("Memory probe create session: %08x\n", rc);
  if (R_FAILED(rc)) return;
  Thread thread{};
  rc = threadCreate(&thread, ReceiveHandle, &probe, nullptr, 0x4000, 0x2C, -2);
  Record("Memory probe create receiver: %08x\n", rc);
  if (R_FAILED(rc)) { svcCloseHandle(probe.server); svcCloseHandle(client); return; }
  rc = threadStart(&thread);
  Record("Memory probe start receiver: %08x\n", rc);
  if (R_SUCCEEDED(rc)) {
    Record("Memory probe sending process handle\n");
    HipcMetadata metadata{};
    metadata.type = CmifCommandType_Request;
    metadata.num_copy_handles = 1;
    auto request = hipcMakeRequest(armGetTls(), metadata);
    request.copy_handles[0] = CUR_PROCESS_HANDLE;
    rc = svcSendSyncRequest(client);
    Record("Memory probe send: %08x\n", rc);
    threadWaitForExit(&thread);
    Record("Memory probe received: %08x handle=%08x\n", probe.result, probe.process);
  } else svcCloseHandle(probe.server);
  threadClose(&thread);
  svcCloseHandle(client);
  if (probe.process == INVALID_HANDLE) return;
  void* backing = memalign(0x1000, 0x1000);
  if (!backing) { svcCloseHandle(probe.process); return; }
  std::memset(backing, 0, 0x1000);
  virtmemLock();
  void* shadow = virtmemFindCodeMemory(0x1000, 0x1000);
  void* alias = virtmemFindAslr(0x1000, 0x1000);
  virtmemUnlock();
  rc = svcMapProcessCodeMemory(probe.process, reinterpret_cast<u64>(shadow),
                               reinterpret_cast<u64>(backing), 0x1000);
  Record("Memory probe code mapping: %08x\n", rc);
  if (R_SUCCEEDED(rc)) {
    rc = svcSetProcessMemoryPermission(probe.process, reinterpret_cast<u64>(shadow), 0x1000, Perm_Rw);
    Record("Memory probe shadow permission: %08x\n", rc);
    rc = svcMapProcessMemory(alias, probe.process, reinterpret_cast<u64>(shadow), 0x1000);
    Record("Memory probe alias mapping: %08x\n", rc);
    if (R_SUCCEEDED(rc)) {
      *static_cast<volatile u32*>(alias) = 0x5347524d;
      Record("Memory probe mirror coherent: %s\n",
             *static_cast<volatile u32*>(shadow) == 0x5347524d ? "YES" : "NO");
      svcUnmapProcessMemory(alias, probe.process, reinterpret_cast<u64>(shadow), 0x1000);
    }
    svcUnmapProcessCodeMemory(probe.process, reinterpret_cast<u64>(shadow),
                              reinterpret_cast<u64>(backing), 0x1000);
  }
  free(backing);
  svcCloseHandle(probe.process);
}

static void ProbeGpuMappings(NvAddressSpace& space) {
  nvioctl_va_region regions[2]{};
  Result rc = nvioctlNvhostAsGpu_GetVARegions(space.fd, regions);
  Record("GPU VA regions: %08x\n", rc);
  if (R_FAILED(rc)) return;
  void* backing = memalign(65536, 65536);
  if (!backing) return;
  std::memset(backing, 0, 65536);
  NvMap map{};
  rc = nvMapCreate(&map, backing, 65536, 65536, NvKind_Pitch, true);
  Record("GPU buffer create: %08x\n", rc);
  if (R_SUCCEEDED(rc)) {
    for (const auto& region : regions) {
      Record("GPU region: start=%llx page=%x pages=%llx\n",
             (unsigned long long)region.offset, region.page_size,
             (unsigned long long)region.pages);
      if (region.page_size != space.page_size || region.pages < 3) continue;
      const u64 addresses[] = {region.offset + space.page_size,
          region.offset + region.pages * region.page_size - space.page_size};
      for (u64 address : addresses) {
        u64 reserved = 0;
        rc = nvioctlNvhostAsGpu_AllocSpace(space.fd, 1, space.page_size,
            NvMapBufferFlags_FixedOffset, address, &reserved);
        Record("GPU reserve %llx: %08x returned=%llx\n",
               (unsigned long long)address, rc, (unsigned long long)reserved);
        if (R_FAILED(rc)) continue;
        rc = nvAddressSpaceMapFixed(&space, nvMapGetHandle(&map), true, NvKind_Pitch, address);
        Record("GPU map %llx: %08x\n", (unsigned long long)address, rc);
        if (R_SUCCEEDED(rc)) nvAddressSpaceUnmap(&space, address);
        nvAddressSpaceFree(&space, address, 65536);
      }
    }
    u64 dynamic = 0;
    rc = nvAddressSpaceAlloc(&space, false, 65536, &dynamic);
    Record("GPU dynamic reserve: %08x returned=%llx\n", rc, (unsigned long long)dynamic);
    if (R_SUCCEEDED(rc)) {
      rc = nvAddressSpaceMapFixed(&space, nvMapGetHandle(&map), true, NvKind_Pitch, dynamic);
      Record("GPU dynamic map: %08x\n", rc);
      if (R_SUCCEEDED(rc)) nvAddressSpaceUnmap(&space, dynamic);
      nvAddressSpaceFree(&space, dynamic, 65536);
    }
    nvMapClose(&map);
  }
  free(backing);
}

int main() {
  consoleInit(nullptr);
  mkdir("sdmc:/switch", 0777);
  mkdir("sdmc:/switch/superman-returns-nx", 0777);
  report = fopen("sdmc:/switch/superman-returns-nx/platform-probe.log", "w");
  Record("Superman Returns NX platform probe (no game code)\n");
  u64 address = 0, size = 0;
  Result rc = svcGetInfo(&address, InfoType_AslrRegionAddress, CUR_PROCESS_HANDLE, 0);
  Record("ASLR address: result=%08x value=%016llx\n", rc, (unsigned long long)address);
  rc = svcGetInfo(&size, InfoType_AslrRegionSize, CUR_PROCESS_HANDLE, 0);
  Record("ASLR size: result=%08x value=%016llx\n", rc, (unsigned long long)size);
  Record("39-bit address space: %s\n", size >= (1ull << 38) ? "YES" : "NO");
  ProbeMemory();
  rc = nvInitialize();
  Record("nvInitialize: %08x\n", rc);
  if (R_SUCCEEDED(rc)) {
    Result gpu = nvGpuInit();
    Record("nvGpuInit: %08x\n", gpu);
    if (R_SUCCEEDED(gpu)) {
      NvAddressSpace space{};
      Result as = nvAddressSpaceCreate(&space, 65536);
      Record("nvAddressSpaceCreate: %08x\n", as);
      if (R_SUCCEEDED(as)) {
        ProbeGpuMappings(space);
        NvGpuChannel channel{};
        Result ch = nvGpuChannelCreate(&channel, &space, NvChannelPriority_Medium);
        Record("nvGpuChannelCreate: %08x\n", ch);
        if (R_SUCCEEDED(ch)) nvGpuChannelClose(&channel);
        nvAddressSpaceClose(&space);
      }
      nvGpuExit();
    }
    nvExit();
  }
  Record("Probe complete. This does not validate NVK or gameplay.\n");
  if (report) fclose(report);
  for (int i = 0; i < 300 && appletMainLoop(); ++i) consoleUpdate(nullptr);
  consoleExit(nullptr);
  return 0;
}
