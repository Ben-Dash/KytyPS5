#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORHEAP_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORHEAP_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <deque>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;
class MasterSemaphore;

class DescriptorHeap {
public:
	DescriptorHeap(GraphicContext& graphics, MasterSemaphore& master_semaphore);
	~DescriptorHeap();
	KYTY_CLASS_NO_COPY(DescriptorHeap);

	[[nodiscard]] vk::DescriptorSet Commit(vk::DescriptorSetLayout layout);

private:
	static constexpr uint32_t DescriptorSetBatch = 32;

	// A pool keeps every set it allocated, by layout: when the GPU has finished with the pool,
	// its sets are handed out again as they are, instead of resetting the pool and allocating
	// them from the driver once more (about 5% of the GPU thread in ASTRO BOT's Sky Garden).
	struct Pool {
		vk::DescriptorPool                                                      pool = nullptr;
		std::unordered_map<vk::DescriptorSetLayout, std::vector<vk::DescriptorSet>> sets;
	};

	[[nodiscard]] bool Allocate(vk::DescriptorSetLayout layout, std::vector<vk::DescriptorSet>& sets);
	void               CreateDescriptorPool();

	GraphicContext&                                       m_graphics;
	MasterSemaphore&                                      m_master_semaphore;
	Pool                                                  m_current;
	std::deque<std::pair<Pool, uint64_t>>                 m_pending_pools;
	// Sets of the current pool handed out since it became current, by layout and in total.
	std::unordered_map<vk::DescriptorSetLayout, uint32_t> m_next;
	uint32_t                                              m_handed = 0;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORHEAP_H_
