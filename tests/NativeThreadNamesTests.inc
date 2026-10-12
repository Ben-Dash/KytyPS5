// Diagnostic-only native CPU probe. Include at global scope before main.
// No Vulkan device/game/profiler startup is required.
#include "common/profiler.h"
#include <string>
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace Libs::Graphics {
namespace {
void CheckNativeThreadNamesWithoutProfiler() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  constexpr const char* name = "NativeThreadNamesWithoutProfiler";
  Require(name, "disabled profiler control", !tracy::ProfilerAvailable(),
      "probe must begin with Tracy disabled; it is not a profile-mode test");
  const auto description = []() {
    PWSTR text = nullptr;
    const auto result = GetThreadDescription(GetCurrentThread(), &text);
    std::wstring output;
    if (SUCCEEDED(result) && text != nullptr) output = text;
    if (text != nullptr) LocalFree(text);
    return output;
  };
  const auto original = description();
  Profiler::SetThreadName("KytyNativeNameProbe");
  const auto ascii = description();
  const auto no_startup = !tracy::ProfilerAvailable();
  Profiler::SetThreadName(nullptr);
  const auto null_preserves = description() == ascii;
  Profiler::SetThreadName("Kyty-\xd0\xa2\xd0\xb5\xd1\x81\xd1\x82");
  const auto unicode = description();
  // Restore this diagnostic thread before making an assertion.
  (void)SetThreadDescription(GetCurrentThread(), original.c_str());
  Require(name, "OS name with profiler disabled", ascii == L"KytyNativeNameProbe",
      "Profiler::SetThreadName failed to publish an OS description with Tracy disabled");
  Require(name, "no Tracy startup", no_startup && !tracy::ProfilerAvailable(),
      "OS thread naming touched delayed/manual-lifetime profiler startup");
  Require(name, "null name preserves description", null_preserves,
      "null naming request erased a valid OS description");
  Require(name, "UTF8 description", unicode == L"Kyty-\u0422\u0435\u0441\u0442",
      "UTF8 name did not produce its exact native Unicode description");
  std::printf("[host]    %-32s ok\n", name);
#else
  std::puts("[host] NativeThreadNamesWithoutProfiler SKIP: Windows diagnostic");
#endif
}
}
}
