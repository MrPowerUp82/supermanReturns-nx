#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <cstdlib>
#include "sr_shader_registry.h"
#if REX_PLATFORM_SWITCH
#include <switch.h>
extern "C" void SrTouchEmulatorTlsGuard();
#endif

class SupermanReturnsApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
#if REX_PLATFORM_SWITCH
    SrTouchEmulatorTlsGuard();
#endif
    return std::unique_ptr<SupermanReturnsApp>(
        new SupermanReturnsApp(ctx, "superman_returns", PPCImageConfig));
  }

 protected:
  void OnConfigurePaths(rex::PathConfig& paths) override {
    const auto folder = rex::filesystem::GetExecutableFolder();
    if (paths.game_data_root.empty()) paths.game_data_root = folder / "game_root";
    // Keep saves and caches next to the NRO; preserve command-line overrides.
    if (rex::cvar::GetFlagByName("user_data_root").empty())
      paths.user_data_root = folder / "userdata";
    if (rex::cvar::GetFlagByName("cache_root").empty())
      paths.cache_root = paths.user_data_root / "cache";
    paths.config_path = folder / "superman_returns.toml";
  }

  void OnPostInitLogging() override {
    SetDefault("gpu_plugin", "xenos");
#if REX_PLATFORM_SWITCH
    SetDefault("switch_guest_yield_us", "0");
    REXLOG_INFO("Switch guest yield sleep: {} us (set switch_guest_yield_us=0 for baseline)",
                rex::cvar::GetFlagByName("switch_guest_yield_us"));
    // The game creates sockets during its startup even before gameplay.
    // libnx POSIX socket() needs the BSD service initialized by the application;
    // NetDll_WSAStartup's POSIX implementation only fills the guest WSADATA.
    // Keep the service alive until process teardown, after guest threads stop.
    const Result socket_result = socketInitializeDefault();
    if (R_SUCCEEDED(socket_result)) {
      REXLOG_INFO("Switch BSD sockets initialized");
    } else {
      REXLOG_ERROR("Switch BSD socket initialization failed: 0x{:08X}", socket_result);
    }
    if (envGetOwnProcessHandle() == INVALID_HANDLE) {
      SetDefault("switch_eager_memory", "true");
      // Use the NVDRV fence path while checking emulator command submission.
      ::setenv("NVK_SWITCH_MAPPED_COMPLETION", "false", 0);
      // Buffer copies on the DMA engine instead of NVK's compute shader:
      // yuzu-derived emulators read a storage buffer's size from the word
      // after its address in constant buffer 0 (an NVN convention), which for
      // NVK's copy shader is the other address, and allocate gigabytes on the
      // host. Verified with tools/switch/vk-probe; see docs/vk-probe.md.
      ::setenv("NVK_COPY_ENGINE", "1", 0);
    }
    SetDefault("mnk_mode", "false");
    // NFSMW's IO range cache has not been profiled with Superman's AST files.
    SetDefault("nfsmw_io_rangos_mb", "0");
#endif
    REXLOG_INFO("Superman Returns NX: experimental Vulkan/Xenos boot build");
    sr::native::InitializeRuntimeShaders();
  }

 private:
  static void SetDefault(const char* name, const char* value) {
    if (rex::cvar::GetFlagInfo(name) &&
        rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault)
      rex::cvar::SetFlagByName(name, value);
  }
};
