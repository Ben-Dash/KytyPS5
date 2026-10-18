#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PALETTESANITIZER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PALETTESANITIZER_H_

#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"

#include <cinttypes>
#include <cstdio>
#include <vector>

namespace Libs::Graphics {

inline void SanitizeSkinningPaletteBuffer(vk::CommandBuffer vk_buffer, const PreparedBindings& bindings) {
	for (size_t b_idx = 0; b_idx < bindings.buffers.size(); ++b_idx) {
		const auto& b_info = bindings.buffers[b_idx];
		if (b_info.buffer == nullptr) {
			continue;
		}

		bool is_palette = false;
		uint64_t palette_size = 0;
		if (b_idx < bindings.buffer_sources.size()) {
			const auto src_size = bindings.buffer_sources[b_idx].size;
			if (src_size == 124224 || src_size == 579024) {
				is_palette = true;
				palette_size = src_size;
			}
		}
		if (!is_palette) {
			if (b_info.range == 124224 || b_info.range == 579024) {
				is_palette = true;
				palette_size = b_info.range;
			} else if (b_info.range >= 124224 && b_info.range <= 124224 + 256) {
				is_palette = true;
				palette_size = 124224;
			} else if (b_info.range >= 579024 && b_info.range <= 579024 + 256) {
				is_palette = true;
				palette_size = 579024;
			}
		}
		if (!is_palette) {
			continue;
		}

		// Calculate exact base offset accounting for storage alignment adjustments
		const vk::DeviceSize adjustment = (b_info.range > palette_size) ? (b_info.range - palette_size) : 0u;
		const vk::DeviceSize base_offset = b_info.offset + adjustment;
		const vk::DeviceSize barrier_size = (palette_size > 0) ? palette_size : b_info.range;

		// Pre-copy barrier: ensure compute/transfer writes are available to transfer reads
		vk::BufferMemoryBarrier pre_barrier {};
		pre_barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eTransferWrite;
		pre_barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite;
		pre_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		pre_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		pre_barrier.buffer = b_info.buffer;
		pre_barrier.offset = base_offset;
		pre_barrier.size = barrier_size;
		vk_buffer.pipelineBarrier(
		    vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eTransfer | vk::PipelineStageFlagBits::eAllCommands,
		    vk::PipelineStageFlagBits::eTransfer,
		    {}, 0, nullptr, 1, &pre_barrier, 0, nullptr);

		std::vector<vk::BufferCopy> copies;
		copies.reserve(192);

		// Slice 0: rows 1050..1113 inherit row 1049
		for (uint32_t r = 1050; r <= 1113; ++r) {
			copies.push_back(vk::BufferCopy {
			    .srcOffset = base_offset + 1049u * 48u,
			    .dstOffset = base_offset + r * 48u,
			    .size = 48u
			});
		}

		// Slice 1: rows 1562..1625 inherit row 1561
		for (uint32_t r = 1562; r <= 1625; ++r) {
			copies.push_back(vk::BufferCopy {
			    .srcOffset = base_offset + 1561u * 48u,
			    .dstOffset = base_offset + r * 48u,
			    .size = 48u
			});
		}

		// Slice 2: rows 2074..2137 inherit row 2073 (Atsu hand/wrist, contains leaf bones 2097 & 2098)
		for (uint32_t r = 2074; r <= 2137; ++r) {
			copies.push_back(vk::BufferCopy {
			    .srcOffset = base_offset + 2073u * 48u,
			    .dstOffset = base_offset + r * 48u,
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
		post_barrier.offset = base_offset;
		post_barrier.size = barrier_size;
		vk_buffer.pipelineBarrier(
		    vk::PipelineStageFlagBits::eTransfer,
		    vk::PipelineStageFlagBits::eVertexShader | vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eAllCommands,
		    {}, 0, nullptr, 1, &post_barrier, 0, nullptr);

		static uint32_t s_log_count = 0;
		if (s_log_count++ < 20) {
			std::printf("\x1b[92m[PALETTE-SANITIZE] Restored 192 bone matrix rows (including leaf/poster bones 2097 & 2098, size=%" PRIu64 ")\x1b[0m\n",
			            palette_size);
			std::fflush(stdout);
		}
	}
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PALETTESANITIZER_H_
