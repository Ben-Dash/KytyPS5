#include "graphics/host_gpu/handleRegistry.h"

#include "common/emulatorConfig.h"
#include "common/switches.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace Libs::Graphics::HandleRegistry {

namespace {

struct Entry {
	uint64_t generation = 0;
	Kind     kind       = Kind::Buffer;
	bool     dead       = false;
};

std::mutex                                g_mutex;
std::unordered_map<uint64_t, Entry>       g_entries;
uint64_t                                  g_next_generation = 1;
std::atomic<uint64_t>                     g_stale {0};
std::atomic<uint32_t>                     g_logged {0};

const char* KindName(Kind kind) {
	return kind == Kind::Image ? "image" : "buffer";
}

} // namespace

bool CheckEnabled() {
	static const bool enabled = [] {
		const char* text = KYTY_GETENV("KYTY_DEFERRED_HANDLE_CHECK");
		if (text != nullptr) {
			return std::strcmp(text, "0") != 0;
		}
		return Config::RenderDocEnabled() || KYTY_GETENV("KYTY_DEBUG_DEVICE_FAULT") != nullptr;
	}();
	return enabled;
}

void Register(uint64_t handle, Kind kind) {
	if (handle == 0) {
		return;
	}
	std::scoped_lock lock {g_mutex};
	auto&            entry = g_entries[handle];
	entry.generation       = g_next_generation++;
	entry.kind             = kind;
	entry.dead             = false;
}

void Unregister(uint64_t handle) {
	if (handle == 0) {
		return;
	}
	std::scoped_lock lock {g_mutex};
	if (const auto found = g_entries.find(handle); found != g_entries.end()) {
		found->second.dead = true;
	}
}

uint64_t Stamp(uint64_t handle) {
	if (!CheckEnabled() || handle == 0) {
		return 0;
	}
	std::scoped_lock lock {g_mutex};
	const auto       found = g_entries.find(handle);
	return found != g_entries.end() && !found->second.dead ? found->second.generation : 0;
}

bool Same(uint64_t handle, uint64_t stamp, const char* call, const char* role) {
	if (stamp == 0) {
		return true;
	}
	Entry now {};
	{
		std::scoped_lock lock {g_mutex};
		if (const auto found = g_entries.find(handle); found != g_entries.end()) {
			now = found->second;
		}
	}
	if (now.generation == stamp && !now.dead) {
		return true;
	}
	g_stale.fetch_add(1, std::memory_order_relaxed);
	if (g_logged.fetch_add(1, std::memory_order_relaxed) < 32) {
		std::printf("deferred-call: stale %s %s handle=0x%016llx recorded generation %llu, now %llu%s; "
		            "%s skipped\n",
		            KindName(now.kind), role, static_cast<unsigned long long>(handle),
		            static_cast<unsigned long long>(stamp),
		            static_cast<unsigned long long>(now.generation),
		            now.dead ? " (destroyed)" : " (recreated)", call);
		std::fflush(stdout);
	}
	return false;
}

uint64_t StaleCount() {
	return g_stale.load(std::memory_order_relaxed);
}

} // namespace Libs::Graphics::HandleRegistry
