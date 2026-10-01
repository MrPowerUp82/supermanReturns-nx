// Diagnostic NRO. This does not contain or execute Superman Returns.
#include <switch.h>
#include <cstdio>
#include <cstdarg>
#include <sys/stat.h>

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
