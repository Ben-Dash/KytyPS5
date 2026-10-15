#include "graphics/presentation/renderDoc.h"
#include "graphics/presentation/renderDocCaptureRequest.h"

#include "common/logging/log.h"
#include "common/switches.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/renderContext.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <renderdoc_app.h>
#include <string>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#else
#include <dlfcn.h>
#endif

namespace Libs::Graphics {

enum class RenderDocState : uint32_t {
	Idle,
	Requested,
	Capturing,
};

static RENDERDOC_API_1_6_0*        g_api             = nullptr;
static std::atomic<RenderDocState> g_state           = RenderDocState::Idle;
static uint32_t                    g_captured_flips  = 0;
static std::atomic_bool            g_unavailable_log = false;

// RenderDoc messages go to the console as well as the log: the log is silent with
// --printf-direction Silent, and the user has to see that F1 took and where the file went.
template <typename... Args>
static void Say(const char* format, Args... args) {
	LOGF(format, args...);
	std::printf(format, args...);
	std::fflush(stdout);
}

static RenderDocCaptureRequest& AutomaticCaptureRequest() {
	static RenderDocCaptureRequest request(KYTY_GETENV("KYTY_DEBUG_RENDERDOC_CAPTURE_REQUEST_FILE"));
	return request;
}

static void ArmAutomaticCaptureRequest() {
	using Status = RenderDocCaptureRequest::Status;
	const auto status = AutomaticCaptureRequest().ArmStatus();
	if (status == Status::Pending) {
		Say("RenderDoc: automatic request armed; waiting for a fresh empty gate file\n");
	} else if (status != Status::Disabled) {
		Say("RenderDoc: automatic request rejected at startup (status %u)\n",
		    static_cast<unsigned>(status));
	}
}

static void PollAutomaticCaptureRequest() {
	using Status = RenderDocCaptureRequest::Status;
	const auto status = AutomaticCaptureRequest().Poll(
	    g_api != nullptr, g_state.load(std::memory_order_acquire) == RenderDocState::Idle);
	static auto last_status = Status::Disabled;
	if (status == Status::Request) {
		Say("RenderDoc: fresh gate requested one guest-flip capture\n");
		RenderDocRequestCapture();
	} else if (status != last_status &&
	           (status == Status::FileError || status == Status::InvalidGate)) {
		Say("RenderDoc: automatic request rejected (status %u)\n", static_cast<unsigned>(status));
	}
	last_status = status;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

static bool BindRenderDocApi(HMODULE module) {
	auto* get_api = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI"));
#else
static bool BindRenderDocApi(void* module) {
	auto* get_api = reinterpret_cast<pRENDERDOC_GetAPI>(::dlsym(module, "RENDERDOC_GetAPI"));
#endif
	if (get_api == nullptr) {
		return false;
	}

	void* api = nullptr;
	if (get_api(eRENDERDOC_API_Version_1_6_0, &api) != 1 || api == nullptr) {
		return false;
	}

	g_api = static_cast<RENDERDOC_API_1_6_0*>(api);
	g_api->SetCaptureKeys(nullptr, 0);
	g_api->UnloadCrashHandler();
	Say("RenderDoc: API 1.6.0 bound\n");
	return true;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

void RenderDocInit() {
	ArmAutomaticCaptureRequest();
	if (g_api != nullptr) {
		return;
	}

	auto* module = GetModuleHandleA("renderdoc.dll");
	if (module == nullptr) {
		HKEY key = nullptr;
		if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
		                  L"SOFTWARE\\Classes\\RenderDoc.RDCCapture.1\\DefaultIcon\\", 0, KEY_READ,
		                  &key) != ERROR_SUCCESS) {
			return;
		}

		std::array<wchar_t, MAX_PATH> path_buffer {};
		DWORD      path_size = static_cast<DWORD>(path_buffer.size() * sizeof(wchar_t));
		const auto result    = RegQueryValueExW(
		    key, L"", nullptr, nullptr, reinterpret_cast<LPBYTE>(path_buffer.data()), &path_size);
		RegCloseKey(key);
		if (result != ERROR_SUCCESS) {
			return;
		}

		auto path = std::filesystem::path(path_buffer.data()).parent_path() / "renderdoc.dll";
		module    = LoadLibraryW(path.c_str());
		if (module == nullptr) {
			return;
		}
	}

	if (!BindRenderDocApi(module)) {
		Say("RenderDoc: API 1.6.0 is unavailable\n");
	}
}

#else

void RenderDocInit() {
	ArmAutomaticCaptureRequest();
	if (g_api != nullptr) {
		return;
	}

	auto* module = ::dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
	if (module == nullptr) {
		module = ::dlopen("librenderdoc.so", RTLD_NOW);
	}
	if (module == nullptr) {
		return;
	}

	if (!BindRenderDocApi(module)) {
		Say("RenderDoc: API 1.6.0 is unavailable\n");
		::dlclose(module);
	}
}

#endif

void RenderDocRequestCapture() {
	if (g_api == nullptr) {
		if (!g_unavailable_log.exchange(true)) {
			Say("RenderDoc: capture requested, but RenderDoc is unavailable\n");
		}
		return;
	}

	RenderDocState expected = RenderDocState::Idle;
	if (g_state.compare_exchange_strong(expected, RenderDocState::Requested)) {
		Say("RenderDoc: capture requested\n");
	}
}

static void StartCapture() {
	if (g_api->IsFrameCapturing() != 0) {
		g_state.store(RenderDocState::Idle, std::memory_order_release);
		Say("RenderDoc: capture request ignored because a capture is already active\n");
		return;
	}

	const auto capture_id   = std::chrono::duration_cast<std::chrono::microseconds>(
	                              std::chrono::system_clock::now().time_since_epoch())
	                              .count();
	// An absolute path in the working directory's _RenderDoc folder: what RenderDoc does with a
	// relative template is not specified, and the file has to be where the user looks.
	std::error_code error;
	std::filesystem::create_directories("_RenderDoc", error);
	const auto capture_path =
	    (std::filesystem::absolute("_RenderDoc", error) / ("kyty_" + std::to_string(capture_id)))
	        .string();
	g_api->SetCaptureFilePathTemplate(capture_path.c_str());
	g_api->StartFrameCapture(nullptr, nullptr);
	if (g_api->IsFrameCapturing() == 0) {
		g_state.store(RenderDocState::Idle, std::memory_order_release);
		Say("RenderDoc: capture failed to start\n");
		return;
	}
	g_captured_flips = 0;
	g_state.store(RenderDocState::Capturing, std::memory_order_release);
	Say("RenderDoc: capture started\n");
}

void RenderDocOnGuestFlip(RenderContext& renderer) {
	if (g_api == nullptr) {
		return;
	}
	PollAutomaticCaptureRequest();
	const auto state = g_state.load(std::memory_order_acquire);
	if (state == RenderDocState::Idle) {
		return;
	}
	if (state == RenderDocState::Capturing) {
		Say("RenderDoc: captured guest flip %u/2\n", ++g_captured_flips);
		if (g_captured_flips < 2) {
			return;
		}
	}

	// Capture boundaries follow presentation and exclude concurrent queue access.
	Common::LockGuard render_lock(renderer.GetMutex());
	Common::LockGuard queue_lock(renderer.GetGraphics().queue_mutex);
	if (state == RenderDocState::Requested) {
		StartCapture();
	} else {
		const auto ok = g_api->EndFrameCapture(nullptr, nullptr);
		g_state.store(RenderDocState::Idle, std::memory_order_release);
		if (ok == 0) {
			Say("RenderDoc: capture failed\n");
			return;
		}
		// EndFrameCapture has written the file: name it with its full path.
		const auto count  = g_api->GetNumCaptures();
		uint32_t   length = 0;
		if (count == 0 || g_api->GetCapture(count - 1, nullptr, &length, nullptr) == 0 || length == 0) {
			Say("RenderDoc: capture finished, but RenderDoc lists no capture file\n");
			return;
		}
		std::string path(length, ' ');
		uint64_t    timestamp = 0;
		if (g_api->GetCapture(count - 1, path.data(), &length, &timestamp) != 0) {
			path.resize(std::strlen(path.c_str()));
			Say("RenderDoc: capture %u written to %s\n", count, path.c_str());
		}
	}
}

} // namespace Libs::Graphics
