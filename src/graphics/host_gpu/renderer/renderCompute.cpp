#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/gpuZones.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"
#include "kernel/eventQueue.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/errno.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {
void DebugRegisterDumpAddress(uint64_t address);

// A BVH traversal reads its nodes, instance records and bottom-level BVHs by address. A page
// missing from the BDA page table reads zero until the fault is processed, a frame later; Astro
// Bot relights each probe once, so the rays of those first dispatches all missed and the probes
// stayed black. The SDK's BVHs start with "PSR_BVHL" and give their size at +16: this finds the
// ones a compute shader's SRT points to and has the buffer cache hold them (and the BVHs their
// instance records name) before the dispatch.
static void PrefetchBvhs(BufferCache& buffers,
                         const ShaderRecompiler::IR::ResourceSnapshot& resources) {
	constexpr uint32_t MagicLow    = 0x5f525350u; // "PSR_"
	constexpr uint32_t MagicHigh   = 0x4c485642u; // "BVHL"
	constexpr uint64_t MaxBvhBytes = 1ull << 30u;
	if (resources.user_data.size() < 2u) {
		return;
	}
	const auto read = [](uint64_t address, uint32_t* words, uint32_t count) {
		return address >= 0x100000u && (address & 3u) == 0u &&
		       LibKernel::Memory::TryReadBacking(address, words, count * sizeof(uint32_t));
	};
	// Returns the BVH's size when `header` starts one and all of it is mapped.
	const auto bvh_size = [&](uint64_t header, std::array<uint32_t, 24>& head) -> uint64_t {
		if (!read(header, head.data(), 24) || head[0] != MagicLow || head[1] != MagicHigh) {
			return 0;
		}
		const uint64_t size = head[4] | (uint64_t {head[5]} << 32u);
		uint32_t       last = 0;
		if (size < 256u || size > MaxBvhBytes || !read((header + size - 4u) & ~uint64_t {3}, &last, 1)) {
			return 0;
		}
		return size;
	};
	const uint64_t srt = resources.user_data[0] | (uint64_t {resources.user_data[1]} << 32u);
	std::array<uint32_t, 24> table {};
	if (!read(srt, table.data(), 24)) {
		return;
	}
	static std::unordered_map<uint64_t, uint64_t> held; // Bottom-level header -> size.
	for (uint32_t word = 0; word + 1u < table.size(); word += 2u) {
		const uint64_t header = table[word] | (uint64_t {table[word + 1u]} << 32u);
		std::array<uint32_t, 24> head {};
		const auto size = bvh_size(header, head);
		if (size == 0) {
			continue;
		}
		(void)buffers.FindBuffer(header, size);
		// Instance records (128 bytes, the bottom-level pointer / 8 in dwords 12 and 13) follow
		// the nodes.
		const uint64_t nodes     = (uint64_t {head[22]} + 1u) << 6u;
		const uint64_t instances = head[12] | (uint64_t {head[13]} << 32u);
		if (instances < 0x100u || instances >= size || nodes > size) {
			continue;
		}
		const auto count = std::min<uint64_t>((size - instances) >> 7u, 1u << 16u);
		for (uint64_t index = 0; index < count; index++) {
			std::array<uint32_t, 2> pointer {};
			if (!read(header + instances + (index << 7u) + 48u, pointer.data(), 2)) {
				break;
			}
			const uint64_t bottom =
			    (pointer[0] | (uint64_t {pointer[1] & 0x003fffffu} << 32u)) << 3u;
			if (bottom == 0 || held.contains(bottom)) {
				continue;
			}
			std::array<uint32_t, 24> bottom_head {};
			const auto bottom_size = bvh_size(bottom, bottom_head);
			held.emplace(bottom, bottom_size);
			if (bottom_size != 0) {
				(void)buffers.FindBuffer(bottom, bottom_size);
			}
		}
	}
}
// KYTY_DEBUG_BVH_CHECK=<hash>: walks, on the CPU, the BVH that compute shader traverses (its
// header pointer is at SRT+40) and reports its shape: a cycle or garbage there makes the
// shader's traversal loop endless.
static void DebugCheckBvh(const ShaderRecompiler::IR::CompiledShaderInfo& program,
                          const ShaderRecompiler::IR::ResourceSnapshot& resources,
                          const uint32_t* groups) {
	static const uint64_t hash = [] {
		const char* text = std::getenv("KYTY_DEBUG_BVH_CHECK");
		return text != nullptr ? std::strtoull(text, nullptr, 16) : 0ull;
	}();
	if (hash == 0 || program.shader_hash != hash) {
		return;
	}
	static std::atomic<uint32_t> calls {0};
	const auto call = calls.fetch_add(1, std::memory_order_relaxed);
	if (call >= 6u && call % 200u != 0u) {
		return;
	}
	const auto read = [](uint64_t address, uint32_t* words, uint32_t count) {
		return LibKernel::Memory::TryReadBacking(address, words, count * sizeof(uint32_t));
	};
	std::printf("bvh-check: call=%u groups=%ux%ux%u user_data_base=%u user_data=%zu\n", call,
	            groups[0], groups[1], groups[2], program.user_data_base,
	            resources.user_data.size());
	for (size_t i = 0; i < resources.images.size(); i++) {
		const auto&    image   = resources.images[i];
		const uint64_t address = ((uint64_t {image.dwords[1] & 0xffu} << 32u) | image.dwords[0]) << 8u;
		DebugRegisterDumpAddress(address);
		if (call < 2u) {
			std::printf("bvh-check: image %zu address=0x%" PRIx64 " : %08x %08x %08x %08x %08x %08x %08x %08x\n",
			            i, address, image.dwords[0], image.dwords[1], image.dwords[2],
			            image.dwords[3], image.dwords[4], image.dwords[5], image.dwords[6],
			            image.dwords[7]);
		}
	}
	if (resources.user_data.size() < 2u) {
		std::fflush(stdout);
		return;
	}
	const uint64_t srt = resources.user_data[0] | (uint64_t {resources.user_data[1]} << 32u);
	std::array<uint32_t, 24> table {};
	if (!read(srt, table.data(), 24)) {
		std::printf("bvh-check: srt=0x%016" PRIx64 " unreadable\n", srt);
		std::fflush(stdout);
		return;
	}
	std::printf("bvh-check: srt=0x%016" PRIx64 " :", srt);
	for (const auto word: table) {
		std::printf(" %08x", word);
	}
	const uint64_t header = table[10] | (uint64_t {table[11]} << 32u);
	std::array<uint32_t, 32> head {};
	if (!read(header, head.data(), 32)) {
		std::printf("\nbvh-check: header=0x%016" PRIx64 " unreadable\n", header);
		std::fflush(stdout);
		return;
	}
	std::printf("\nbvh-check: header=0x%016" PRIx64 " :", header);
	for (const auto word: head) {
		std::printf(" %08x", word);
	}
	const uint64_t base      = header & ~uint64_t {0xff};
	const uint64_t instances = header + (head[12] | (uint64_t {head[13]} << 32u));
	const uint64_t last      = head[22] | (uint64_t {head[23] & 0x3ffu} << 32u);
	std::printf("\nbvh-check: instances=0x%016" PRIx64 " last_index=%" PRIu64 "\n", instances, last);
	std::fflush(stdout);

	std::vector<uint32_t>        stack {37u};
	std::unordered_map<uint32_t, uint32_t> seen;
	uint32_t boxes = 0, leaves = 0, revisits = 0, out_of_range = 0, unreadable = 0, zero = 0;
	uint32_t printed = 0, steps = 0, odd_kind = 0, dead_instances = 0;
	while (!stack.empty() && steps < 40000u) {
		const auto id = stack.back();
		stack.pop_back();
		steps++;
		if (seen[id]++ != 0u) {
			revisits++;
			continue;
		}
		const auto kind  = id & 7u;
		const auto index = id >> 3u;
		if (kind == 4u || kind == 5u) {
			std::array<uint32_t, 16> node {};
			if (index + (kind == 5u ? 1u : 0u) > last) {
				out_of_range++;
				continue;
			}
			if (!read(base + (uint64_t {index} << 6u), node.data(), 16)) {
				unreadable++;
				continue;
			}
			boxes++;
			if (std::ranges::all_of(node, [](uint32_t word) { return word == 0u; })) {
				zero++;
			}
			if (printed < 6u) {
				printed++;
				std::printf("bvh-check: box id=0x%x children=%08x %08x %08x %08x w4=%08x w5=%08x\n",
				            id, node[0], node[1], node[2], node[3], node[4], node[5]);
				std::array<float, 32> wide {};
				if (read(base + (uint64_t {index} << 6u), reinterpret_cast<uint32_t*>(wide.data()),
				         32)) {
					std::printf("bvh-check: box floats:");
					for (uint32_t w = 4; w < 32; w++) {
						std::printf(" %g", static_cast<double>(wide[w]));
					}
					std::printf("\n");
				}
			}
			for (uint32_t child = 0; child < 4u; child++) {
				if (node[child] != ~0u) {
					stack.push_back(node[child]);
				}
			}
		} else if (kind == 6u) {
			odd_kind++;
		} else {
			std::array<uint32_t, 16> record {};
			const auto address = kind == 7u ? header + 0x80u : instances + (uint64_t {index} << 7u);
			if (!read(address, record.data(), 16)) {
				unreadable++;
				continue;
			}
			leaves++;
			if (record[15] <= 0x00ffffffu) {
				dead_instances++;
			}
			if (leaves <= 4u) {
				std::printf("bvh-check: leaf id=0x%x record=0x%016" PRIx64 " :", id, address);
				for (const auto word: record) {
					std::printf(" %08x", word);
				}
				std::printf("\n");
			}
		}
	}
	std::printf("bvh-check: steps=%u boxes=%u leaves=%u dead_instances=%u revisits=%u "
	            "out_of_range=%u unreadable=%u zero_boxes=%u kind6=%u left=%zu\n",
	            steps, boxes, leaves, dead_instances, revisits, out_of_range, unreadable, zero,
	            odd_kind, stack.size());
	std::fflush(stdout);
}

