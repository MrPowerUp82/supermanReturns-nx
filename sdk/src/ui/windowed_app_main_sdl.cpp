/**
 * @file        ui/windowed_app_main_sdl.cpp
 * @brief       Entry point for windowed applications (SDL3 windowing)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cstdlib>
#if defined(__SWITCH__)
#include <cstdio>
#endif
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context_sdl.h>

#if REX_PLATFORM_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#endif

namespace {

#if defined(__SWITCH__)
// The regular file logger is initialized after SDL and application creation.
// Keep a synchronous boot breadcrumb so failures before that point leave evidence.
void SwitchBootTrace(const char* stage, int argc = 0, char** argv = nullptr) {
  FILE* trace = std::fopen("sdmc:/switch/superman-returns-nx/native-boot.log", "a");
  if (!trace) return;
  std::fprintf(trace, "[boot] %s", stage);
  if (argv && argc > 0 && argv[0]) std::fprintf(trace, " argv0=%s", argv[0]);
  std::fputc('\n', trace);
  std::fclose(trace);
}
#else
void SwitchBootTrace(const char*, int = 0, char** = nullptr) {}
#endif

int RunWindowedApp(int argc, char** argv) {
  SwitchBootTrace("main entered", argc, argv);
  auto remaining = rex::cvar::Init(argc, argv);
  SwitchBootTrace("cvars initialized");
  rex::cvar::ApplyEnvironment();
  rex::InitLoggingEarly();
  SwitchBootTrace("early logging initialized");

  int result;
  {
    rex::ui::SDLWindowedAppContext app_context;
    SwitchBootTrace("SDL context initializing");
    if (!app_context.Initialize()) {
      SwitchBootTrace("SDL context initialization failed");
      return EXIT_FAILURE;
    }
    SwitchBootTrace("SDL context initialized");

#if REX_PLATFORM_WIN32
    // Apartment-threaded COM for shell dialogs.
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
      return EXIT_FAILURE;
    }
#endif

    std::unique_ptr<rex::ui::WindowedApp> app = rex::ui::GetWindowedAppCreator()(app_context);
    SwitchBootTrace("application created");

    // Match remaining positional args to the app's expected options.
    const auto& option_names = app->GetPositionalOptions();
    std::map<std::string, std::string> parsed;
    size_t count = std::min(remaining.size(), option_names.size());
    for (size_t i = 0; i < count; ++i) {
      parsed[option_names[i]] = remaining[i];
    }
    app->SetParsedArguments(std::move(parsed));

    SwitchBootTrace("application initializing");
    if (app->OnInitialize()) {
      SwitchBootTrace("application initialized; message loop entering");
      result = app_context.RunMainMessageLoop();
      SwitchBootTrace("message loop exited");
    } else {
      SwitchBootTrace("application initialization failed");
      result = EXIT_FAILURE;
    }

    app->InvokeOnDestroy();
  }

#if REX_PLATFORM_WIN32
  CoUninitialize();
#endif

  return result;
}

#if REX_PLATFORM_WIN32
// Convert wide argv from CommandLineToArgvW to UTF-8 for cvar::Init.
std::vector<std::string> WideArgsToUtf8(int argc, wchar_t** wargv) {
  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    std::wstring wide(wargv[i]);
    if (wide.empty()) {
      args.emplace_back();
      continue;
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr,
                                   0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), size,
                        nullptr, nullptr);
    args.push_back(std::move(utf8));
  }
  return args;
}
#endif

}  // namespace

#if REX_PLATFORM_WIN32

int WINAPI wWinMain(HINSTANCE hinstance, HINSTANCE hinstance_prev, LPWSTR command_line,
                    int show_cmd) {
  (void)hinstance;
  (void)hinstance_prev;
  (void)command_line;
  (void)show_cmd;

  int wargc = 0;
  wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
  auto utf8_args = WideArgsToUtf8(wargc, wargv);
  LocalFree(wargv);

  std::vector<char*> argv_ptrs;
  argv_ptrs.reserve(utf8_args.size());
  for (auto& s : utf8_args) {
    argv_ptrs.push_back(s.data());
  }
  return RunWindowedApp(static_cast<int>(argv_ptrs.size()), argv_ptrs.data());
}

#else

int main(int argc, char* argv[]) {
  return RunWindowedApp(argc, argv);
}

#endif
