#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <cstdlib>
#include <string>
#include "sr_native_system.h"
#include "sr_settings.h"
#include "sr_shader_registry.h"
// Source revision of this build (CMake: SR_BUILD_REVISION env var or git). It identifies the
// source, not the NRO bytes: an artifact hash cannot be embedded in the artifact itself.
#ifndef SR_BUILD_REVISION
#define SR_BUILD_REVISION "unknown"
#endif
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

  // The renderer is chosen once per run: no switching at runtime and no per-draw fallback.
  // An invalid value makes native setup fail (the SDK treats a failed presentation setup as
  // fatal) instead of silently running Xenos.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    const std::string mode = rex::cvar::GetFlagByName("sr_renderer");
    REXLOG_INFO("[sr-native] sr_renderer={} milestone=1 build={}", mode, SR_BUILD_REVISION);
    if (mode == "xenos") return;
    const bool valid = mode == "native";
    if (!valid) REXLOG_ERROR("Invalid sr_renderer: {}", mode);
    config.graphics = sr::native::CreateGraphicsSystem(valid);
  }

  // The SDK hard-exits after TerminateTitle without running destructors, so the native
  // workers are stopped and joined here first. No effect with Xenos.
  bool OnWindowCloseRequested() override {
    QuiesceNative();
    return true;
  }
  void OnClosing(rex::ui::UIEvent& e) override {
    QuiesceNative();
    rex::ReXApp::OnClosing(e);
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
    // Drawing with the offline shader pack needs the device features its SPIR-V
    // declares (64-bit integers, buffer addresses, descriptor arrays); they are
    // chosen when the Vulkan device is created, after this point.
    if (rex::cvar::GetFlagByName("pack_shaders") == "draw")
      SetDefault("vulkan_native_shader_features", "true");
    sr::native::InitializeRuntimeShaders();
  }

 private:
  void QuiesceNative() {
    if (runtime()) sr::native::QuiesceNativeGraphicsSystem(runtime()->graphics_system());
  }
  static void SetDefault(const char* name, const char* value) {
    if (rex::cvar::GetFlagInfo(name) &&
        rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault)
      rex::cvar::SetFlagByName(name, value);
  }
};