static bool FillSourcesDisjoint(std::span<const ShaderRecompiler::IR::DescriptorValue> sources,
                                 GuestRange destination, uint32_t output_buffer = UINT32_MAX) {
	for (uint32_t i = 0; i < sources.size(); ++i) {
		if (i == output_buffer) continue;
		const auto source = DecodeNativeDescriptor<ShaderBufferResource>(sources[i]);
		const auto bytes  = source.GetSize();
		if (source.Base48() < destination.End() && destination.address < source.Base48() + bytes)
			return false;
	}
	return true;
}

bool RenderExecutor::TryConsumeComputeMetaClear(const ShaderComputeInputInfo& input,
                                                const CommandBuffer&          buffer) {
	const auto& program   = *input.stage.program;
	const auto& resources = *input.stage.resources;
	if (resources.buffers.size() != program.info.buffers.size()) {
		EXIT("compute runtime buffer count does not match shader metadata\n");
	}
	auto& cache = buffer.GetContext().GetTextureCache();
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		const auto& resource   = program.info.buffers[i];
		const auto  descriptor = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
		// A metadata resource that is also read is not proven to be a full overwrite. Execute it
		// conservatively instead of replacing the dispatch with a coarse full-surface clear.
		if ((!resource.written || resource.read) && cache.IsMeta(descriptor.Base48())) {
			return false;
		}
	}

	if (!program.info.has_bitwise_xor) {
		for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
			const auto& resource = program.info.buffers[i];
			if (resource.written) {
				const auto descriptor =
				    DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
				if (cache.ClearMeta(descriptor.Base48())) {
					return true;
				}
			}
		}
	}
	return false;
}

