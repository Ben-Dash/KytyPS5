#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_FAULTMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_FAULTMANAGER_H_

#include "common/abi.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <array>
#include <cstdint>
#include <memory>

namespace Libs::Graphics {

class BufferCache;

class FaultManager {
	static constexpr size_t MaxPendingFaults = 8;

public:
	FaultManager(GraphicContext& graphics, CommandScheduler& scheduler, BufferCache& buffer_cache);
	~FaultManager();
	KYTY_CLASS_NO_COPY(FaultManager);

	// GPU binding: initialize this allocation before any producer can see it.
	[[nodiscard]] Buffer* GetFaultBuffer() noexcept;
	// Observation must not record GPU work or change first-use state.
	[[nodiscard]] const Buffer* PeekFaultBuffer() const noexcept { return &m_fault_buffer; }
	void                  ProcessFaultBuffer();
	// KYTY_VTX_EXPLODE_CHECK: once per presented frame, copies the explode region after the page
	// bitmap back and clears its counters; VertexExplode::Report prints it after the GPU is done.
	void                  ProcessExplodeLog();

private:
#if defined(KYTY_BINDLESS_LIFECYCLE_TEST_ACCESS)
	friend struct FaultBitmapInitializationTestAccess;
#endif
	GraphicContext&                            m_graphics;
	CommandScheduler&                          m_scheduler;
	BufferCache&                               m_buffer_cache;
	Buffer                                     m_fault_buffer;
	bool                                       m_bitmap_initialized = false;
	Buffer                                     m_download_buffer;
	std::array<uint64_t, MaxPendingFaults>      m_fault_areas {};
	uint32_t                                   m_current_area = 0;
	// KYTY_VTX_EXPLODE_CHECK only (empty otherwise).
	std::unique_ptr<Buffer>                    m_explode_download;
	std::array<uint64_t, MaxPendingFaults>      m_explode_areas {};
	uint32_t                                   m_explode_area    = 0;
	uint64_t                                   m_explode_frame   = UINT64_MAX;
	bool                                       m_explode_cleared = false;
	vk::DescriptorSetLayout                    m_fault_process_desc_layout = nullptr;
	vk::Pipeline                               m_fault_process_pipeline = nullptr;
	vk::PipelineLayout                         m_fault_process_pipeline_layout = nullptr;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_FAULTMANAGER_H_
