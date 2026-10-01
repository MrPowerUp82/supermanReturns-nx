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
    if (envGetOwnProcessHandle() == INVALID_HANDLE) {
      SetDefault("switch_eager_memory", "true");
      // Use the NVDRV fence path while checking emulator command submission.
      ::setenv("NVK_SWITCH_MAPPED_COMPLETION", "false", 0);
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