bool ResolveComputeBufferFill(const ShaderComputeInputInfo& input, uint32_t group_x,
                              uint32_t group_y, uint32_t group_z, uint32_t mode,
                              ShaderBufferResource& resolved_descriptor, uint32_t& resolved_clear,
                              uint64_t& resolved_size) {
	const auto& resources = *input.stage.resources;
	const auto& fill      = resources.uniform_fill;
	if (fill.kind != ShaderRecompiler::IR::UniformFillKind::Buffer) {
		return false;
	}
	const auto element_size = fill.words * sizeof(uint32_t);
	const auto descriptor =
	    DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[fill.resource]);
	constexpr std::array formats {
	    Prospero::BufferFormat::k32UInt, Prospero::BufferFormat::k32_32UInt,
	    Prospero::BufferFormat::k32_32_32UInt, Prospero::BufferFormat::k32_32_32_32UInt};
	if (descriptor.Stride() != element_size || descriptor.Format() != formats[fill.words - 1] ||
	    descriptor.SwizzleEnabled() || descriptor.IndexStride() != 0 || descriptor.AddTid() ||
	    descriptor.Base48() == 0) {
		return false;
	}
	if (input.threads_num[0] == 0 || input.threads_num[0] != fill.group_stride[0] ||
	    input.threads_num[1] != 1 || input.threads_num[2] != 1 || group_x == 0 || group_y != 1 ||
	    group_z != 1 || mode != (input.dispatch_thread_dimensions ? 0x61u : 0x41u)) {
		return false;
	}
	const uint64_t invocations = input.dispatch_thread_dimensions
	                                 ? group_x
	                                 : static_cast<uint64_t>(group_x) * input.threads_num[0];
	const auto     size        = descriptor.GetSize();
	if (invocations != descriptor.NumRecords() || size == 0 || size > UINT32_MAX ||
	    (input.dispatch_thread_dimensions &&
	     (group_x % input.threads_num[0] != 0 || input.dispatch_threads_num[0] != group_x ||
	      input.dispatch_threads_num[1] != 1 || input.dispatch_threads_num[2] != 1))) {
		return false;
	}
	if (!FillSourcesDisjoint(resources.buffers, {descriptor.Base48(), size}, fill.resource))
		return false;
	resolved_descriptor = descriptor;
	resolved_clear      = fill.value;
	resolved_size       = size;
	return true;
}

