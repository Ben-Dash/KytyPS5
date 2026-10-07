#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include "common/assert.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/gpuZones.h"

#include <cinttypes>
#include <cstdio>
#include <iterator>
#include <utility>
#include <vector>

namespace Libs::Graphics {

namespace {

// KYTY_DEBUG_GPU_CHECKPOINTS: the last checkpoints each pipeline stage of the queue passed, with
// the zone and key (shader hash for game draws and dispatches) recorded for them.
void ReportCheckpoints(GraphicContext& graphics) {
	static constexpr const char* ZoneNames[] = {"unmarked",  "draw",     "draw-predicated",
	                                            "dispatch",  "mesh-args", "tiler",
	                                            "dcc-clear", "fault-buffer", "blit",
	                                            "image-copy", "buffer-copy"};
	for (const auto& [name, queue]: {std::pair {"graphics", graphics.queue},
	                                  std::pair {"readback", graphics.readback_queue}}) {
		if (queue == nullptr) {
			continue;
		}
		uint32_t count = 0;
		VULKAN_HPP_DEFAULT_DISPATCHER.vkGetQueueCheckpointDataNV(queue, &count, nullptr);
		std::vector<VkCheckpointDataNV> data(count, {VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV});
		VULKAN_HPP_DEFAULT_DISPATCHER.vkGetQueueCheckpointDataNV(queue, &count, data.data());
		std::printf("gpu-checkpoints: %s queue, %u entries (newest checkpoint %" PRIu64 ")\n", name,
		            count, GpuZones::g_checkpoint_next.load() - 1);
		for (const auto& entry: data) {
			const auto  index = reinterpret_cast<uintptr_t>(entry.pCheckpointMarker);
			const auto& mark  = GpuZones::g_checkpoints[index % GpuZones::CheckpointRing];
			const auto  zone  = static_cast<size_t>(mark.zone);
			std::printf("gpu-checkpoint: stage=%s checkpoint=%" PRIu64 " zone=%s key=%016" PRIx64 "\n",
			            vk::to_string(static_cast<vk::PipelineStageFlagBits>(entry.stage)).c_str(),
			            static_cast<uint64_t>(index),
			            zone < std::size(ZoneNames) ? ZoneNames[zone] : "?", mark.key);
		}
		// The marks between the last finished and the last started checkpoint: the hung command
		// is among them (the driver reports only the newest checkpoint per stage).
		uintptr_t first = UINTPTR_MAX;
		uintptr_t last  = 0;
		for (const auto& entry: data) {
			const auto index = reinterpret_cast<uintptr_t>(entry.pCheckpointMarker);
			first            = std::min(first, index);
			last             = std::max(last, index);
		}
		for (uintptr_t index = first; count != 0 && index <= last + 2u && index - first < 64u;
		     index++) {
			const auto& mark = GpuZones::g_checkpoints[index % GpuZones::CheckpointRing];
			const auto  zone = static_cast<size_t>(mark.zone);
			std::printf("gpu-checkpoint-range: checkpoint=%" PRIu64 " zone=%s key=%016" PRIx64
			            " count=%u instances=%u vs=%016" PRIx64 " indirect=%016" PRIx64 "\n",
			            static_cast<uint64_t>(index),
			            zone < std::size(ZoneNames) ? ZoneNames[zone] : "?", mark.key,
			            static_cast<uint32_t>(mark.detail[0] >> 32u),
			            static_cast<uint32_t>(mark.detail[0]), mark.detail[1], mark.detail[2]);
		}
	}
	std::fflush(stdout);
}

// KYTY_DEBUG_OP_RING: the marks of the submissions the GPU had not completed when the device
// was lost (ticks above `completed`), newest last; the hung command is among them.
void ReportOpRing(uint64_t completed) {
	const auto next  = GpuZones::g_checkpoint_next.load(std::memory_order_relaxed);
	const auto first = next > GpuZones::CheckpointRing ? next - GpuZones::CheckpointRing : 1;
	uint64_t   begin = next;
	for (uint64_t index = next; index > first; index--) {
		if (GpuZones::g_checkpoint_ticks[(index - 1) % GpuZones::CheckpointRing] <= completed) {
			break;
		}
		begin = index - 1;
	}
	std::printf("gpu-op-ring: completed tick %" PRIu64 ", %" PRIu64 " marks in flight\n", completed,
	            next - begin);
	static constexpr const char* ZoneNames[] = {"unmarked",  "draw",     "draw-predicated",
	                                            "dispatch",  "mesh-args", "tiler",
	                                            "dcc-clear", "fault-buffer", "blit",
	                                            "image-copy", "buffer-copy"};
	for (uint64_t index = begin; index < next; index++) {
		const auto& mark = GpuZones::g_checkpoints[index % GpuZones::CheckpointRing];
		const auto  zone = static_cast<size_t>(mark.zone);
		std::printf("gpu-op: tick=%" PRIu64 " zone=%s key=%016" PRIx64 " count=%u instances=%u vs=%016" PRIx64
		            " indirect=%016" PRIx64 "\n",
		            GpuZones::g_checkpoint_ticks[index % GpuZones::CheckpointRing],
		            zone < std::size(ZoneNames) ? ZoneNames[zone] : "?", mark.key,
		            static_cast<uint32_t>(mark.detail[0] >> 32u), static_cast<uint32_t>(mark.detail[0]),
		            mark.detail[1], mark.detail[2]);
	}
	std::fflush(stdout);
}

// KYTY_DEBUG_DEVICE_FAULT: what the driver recorded for the lost device.
void ReportDeviceFault(GraphicContext& graphics, vk::Result result, uint64_t completed_tick) {
	if (graphics.checkpoints_enabled && result == vk::Result::eErrorDeviceLost) {
		ReportCheckpoints(graphics);
	}
	if (GpuZones::g_op_ring && result == vk::Result::eErrorDeviceLost) {
		ReportOpRing(completed_tick);
	}
	if (!graphics.device_fault_enabled || result != vk::Result::eErrorDeviceLost) {
		return;
	}
	vk::DeviceFaultCountsEXT counts {};
	if (graphics.device.getFaultInfoEXT(&counts, nullptr) != vk::Result::eSuccess) {
		std::printf("device-fault: no fault info\n");
		std::fflush(stdout);
		return;
	}
	std::vector<vk::DeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
	std::vector<vk::DeviceFaultVendorInfoEXT>  vendors(counts.vendorInfoCount);
	counts.vendorBinarySize = 0;
	vk::DeviceFaultInfoEXT info {};
	info.pAddressInfos = addresses.data();
	info.pVendorInfos  = vendors.data();
	const auto fetched = graphics.device.getFaultInfoEXT(&counts, &info);
	std::printf("device-fault: result=%d addresses=%u vendors=%u description=%s\n",
	            static_cast<int>(fetched), counts.addressInfoCount, counts.vendorInfoCount,
	            info.description.data());
	for (uint32_t i = 0; i < counts.addressInfoCount; i++) {
		std::printf("device-fault-address: type=%s address=0x%016" PRIx64 " precision=0x%" PRIx64
		            "\n",
		            vk::to_string(addresses[i].addressType).c_str(),
		            static_cast<uint64_t>(addresses[i].reportedAddress),
		            static_cast<uint64_t>(addresses[i].addressPrecision));
	}
	for (uint32_t i = 0; i < counts.vendorInfoCount; i++) {
		std::printf("device-fault-vendor: code=0x%" PRIx64 " data=0x%" PRIx64 " %s\n",
		            static_cast<uint64_t>(vendors[i].vendorFaultCode),
		            static_cast<uint64_t>(vendors[i].vendorFaultData),
		            vendors[i].description.data());
	}
	std::fflush(stdout);
}

} // namespace

MasterSemaphore::MasterSemaphore(GraphicContext& graphics): m_graphics(graphics) {
	vk::SemaphoreTypeCreateInfo type_info {};
	type_info.semaphoreType = vk::SemaphoreType::eTimeline;
	type_info.initialValue  = 0;

	vk::SemaphoreCreateInfo create_info {};
	create_info.pNext = &type_info;

	const auto result = m_graphics.device.createSemaphore(&create_info, nullptr, &m_semaphore);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_semaphore == nullptr);
}

