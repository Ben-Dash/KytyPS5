#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PALETTESANITIZER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PALETTESANITIZER_H_

#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"

#include "graphics/host_gpu/renderer/render.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <utility>
#include <vector>

namespace Libs::Graphics {

inline void SanitizeSkinningPaletteBuffer(CommandBuffer& buffer, const PreparedBindings& bindings, uint64_t submit_id = 0) {
	static const bool s_enabled = [] {
		const char* text = KYTY_GETENV("KYTY_GPU_PALETTE_SANITIZE");
		return text == nullptr || std::strcmp(text, "0") != 0;
	}();
	if (!s_enabled) {
		return;
	}

	static uint64_t s_last_submit_id = UINT64_MAX;
	static std::vector<std::pair<VkBuffer, vk::DeviceSize>> s_sanitized_ranges;

	if (submit_id != 0 && submit_id != s_last_submit_id) {
		s_last_submit_id = submit_id;
		s_sanitized_ranges.clear();
	}

	for (size_t b_idx = 0; b_idx < bindings.buffers.size(); ++b_idx) {
		const auto& b_info = bindings.buffers[b_idx];
		if (b_info.buffer == nullptr) {
			continue;
		}

		bool is_palette = false;
		uint64_t palette_size = 0;
		if (b_idx < bindings.buffer_sources.size()) {
			const auto src_size = bindings.buffer_sources[b_idx].size;
			if (src_size == 124224) {
				is_palette = true;
				palette_size = src_size;
			}
		}
		if (!is_palette) {
			if (b_info.range == 124224) {
				is_palette = true;
				palette_size = b_info.range;
			} else if (b_info.range >= 124224 && b_info.range <= 124224 + 256) {
				is_palette = true;
				palette_size = 124224;
			}
		}
		if (!is_palette) {
			continue;
		}

		// Calculate exact base offset accounting for storage alignment adjustments
		const vk::DeviceSize adjustment = (b_info.range > palette_size) ? (b_info.range - palette_size) : 0u;
		const vk::DeviceSize base_offset = b_info.offset + adjustment;
		const vk::DeviceSize barrier_size = (palette_size > 0) ? palette_size : b_info.range;

		const auto range_key = std::make_pair(VkBuffer(b_info.buffer), base_offset);
		if (submit_id != 0) {
			if (std::find(s_sanitized_ranges.begin(), s_sanitized_ranges.end(), range_key) != s_sanitized_ranges.end()) {
				continue;
			}
		}

		// Vulkan dynamic rendering compliance: transfer commands (vkCmdCopyBuffer and transfer barriers)
		// MUST be recorded outside of a render pass.
		if (buffer.IsRendering()) {
			buffer.EndRendering();
		}

		auto vk_buffer = buffer.Handle();

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

		// Prop bones in character palette (size 124224, 2588 bones):
		// Slice 0: rows 1050..1113 inherit parent row 1049
		// Slice 1: rows 1562..1625 inherit parent row 1561
		// Slice 2: rows 2074..2137 inherit parent row 2073 (Atsu hand/wrist -> leaf/poster bones 2097 & 2098)
		// Body bones (rows 0..1048) and character meshes are strictly preserved and untouched!
		auto add_slice = [&](uint32_t parent_bone, uint32_t start_bone, uint32_t end_bone) {
			for (uint32_t r = start_bone; r <= end_bone; ++r) {
				copies.push_back(vk::BufferCopy {
				    .srcOffset = base_offset + parent_bone * 48u,
				    .dstOffset = base_offset + r * 48u,
				    .size = 48u
				});
			}
		};
		add_slice(1049u, 1050u, 1113u);
		add_slice(1561u, 1562u, 1625u);
		add_slice(2073u, 2074u, 2137u);

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
		if (s_log_count++ < 30) {
			std::printf("\x1b[92m[PALETTE-SANITIZE] Restored 192 prop bone rows (size=124224, leaf bones 2097 & 2098) at offset 0x%" PRIx64 "\x1b[0m\n",
			            static_cast<uint64_t>(base_offset));
			std::fflush(stdout);
		}
		if (submit_id != 0) {
			s_sanitized_ranges.push_back(range_key);
		}
	}
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PALETTESANITIZER_H_