bool RenderExecutor::TryConsumeComputeImageClear(const ShaderComputeInputInfo& input,
                                                CommandBuffer& command, uint32_t group_x,
                                                uint32_t group_y, uint32_t group_z, uint32_t mode) {
	const auto& program   = *input.stage.program;
	const auto& resources = *input.stage.resources;
	const auto& fill      = resources.uniform_fill;
	auto&       cache     = command.GetContext().GetTextureCache();
	if (fill.kind == ShaderRecompiler::IR::UniformFillKind::Image) {
		if (mode != 0x41u || input.dispatch_thread_dimensions || fill.value > 255 ||
		    input.threads_num[2] != 1)
			return false;
		const auto  descriptor = DecodeNativeDescriptor<ShaderTextureResource>(resources.images[0]);
		const auto& resource   = program.info.images[0];
		if (descriptor.IsNull() || descriptor.Format() != Prospero::BufferFormat::k8UInt ||
		    descriptor.Type() != Prospero::ImageType::kColor2DArray || descriptor.MetaCompress() ||
		    descriptor.WriteCompress() || descriptor.BaseLevel() > descriptor.LastLevel() ||
		    descriptor.BaseLevel() > descriptor.MaxMip() ||
		    descriptor.BaseArray5() > descriptor.Depth() || descriptor.DstSelX() != 4)
			return false;
		const std::array extents {
		    std::max(1u, (descriptor.Width5() + 1u) >> descriptor.BaseLevel()),
		    std::max(1u, (descriptor.Height5() + 1u) >> descriptor.BaseLevel()),
		    descriptor.Depth() - descriptor.BaseArray5() + 1u};
		const std::array groups {group_x, group_y, group_z};
		for (uint32_t axis = 0; axis < 3; ++axis) {
			const uint64_t threads = input.threads_num[axis];
			// Guest image writes outside the descriptor dimensions are discarded. Only the
			// final workgroup may extend beyond the selected image view.
			if (threads == 0 || threads != fill.group_stride[axis] ||
			    groups[axis] != (extents[axis] + threads - 1) / threads ||
			    groups[axis] * threads > UINT32_MAX) return false;
		}
		const auto  binding     = ResolveTexture(resource, resources.images[0]);
		const auto& destination = binding.desc.info.data;
		if (!FillSourcesDisjoint(resources.buffers, destination)) return false;
		std::scoped_lock lock {cache.m_lock};
		const auto&      image = cache.GetImage(binding.image_id);
		const auto&      view  = binding.desc.view_info;
		if (image.backing.format != vk::Format::eD32SfloatS8Uint || image.info.samples != 1 ||
		    image.info.stencil != destination || view.base_level >= image.backing.mip_levels ||
		    view.base_layer >= image.backing.layers || view.layer_count != extents[2] ||
		    view.layer_count > image.backing.layers - view.base_layer ||
		    std::max(1u, image.info.extent.width >> view.base_level) != extents[0] ||
		    std::max(1u, image.info.extent.height >> view.base_level) != extents[1]) return false;
		const vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eStencil, view.base_level,
		                                       1, view.base_layer, view.layer_count};
		vk::ClearValue clear {};
		clear.depthStencil = vk::ClearDepthStencilValue {0.0f, fill.value};
		cache.ClearImage(command, binding.image_id, image.backing.format, range, clear);
		return true;
	}
	ShaderBufferResource descriptor;
	uint32_t             packed_clear = 0;
	uint64_t             size         = 0;
	if (!ResolveComputeBufferFill(input, group_x, group_y, group_z, mode, descriptor, packed_clear,
	                              size)) {
		return false;
	}
	if (!cache.ClearImageFromBuffer(command, descriptor.Base48(), size, packed_clear)) {
		return false;
	}
	static std::atomic<uint32_t> logged_clears {0};
	if (logged_clears.fetch_add(1, std::memory_order_relaxed) < 32) {
		LOGF("GraphicsRenderDispatchDirect: compute image clear shader=0x%016" PRIx64
		     " addr=0x%016" PRIx64 " size=0x%016" PRIx64 " value=0x%08" PRIx32 "\n",
		     input.stage.program->shader_hash, descriptor.Base48(), size, packed_clear);
	}
	return true;
}

