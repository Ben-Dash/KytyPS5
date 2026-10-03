#include "graphics/host_gpu/renderer/pipeline/descriptorHeap.h"

#include "common/assert.h"
#include "common/profiler.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include <algorithm>

namespace Libs::Graphics {
namespace {

constexpr uint32_t   DescriptorHeapCount = 1024;
constexpr std::array DescriptorPoolSizes = {
    vk::DescriptorPoolSize {vk::DescriptorType::eStorageBuffer, 8192},
    vk::DescriptorPoolSize {vk::DescriptorType::eSampledImage, 8192},
    vk::DescriptorPoolSize {vk::DescriptorType::eStorageImage, 1024},
    vk::DescriptorPoolSize {vk::DescriptorType::eSampler, 1024},
};

} // namespace

DescriptorHeap::DescriptorHeap(GraphicContext& graphics, MasterSemaphore& master_semaphore)
    : m_graphics(graphics), m_master_semaphore(master_semaphore) {
	CreateDescriptorPool();
}

DescriptorHeap::~DescriptorHeap() {
	m_graphics.device.destroyDescriptorPool(m_current.pool, nullptr);
	for (const auto& [pool, tick]: m_pending_pools) {
		m_master_semaphore.Wait(tick);
		m_graphics.device.destroyDescriptorPool(pool.pool, nullptr);
	}
}

vk::DescriptorSet DescriptorHeap::Commit(vk::DescriptorSetLayout layout) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(layout == nullptr);

	for (;;) {
		auto& sets = m_current.sets[layout];
		auto& next = m_next[layout];
		if (next < sets.size() || Allocate(layout, sets)) {
			m_handed++;
			return sets[next++];
		}
		if (m_handed == 0) {
			// A reused pool whose sets are of other layouts: none is in use, start it over.
			EXIT_IF(std::ranges::none_of(m_current.sets,
			                             [](const auto& entry) { return !entry.second.empty(); }));
			EXIT_IF(m_graphics.device.resetDescriptorPool(m_current.pool, {}) !=
			        vk::Result::eSuccess);
			m_current.sets.clear();
			m_next.clear();
			continue;
		}
		m_pending_pools.emplace_back(std::move(m_current), m_master_semaphore.CurrentTick());
		m_current = {};
		if (m_master_semaphore.IsFree(m_pending_pools.front().second)) {
			m_current = std::move(m_pending_pools.front().first);
			m_pending_pools.pop_front();
		} else {
			CreateDescriptorPool();
		}
		m_next.clear();
		m_handed = 0;
	}
}

bool DescriptorHeap::Allocate(vk::DescriptorSetLayout layout, std::vector<vk::DescriptorSet>& sets) {
	std::array<vk::DescriptorSetLayout, DescriptorSetBatch> layouts;
	layouts.fill(layout);
	std::array<vk::DescriptorSet, DescriptorSetBatch> batch {};

	vk::DescriptorSetAllocateInfo allocate {};
	allocate.descriptorPool = m_current.pool;
	allocate.pSetLayouts    = layouts.data();

	for (uint32_t count = DescriptorSetBatch; count != 0; count /= 2) {
		allocate.descriptorSetCount = count;
		const auto result = m_graphics.device.allocateDescriptorSets(&allocate, batch.data());
		if (result == vk::Result::eSuccess) {
			sets.insert(sets.end(), batch.begin(), batch.begin() + count);
			return true;
		}
		EXIT_IF(result != vk::Result::eErrorOutOfPoolMemory &&
		        result != vk::Result::eErrorFragmentedPool);
	}
	return false;
}

void DescriptorHeap::CreateDescriptorPool() {
	vk::DescriptorPoolCreateInfo create {};
	create.maxSets       = DescriptorHeapCount;
	create.poolSizeCount = static_cast<uint32_t>(DescriptorPoolSizes.size());
	create.pPoolSizes    = DescriptorPoolSizes.data();
	EXIT_IF(m_graphics.device.createDescriptorPool(&create, nullptr, &m_current.pool) !=
	        vk::Result::eSuccess);
}

} // namespace Libs::Graphics
