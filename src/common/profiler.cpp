#include "common/profiler.h"

#include "common/emulatorConfig.h"

#include <common/TracyProtocol.hpp>
#include <common/TracyVersion.hpp>
#include <cstdio>
#include <tracy/Tracy.hpp>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <array>
#endif

namespace Profiler {

void SetThreadName(const char* name) {
	if (name == nullptr) {
		return;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// OS names are also useful with profiling disabled. Calling Tracy's naming
	// helper before StartupProfiler would access its delayed/manual-lifetime
	// data, so only publish the native description on this path.
	using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
	static const auto set_description = []() -> SetDescription {
		const auto kernel = GetModuleHandleW(L"kernel32.dll");
		const auto candidate = kernel == nullptr ? nullptr : reinterpret_cast<SetDescription>(
		    GetProcAddress(kernel, "SetThreadDescription"));
		if (candidate != nullptr) return candidate;
		// Windows10 1607 exposes it dynamically through KernelBase.
		const auto base = GetModuleHandleW(L"kernelbase.dll");
		return base == nullptr ? nullptr : reinterpret_cast<SetDescription>(
		    GetProcAddress(base, "SetThreadDescription"));
	}();
	if (set_description != nullptr) {
		std::array<wchar_t, 256> native_name {};
		// Thread/guest names are UTF-8. An invalid or oversized name leaves the
		// optional native description alone; it never affects guest execution.
		if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1,
		                        native_name.data(), static_cast<int>(native_name.size())) > 0) {
			(void)set_description(GetCurrentThread(), native_name.data());
		}
	}
#endif
	if (tracy::ProfilerAvailable()) {
		tracy::SetThreadName(name);
	}
}

void Initialize() {
	if (Config::ProfilerEnabled() && !tracy::ProfilerAvailable()) {
		tracy::StartupProfiler();
		TracySetProgramName("KytyPS5");
		::printf("Tracy profiler enabled: client %d.%d.%d, protocol %u, "
		         "broadcast %u, connect to 127.0.0.1:8086\n",
		         tracy::Version::Major, tracy::Version::Minor, tracy::Version::Patch,
		         tracy::ProtocolVersion, tracy::BroadcastVersion);
	}
}

void Shutdown() {
	if (tracy::ProfilerAvailable()) {
		tracy::ShutdownProfiler();
	}
}

} // namespace Profiler
