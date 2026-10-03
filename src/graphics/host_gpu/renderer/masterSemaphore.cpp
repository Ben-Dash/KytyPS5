#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include "common/assert.h"
#include "graphics/host_gpu/graphicContext.h"

#include <cinttypes>
#include <cstdio>
#include <vector>

namespace Libs::Graphics {

namespace {

// KYTY_DEBUG_DEVICE_FAULT: what the driver recorded for the lost device.
void ReportDeviceFault(GraphicContext& graphics, vk::Result result) {
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
	ReportDeviceFault(m_graphics, result);
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
	ReportDeviceFault(m_graphics, result);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
	Refresh();
}

} // namespace Libs::Graphics