void RenderExecutor::DispatchDirect(uint64_t submit_id, CommandBuffer& buffer,
                                    uint32_t thread_group_x, uint32_t thread_group_y,
                                    uint32_t thread_group_z, uint32_t mode) {
	EXIT_IF(buffer.IsInvalid());
	m_context.GetCommandScheduler().PopPendingOperations(false);
	auto& ctx    = buffer.GetRegisters();
	auto& sh_ctx = buffer.GetShaders();

	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DispatchDirect), submit_id,
	                    thread_group_x, thread_group_y, thread_group_z, mode,
	                    sh_ctx.GetCs().cs_regs.data_addr);

	if (thread_group_x == 0 || thread_group_y == 0 || thread_group_z == 0) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("GraphicsRenderDispatchDirect: skipping zero-sized dispatch groups=%ux%ux%u "
			     "mode=0x%08" PRIx32 " shader=0x%016" PRIx64 "\n",
			     thread_group_x, thread_group_y, thread_group_z, mode,
			     sh_ctx.GetCs().cs_regs.data_addr);
		}
		return;
	}

	Common::LockGuard lock(m_context.GetMutex());
	if (sh_ctx.GetCs().cs_regs.data_addr == 0) {
		LOGF("GraphicsRenderDispatchDirect: temporary: ignoring dispatch with null CS shader, "
		     "groups=%ux%ux%u mode=%u\n",
		     thread_group_x, thread_group_y, thread_group_z, mode);
		return;
	}

	if (sh_ctx.GetCs().cs_regs.data_addr == 0) {
		return;
	}

	constexpr uint32_t DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS = 1u << 5u;
	constexpr uint32_t DISPATCH_INITIATOR_BASE_BITS             = 0x41u;
	constexpr uint32_t DISPATCH_INITIATOR_MODIFIER_BITS         = 0xa038u;
	constexpr uint32_t DISPATCH_INITIATOR_KNOWN_MASK =
	    DISPATCH_INITIATOR_BASE_BITS | DISPATCH_INITIATOR_MODIFIER_BITS;

	const uint32_t unknown_mode_bits = mode & ~DISPATCH_INITIATOR_KNOWN_MASK;
	if (unknown_mode_bits != 0) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("GraphicsRenderDispatchDirect: unknown dispatch initiator bits "
			     "mode=0x%08" PRIx32 " unknown=0x%08" PRIx32 " shader=0x%016" PRIx64
			     " groups=%ux%ux%u\n",
			     mode, unknown_mode_bits, sh_ctx.GetCs().cs_regs.data_addr, thread_group_x,
			     thread_group_y, thread_group_z);
		}
	}

	const auto& cs_regs = sh_ctx.GetCs();
	const auto& sh_regs = ctx.GetShaderRegisters();

	ShaderComputeInputInfo input_info {};
	const bool use_thread_dimensions = (mode & DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0;
	input_info.dispatch_thread_dimensions = use_thread_dimensions;
	const auto compute_program = [&] {
		DrainStats::SlowLookupTimer compile_timer(DrainStats::Kind::ShaderCompile);
		return m_context.GetPipelineCache().GetComputeProgram(cs_regs, sh_regs, input_info);
	}();
	if (!compute_program) {
		// Temporary until RT is implemented.
		return;
	}
	if (DebugSkipShader(input_info.stage.program->shader_hash, DebugShaderKind::Compute))
	    [[unlikely]] {
		return;
	}
	if (use_thread_dimensions) {
		input_info.dispatch_threads_num[0]    = thread_group_x;
		input_info.dispatch_threads_num[1]    = thread_group_y;
		input_info.dispatch_threads_num[2]    = thread_group_z;
	}

	const auto& program   = *input_info.stage.program;
	const auto& resources = *input_info.stage.resources;
	{
		const std::array groups {thread_group_x, thread_group_y, thread_group_z};
		DebugCheckBvh(program, resources, groups.data());
		if (StampTraceEnabled()) [[unlikely]] {
			StampTraceDispatch(program.shader_hash);
		}
	}
	if (resources.specialization_reads.empty() &&
	    (TryConsumeComputeMetaClear(input_info, buffer) ||
	     TryConsumeComputeImageClear(input_info, buffer, thread_group_x, thread_group_y,
	                                 thread_group_z, mode))) {
		ResetBindings();
		return;
	}

	const bool large_workgroup =
	    (input_info.threads_num[0] * input_info.threads_num[1] * input_info.threads_num[2] >= 512);
	const bool                   has_sampler = !program.info.samplers.empty();
	static std::atomic<uint32_t> dispatch_log_count {0};
	if ((large_workgroup || has_sampler) &&
	    dispatch_log_count.fetch_add(1, std::memory_order_relaxed) < 512) {
		const auto sampled_images = std::count_if(
		    program.info.images.begin(), program.info.images.end(), [](const auto& image) {
			    return image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled;
		    });
		const uint32_t frame_num = static_cast<uint32_t>(m_context.GetGpu().GetFrameNum());
		LOGF("GraphicsRenderDispatchDirect: frame=%u shader=0x%016" PRIx64
		     " groups=%ux%ux%u mode=0x%08" PRIx32 " local=%ux%ux%u "
		     "buffers=%zu textures=%zu sampled=%zu storage=%zu samplers=%zu push=%u\n",
		     frame_num, sh_ctx.GetCs().cs_regs.data_addr, thread_group_x, thread_group_y,
		     thread_group_z, mode, input_info.threads_num[0], input_info.threads_num[1],
		     input_info.threads_num[2], program.info.buffers.size(), program.info.images.size(),
		     sampled_images, program.info.images.size() - sampled_images,
		     program.info.samplers.size(),
		     program.bindings.UsesPushData()
		         ? static_cast<uint32_t>(sizeof(ShaderRecompiler::IR::PushData))
		         : 0u);
		for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
			const auto& buffer = program.info.buffers[i];
			const auto  r      = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
			LOGF("  CS buffer[%u]: source=%u usage=%s addr=0x%012" PRIx64
			     " stride=%u records=%u format=%u\n",
			     i, buffer.source, buffer.written ? "read-write" : "read-only", r.Base48(),
			     r.Stride(), r.NumRecords(), r.RawFormat());
		}
		for (uint32_t i = 0; i < program.info.images.size(); i++) {
			const auto& image = program.info.images[i];
			const auto  r     = DecodeNativeDescriptor<ShaderTextureResource>(resources.images[i]);
			LOGF("  CS texture[%u]: source=%u usage=%s sampled=%s addr=0x%010" PRIx64
			     " type=%u fmt=%u extent=%ux%u depth=%u levels=%u tile=%u\n",
			     i, image.source, image.written ? "read-write" : "read-only",
			     image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled
			         ? "true"
			         : "false",
			     r.Base40(), static_cast<uint32_t>(r.Type()), static_cast<uint32_t>(r.Format()),
			     static_cast<uint32_t>(r.Width5()) + 1u, static_cast<uint32_t>(r.Height5()) + 1u,
			     static_cast<uint32_t>(r.Depth()) + 1u,
			     r.Type() == Prospero::ImageType::kColor2DMsaa ||
			             r.Type() == Prospero::ImageType::kColor2DMsaaArray
			         ? 1u
			         : static_cast<uint32_t>(image.r128 ? r.LastLevel() : r.MaxMip()) + 1u,
			     static_cast<uint32_t>(r.TileMode()));
		}
		for (uint32_t i = 0; i < program.info.samplers.size(); i++) {
			const auto r = DecodeNativeDescriptor<ShaderSamplerResource>(resources.samplers[i]);
			LOGF("  CS sampler[%u]: source=%u clamp=%u/%u/%u filter=%u/%u/%u mip=%u "
			     "lod=%u-%u bias=%d\n",
			     i, program.info.samplers[i].source, static_cast<uint32_t>(r.ClampX()),
			     static_cast<uint32_t>(r.ClampY()), static_cast<uint32_t>(r.ClampZ()),
			     static_cast<uint32_t>(r.XyMagFilter()), static_cast<uint32_t>(r.XyMinFilter()),
			     static_cast<uint32_t>(r.ZFilter()), static_cast<uint32_t>(r.MipFilter()),
			     static_cast<uint32_t>(r.MinLod()), static_cast<uint32_t>(r.MaxLod()),
			     static_cast<int32_t>(r.LodBias()));
		}
	}

	if (use_thread_dimensions) {
		auto groups_from_threads = [](uint32_t threads, uint32_t group_size) {
			return (threads == 0
			            ? 0u
			            : (threads + std::max(group_size, 1u) - 1u) / std::max(group_size, 1u));
		};

		const uint32_t old_x = thread_group_x;
		const uint32_t old_y = thread_group_y;
		const uint32_t old_z = thread_group_z;
		thread_group_x       = groups_from_threads(thread_group_x, cs_regs.cs_regs.num_thread_x);
		thread_group_y       = groups_from_threads(thread_group_y, cs_regs.cs_regs.num_thread_y);
		thread_group_z       = groups_from_threads(thread_group_z, cs_regs.cs_regs.num_thread_z);

		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("GraphicsRenderDispatchDirect: use-thread-dimensions %ux%ux%u / %ux%ux%u -> "
			     "groups %ux%ux%u\n",
			     old_x, old_y, old_z, std::max(cs_regs.cs_regs.num_thread_x, 1u),
			     std::max(cs_regs.cs_regs.num_thread_y, 1u),
			     std::max(cs_regs.cs_regs.num_thread_z, 1u), thread_group_x, thread_group_y,
			     thread_group_z);
		}
	}

	buffer.EndRendering();
	auto& pipeline = [&]() -> PipelineCache::Pipeline& {
		DrainStats::SlowLookupTimer create_timer(DrainStats::Kind::PipelineCreate);
		return m_context.GetPipelineCache().GetComputePipeline(input_info, compute_program);
	}();
	auto& bindings = m_compute_bindings;
	PrepareBindings(input_info.stage, bindings);
	FindBuffers(bindings);
	if (program.info.uses_dma) {
		PrefetchBvhs(m_context.GetBufferCache(), *input_info.stage.resources);
		m_context.PrepareBda();
	}
	if (std::getenv("KYTY_DEBUG_BVH_CHECK") != nullptr && program.shader_hash == 0x380bb9d636390baeull) {
		static uint32_t described = 0;
		const auto&     user      = resources.user_data;
		std::array<uint32_t, 12> table {};
		if (described++ % 25u == 0u && user.size() >= 2u &&
		    LibKernel::Memory::TryReadBacking(user[0] | (uint64_t {user[1]} << 32u), table.data(),
		                                      sizeof(table))) {
			m_context.GetBufferCache().DebugDescribePage((table[10] | (uint64_t {table[11]} << 32u)) +
			                                             0x100u);
		}
	}
	RebindImages(bindings);
	if (ImageUsersEnabled()) [[unlikely]] {
		PreparedBindings* const compute_stages[] {&bindings};
		NoteImageUsers(m_context.GetTextureCache(), compute_stages, {}, nullptr, 0,
		               input_info.stage.program->shader_hash);
	}
	RebindBuffers(bindings);

	auto              vk_buffer        = buffer.Handle();
	PreparedBindings* descriptor_stage = &bindings;
	CommitBindings(buffer, vk::PipelineBindPoint::eCompute, pipeline,
	               std::span {&descriptor_stage, 1u});
	bool has_storage_writes = HasShaderBufferWrites(input_info.stage);
	has_storage_writes =
	    std::any_of(program.info.images.begin(), program.info.images.end(),
	                [](const auto& image) {
		                return image.written &&
		                       image.resource_class ==
		                           ShaderRecompiler::IR::ImageResourceClass::Storage;
	                }) ||
	    has_storage_writes;
	if (has_storage_writes) {
		// A host fence used to serialize every dispatch. Preserve its read-before-write ordering
		// while allowing the queue to execute asynchronously.
		ShaderWriteHazardBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
	}
	if (DebugFullBarriers()) [[unlikely]] {
		m_context.GetCommandScheduler().EndRendering();
		RecordFullBarrier(vk_buffer);
	}
	GpuZones::Mark(vk_buffer, DrainStats::Zone::GameDispatch, input_info.stage.program->shader_hash);
	vk_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.pipeline);
	vk_buffer.dispatch(thread_group_x, thread_group_y, thread_group_z);

	// The removed host fence also ordered read-only dispatches before later writers.
	ShaderAccessBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
	m_context.GetBufferCache().OnCommandRecorded();
	ResetBindings();
}

