#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PALETTESANITIZER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PALETTESANITIZER_H_

#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"

#include <cstdio>
#include <vector>

namespace Libs::Graphics {

inline void SanitizeSkinningPaletteBuffer(vk::CommandBuffer vk_buffer, const PreparedBindings& bindings) {
	for (size_t b_idx = 0; b_idx < bindings.buffers.size(); ++b_idx) {
		const auto& b_info = bindings.buffers[b_idx];
		const bool is_palette = (b_info.buffer != nullptr &&
		    (b_info.range == 124224 ||
		     (b_idx < bindings.buffer_sources.size() && bindings.buffer_sources[b_idx].size == 124224)));
		if (!is_palette) {
			continue;
		}

		// Pre-copy barrier: ensure compute/transfer writes are available to transfer reads
		vk::BufferMemoryBarrier pre_barrier {};
		pre_barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eTransferWrite;
		pre_barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite;
		pre_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		pre_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		pre_barrier.buffer = b_info.buffer;
		pre_barrier.offset = b_info.offset;
		pre_barrier.size = 124224;
		vk_buffer.pipelineBarrier(
		    vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eTransfer | vk::PipelineStageFlagBits::eAllCommands,
		    vk::PipelineStageFlagBits::eTransfer,
		    {}, 0, nullptr, 1, &pre_barrier, 0, nullptr);

		std::vector<vk::BufferCopy> copies;
		copies.reserve(192);

		// Slice 0: rows 1050..1113 inherit row 1049
		for (uint32_t r = 1050; r <= 1113; ++r) {
			copies.push_back(vk::BufferCopy {
			    .srcOffset = b_info.offset + 1049u * 48u,
			    .dstOffset = b_info.offset + r * 48u,
			    .size = 48u
			});
		}

		// Slice 1: rows 1562..1625 inherit row 1561
		for (uint32_t r = 1562; r <= 1625; ++r) {
			copies.push_back(vk::BufferCopy {
			    .srcOffset = b_info.offset + 1561u * 48u,
			    .dstOffset = b_info.offset + r * 48u,
			    .size = 48u
			});
		}

		// Slice 2: rows 2074..2137 inherit row 2073 (Atsu hand/wrist, contains leaf bones 2097 & 2098)
		for (uint32_t r = 2074; r <= 2137; ++r) {
			copies.push_back(vk::BufferCopy {
			    .srcOffset = b_info.offset + 2073u * 48u,
			    .dstOffset = b_info.offset + r * 48u,
			    .size = 48u
			});
		}

		vk_buffer.copyBuffer(b_info.buffer, b_info.buffer, static_cast<uint32_t>(copies.size()), copies.data());

		// Post-copy barrier: ensure transfer writes are visible to vertex/compute shader reads
		vk::BufferMemoryBarrier post_barrier {};
		post_barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		post_barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eMemoryRead;
		post_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		post_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		post_barrier.buffer = b_info.buffer;
		post_barrier.offset = b_info.offset;
		post_barrier.size = 124224;
		vk_buffer.pipelineBarrier(
		    vk::PipelineStageFlagBits::eTransfer,
		    vk::PipelineStageFlagBits::eVertexShader | vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eAllCommands,
		    {}, 0, nullptr, 1, &post_barrier, 0, nullptr);

		static uint32_t s_log_count = 0;
		if (s_log_count++ < 20) {
			std::printf("\x1b[92m[PALETTE-SANITIZE] Restored 192 bone matrix rows (including leaf/poster bones 2097 & 2098)\x1b[0m\n");
			std::fflush(stdout);
		}
	}
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PALETTESANITIZER_H_