MasterSemaphore::~MasterSemaphore() {
	if (m_semaphore != nullptr) {
		m_graphics.device.destroySemaphore(m_semaphore, nullptr);
	}
}

void MasterSemaphore::Refresh() {
	uint64_t   counter = 0;
	const auto result  = m_graphics.device.getSemaphoreCounterValue(m_semaphore, &counter);
	ReportDeviceFault(m_graphics, result, m_gpu_tick.load(std::memory_order_acquire));
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	auto known = m_gpu_tick.load(std::memory_order_acquire);
	while (known < counter &&
	       !m_gpu_tick.compare_exchange_weak(known, counter, std::memory_order_release,
	                                         std::memory_order_relaxed)) {
	}
}

void MasterSemaphore::Wait(uint64_t tick) {
	if (IsFree(tick)) {
		return;
	}
	Refresh();
	if (IsFree(tick)) {
		return;
	}

	vk::SemaphoreWaitInfo wait_info {};
	wait_info.semaphoreCount = 1;
	wait_info.pSemaphores    = &m_semaphore;
	wait_info.pValues        = &tick;

	const auto result = m_graphics.device.waitSemaphores(&wait_info, UINT64_MAX);
	ReportDeviceFault(m_graphics, result, m_gpu_tick.load(std::memory_order_acquire));
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
	Refresh();
}

} // namespace Libs::Graphics