void RenderExecutor::DispatchIndirect(uint64_t submit_id, CommandBuffer& buffer,
                                      uint64_t args_addr, uint32_t mode) {
	EXIT_IF(buffer.IsInvalid() || args_addr == 0 || (args_addr & 3u) != 0 ||
	        (mode & Pm4::COMPUTE_DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0);
	m_context.GetCommandScheduler().PopPendingOperations(false);
	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DispatchIndirect), submit_id,
	                    static_cast<uint32_t>(args_addr), static_cast<uint32_t>(args_addr >> 32u),
	                    0, mode, buffer.GetShaders().GetCs().cs_regs.data_addr);
	Common::LockGuard lock(m_context.GetMutex());
	const auto& cs_regs = buffer.GetShaders().GetCs();
	if (cs_regs.cs_regs.data_addr == 0) {
		return;
	}
	ShaderComputeInputInfo input_info {};
	const auto compute_program = [&] {
		DrainStats::SlowLookupTimer compile_timer(DrainStats::Kind::ShaderCompile);
		return m_context.GetPipelineCache().GetComputeProgram(
		    cs_regs, buffer.GetRegisters().GetShaderRegisters(), input_info);
	}();
	if (!compute_program) {
		// Temporary until RT is implemented.
		return;
	}
	if (DebugSkipShader(input_info.stage.program->shader_hash, DebugShaderKind::Compute))
	    [[unlikely]] {
		return;
	}
	buffer.EndRendering();
	auto& pipeline = [&]() -> PipelineCache::Pipeline& {
		DrainStats::SlowLookupTimer create_timer(DrainStats::Kind::PipelineCreate);
		return m_context.GetPipelineCache().GetComputePipeline(input_info, compute_program);
	}();
	auto& bindings = m_compute_bindings;
	PrepareBindings(input_info.stage, bindings);
	FindBuffers(bindings);
	const auto& program = *input_info.stage.program;
	{
		std::array<uint32_t, 3> groups {};
		LibKernel::Memory::TryReadBacking(args_addr, groups.data(), sizeof(groups));
		DebugCheckBvh(program, *input_info.stage.resources, groups.data());
		if (StampTraceEnabled()) [[unlikely]] {
			StampTraceDispatch(program.shader_hash);
		}
	}
	if (program.info.uses_dma) {
		PrefetchBvhs(m_context.GetBufferCache(), *input_info.stage.resources);
		m_context.PrepareBda();
	}
	if (std::getenv("KYTY_DEBUG_BVH_CHECK") != nullptr && program.shader_hash == 0x380bb9d636390baeull) {
		static uint32_t described = 0;
		const auto&     user      = input_info.stage.resources->user_data;
		std::array<uint32_t, 12> table {};
		if (described++ % 25u == 0u && user.size() >= 2u &&
		    LibKernel::Memory::TryReadBacking(user[0] | (uint64_t {user[1]} << 32u), table.data(),
		                                      sizeof(table))) {
			m_context.GetBufferCache().DebugDescribePage((table[10] | (uint64_t {table[11]} << 32u)) +
			                                             0x100u);
		}
	}
	RebindImages(bindings);
	if (ImageUsersEnabled()) [[unlikely]] {
		PreparedBindings* const compute_stages[] {&bindings};
		NoteImageUsers(m_context.GetTextureCache(), compute_stages, {}, nullptr, 0,
		               input_info.stage.program->shader_hash);
	}
	// Acquiring arguments can merge cache buffers; finalize shader bindings afterward.
	const auto [args_buffer, args_offset] = m_context.GetBufferCache().ObtainBuffer(
	    args_addr, sizeof(vk::DispatchIndirectCommand), false);
	EXIT_IF(args_buffer == nullptr || (args_offset & 3u) != 0);
	RebindBuffers(bindings);
	PreparedBindings* descriptor_stage = &bindings;
	CommitBindings(buffer, vk::PipelineBindPoint::eCompute, pipeline,
	               std::span {&descriptor_stage, 1u});
	const auto vk_buffer = buffer.Handle();
	const bool has_storage_writes = HasShaderBufferWrites(input_info.stage) ||
	    std::any_of(program.info.images.begin(), program.info.images.end(), [](const auto& image) {
		    return image.written && image.resource_class ==
		                                ShaderRecompiler::IR::ImageResourceClass::Storage;
	    });
	if (has_storage_writes) {
		ShaderWriteHazardBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
	}
	vk::MemoryBarrier barrier {};
	barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eTransferWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead;
	vk_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eAllGraphics |
	                              vk::PipelineStageFlagBits::eComputeShader |
	                              vk::PipelineStageFlagBits::eTransfer,
	                          vk::PipelineStageFlagBits::eDrawIndirect, {},
	                          1, &barrier, 0, nullptr, 0, nullptr);
	if (DebugFullBarriers()) [[unlikely]] {
		m_context.GetCommandScheduler().EndRendering();
		RecordFullBarrier(vk_buffer);
	}
	GpuZones::Mark(vk_buffer, DrainStats::Zone::GameDispatch, input_info.stage.program->shader_hash);
	vk_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.pipeline);
	vk_buffer.dispatchIndirect(args_buffer->Handle(), args_offset);
	ShaderAccessBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
	m_context.GetBufferCache().OnCommandRecorded();
	ResetBindings();
}

} // namespace Libs::Graphics
