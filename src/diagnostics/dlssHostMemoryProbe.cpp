#include "diagnostics/dlssHostMemoryProbe.h"

#include "common/common.h"
#include "kernel/memory.h"

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Diagnostics {

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
namespace {

bool DescribeNative(const char* phase, uint64_t address, MEMORY_BASIC_INFORMATION* info) {
	const auto bytes = VirtualQuery(reinterpret_cast<const void*>(address), info, sizeof(*info));
	const auto error = bytes == 0 ? GetLastError() : ERROR_SUCCESS;
	std::printf("DLSS HOST MEMORY %s: address=0x%016" PRIx64
	            " query_bytes=%zu error=%lu base=%p allocation=%p size=0x%zx"
	            " state=0x%lx type=0x%lx protection=0x%lx allocation_protection=0x%lx\n",
	            phase, address, static_cast<size_t>(bytes), static_cast<unsigned long>(error),
	            info->BaseAddress, info->AllocationBase, static_cast<size_t>(info->RegionSize),
	            static_cast<unsigned long>(info->State), static_cast<unsigned long>(info->Type),
	            static_cast<unsigned long>(info->Protect), static_cast<unsigned long>(info->AllocationProtect));
	std::fflush(stdout);
	return bytes == sizeof(*info);
}

} // namespace
#endif

bool RunDlssHostMemoryProbe() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	using namespace Libs::LibKernel::Memory;
	// Canonical guest user-space boundary, used only by this explicit fixture.
	constexpr uint64_t guest_base = 0x1000000000ull;
	constexpr size_t guest_page = 0x4000;
	constexpr int cpu_read_write = 0x03;
	constexpr int map_fixed = 0x10;
	constexpr int memory_type = 11;
	std::printf("DLSS HOST MEMORY host-image: base=%p\n", static_cast<void*>(GetModuleHandleW(nullptr)));
	MEMORY_BASIC_INFORMATION before{};
	DescribeNative("before", guest_base, &before);
	VirtualQueryInfo previous{};
	const auto previous_result = KernelVirtualQuery(reinterpret_cast<const void*>(guest_base), 0,
	                                                &previous, sizeof(previous));
	std::printf("DLSS HOST MEMORY before guest: result=0x%08x start=0x%016" PRIxPTR
	            " end=0x%016" PRIxPTR " protection=0x%x committed=%u direct=%u\n",
	            static_cast<unsigned>(previous_result), previous.start, previous.end,
	            previous.protection, previous.is_committed, previous.is_direct);
	if (previous_result == 0) {
		std::puts("DLSS HOST MEMORY FAIL: canonical fixture range already has a guest mapping; preserved");
		return false;
	}

	int64_t physical = -1;
	const auto allocate_result = KernelAllocateDirectMemory(
	    0, static_cast<int64_t>(KernelGetDirectMemorySize()), guest_page, guest_page, memory_type, &physical);
	std::printf("DLSS HOST MEMORY allocate: result=0x%08x physical=0x%016" PRIx64 " bytes=0x%zx\n",
	            static_cast<unsigned>(allocate_result), static_cast<uint64_t>(physical), guest_page);
	if (allocate_result != 0) {
		std::puts("DLSS HOST MEMORY FAIL: physical allocation setup failed");
		return false;
	}

	KernelBatchMapEntry entry{};
	entry.start = reinterpret_cast<void*>(guest_base);
	entry.offset = static_cast<uint64_t>(physical);
	entry.length = guest_page;
	entry.protection = cpu_read_write;
	entry.type = memory_type;
	entry.operation = 0; // MAP_OP_MAP_DIRECT
	int processed = -1;
	const auto map_result = KernelBatchMap2(&entry, 1, &processed, map_fixed);
	const bool mapped = map_result == 0;
	std::printf("DLSS HOST MEMORY map: result=0x%08x processed=%d requested=0x%016" PRIx64
	            " returned=%p\n", static_cast<unsigned>(map_result), processed, guest_base, entry.start);
	MEMORY_BASIC_INFORMATION native{};
	const bool native_query = DescribeNative(mapped ? "mapped" : "map-failed", guest_base, &native);
	VirtualQueryInfo info{};
	const auto query_result = KernelVirtualQuery(reinterpret_cast<const void*>(guest_base), 0, &info, sizeof(info));
	std::printf("DLSS HOST MEMORY guest: result=0x%08x start=0x%016" PRIxPTR
	            " end=0x%016" PRIxPTR " offset=0x%016" PRIx64
	            " protection=0x%x committed=%u direct=%u\n",
	            static_cast<unsigned>(query_result), info.start, info.end, info.offset,
	            info.protection, info.is_committed, info.is_direct);
	bool valid = mapped && processed == 1 && reinterpret_cast<uint64_t>(entry.start) == guest_base &&
	             query_result == 0 && info.start <= guest_base && info.end >= guest_base + guest_page &&
	             info.offset + (guest_base - info.start) == static_cast<uint64_t>(physical) &&
	             info.is_committed && info.is_direct && (info.protection & cpu_read_write) == cpu_read_write &&
	             native_query && native.State == MEM_COMMIT && native.Type == MEM_MAPPED &&
	             (native.Protect & 0xffu) == PAGE_READWRITE &&
	             reinterpret_cast<uint64_t>(native.BaseAddress) <= guest_base &&
	             reinterpret_cast<uint64_t>(native.BaseAddress) + native.RegionSize >= guest_base + guest_page;
	if (valid) {
		constexpr uint64_t first = 0x484f535456414c31ull;
		constexpr uint64_t last = 0x484f535456414c32ull;
		uint64_t first_read = 0, last_read = 0;
		SIZE_T transferred = 0;
		const auto process = GetCurrentProcess();
		const auto tail = guest_base + guest_page - sizeof(uint64_t);
		valid = WriteProcessMemory(process, reinterpret_cast<void*>(guest_base), &first, sizeof(first), &transferred) &&
		        transferred == sizeof(first) &&
		        WriteProcessMemory(process, reinterpret_cast<void*>(tail), &last, sizeof(last), &transferred) &&
		        transferred == sizeof(last) &&
		        ReadProcessMemory(process, reinterpret_cast<const void*>(guest_base), &first_read, sizeof(first_read), &transferred) &&
		        transferred == sizeof(first_read) &&
		        ReadProcessMemory(process, reinterpret_cast<const void*>(tail), &last_read, sizeof(last_read), &transferred) &&
		        transferred == sizeof(last_read) && first_read == first && last_read == last;
		std::printf("DLSS HOST MEMORY canaries: valid=%u first=0x%016" PRIx64 " last=0x%016" PRIx64 "\n",
		            static_cast<unsigned>(valid), first_read, last_read);
	}
	bool cleanup = true;
	if (mapped) {
		const auto unmap_result = KernelMunmap(reinterpret_cast<uint64_t>(entry.start), guest_page);
		cleanup = unmap_result == 0;
		std::printf("DLSS HOST MEMORY unmap: result=0x%08x\n", static_cast<unsigned>(unmap_result));
	}
	const auto release_result = KernelCheckedReleaseDirectMemory(physical, guest_page);
	cleanup = release_result == 0 && cleanup;
	std::printf("DLSS HOST MEMORY release: result=0x%08x\n", static_cast<unsigned>(release_result));
	MEMORY_BASIC_INFORMATION after{};
	DescribeNative("after-cleanup", guest_base, &after);
	std::printf("DLSS HOST MEMORY %s: mapped=%u validated=%u cleanup=%u\n", valid && cleanup ? "PASS" : "FAIL",
	            static_cast<unsigned>(mapped), static_cast<unsigned>(valid), static_cast<unsigned>(cleanup));
	std::fflush(stdout);
	return valid && cleanup;
#else
	std::puts("DLSS HOST MEMORY UNSUPPORTED: native Windows diagnostic required");
	return false;
#endif
}

} // namespace Diagnostics
