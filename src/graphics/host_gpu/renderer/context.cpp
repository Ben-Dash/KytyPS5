#include "common/assert.h"
#include "graphics/host_gpu/renderer/labelCausalTrace.h"
#include "common/common.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/gpuZones.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <utility>
namespace Libs::Graphics {

CommandBuffer::CommandBuffer(CommandScheduler& scheduler)
    : m_context(scheduler.Context()), m_graphics(scheduler.Graphics()) {}

bool CommandBuffer::IsInvalid() const {
	return m_buffer == nullptr;
}

void CommandBuffer::BeginUploadBatch() const {
	EXIT_IF(IsInvalid() || m_upload_batch_depth == UINT32_MAX);
	++m_upload_batch_depth;
}

void CommandBuffer::EndUploadBatch() const {
	EXIT_IF(m_upload_batch_depth == 0);
	if (--m_upload_batch_depth == 0) FlushUploadDependency();
}

void CommandBuffer::FlushUploadDependency() const {
	if (!m_upload_dependency_pending) return;
	EXIT_IF(IsInvalid() || m_rendering);
	// Clear before any later handle request; the actual copies are already recorded.
	m_upload_dependency_pending = false;
	m_upload_destinations.clear();
	NoteSubmissionWork();
	vk::MemoryBarrier after {};
	after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	after.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	m_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                         vk::PipelineStageFlagBits::eAllCommands,
	                         vk::DependencyFlagBits::eByRegion, 1, &after, 0, nullptr, 0, nullptr);
}

bool CommandBuffer::RecordBdaUploadCopy(vk::Buffer source, vk::Buffer destination,
	                                      std::span<const vk::BufferCopy> copies) const {
	if (!UploadBatchActive() || copies.empty()) return false;
	EXIT_IF(IsInvalid() || source == nullptr || destination == nullptr);
	EndRendering();
	// Initially split even disjoint ranges of the same owner. Repeated destinations may
	// carry a real transfer WAW dependency; distinct cache allocations do not alias.
	if (std::find(m_upload_destinations.begin(), m_upload_destinations.end(), destination) !=
	    m_upload_destinations.end()) {
		FlushUploadDependency();
	}
	if (!m_upload_dependency_pending) {
		// This stronger global pre-dependency also covers a pending compute chain.
		const auto native = HandleForFullBarrier();
		GpuZones::Mark(native, DrainStats::Zone::BufferCopy);
		vk::MemoryBarrier before {};
		before.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite |
		                       vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite;
		before.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
		native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
		                       vk::PipelineStageFlagBits::eTransfer,
		                       vk::DependencyFlagBits::eByRegion, 1, &before, 0, nullptr, 0, nullptr);
	}
	// Keep the source reservation, native copy and temporary owner's completion in the
	// same tick. Only the post-copy visibility dependency is deferred to the boundary.
	NoteSubmissionWork();
	m_buffer.copyBuffer(source, destination, static_cast<uint32_t>(copies.size()), copies.data());
	m_upload_destinations.push_back(destination);
	m_upload_dependency_pending = true;
	return true;
}

vk::CommandBuffer CommandBuffer::Handle() const {
	EXIT_IF(IsInvalid());
	FlushUploadDependency();
	NoteSubmissionWork();
	if (m_compute_access_pending) [[unlikely]] {
		LabelCausalTrace::NotePartialChainBoundary(DrainStats::t_pm4_op,false);
		m_compute_access_pending = false;
		ShaderAccessBarrier(m_buffer, vk::PipelineStageFlagBits::eComputeShader);
	}
	return m_buffer;
}

vk::CommandBuffer CommandBuffer::ChainHandle() const {
	EXIT_IF(IsInvalid() || m_rendering);
	FlushUploadDependency();
	NoteSubmissionWork();
	return m_buffer;
}

vk::CommandBuffer CommandBuffer::HandleForFullBarrier() const {
	EXIT_IF(IsInvalid());
	FlushUploadDependency();
	NoteSubmissionWork();
	if(m_compute_access_pending)LabelCausalTrace::NotePartialChainBoundary(DrainStats::t_pm4_op,true);
	m_compute_access_pending = false;
	return m_buffer;
}

void CommandBuffer::Begin() {
	EXIT_IF(m_rendering || m_pending_shader_writes || m_upload_dependency_pending ||
	        !m_upload_destinations.empty() || IsInvalid());
	m_compute_access_pending = false;
	m_native_handle_requested = false;
	// Begin itself has no GPU effect. Public handle access conservatively marks work.
	auto buffer = m_buffer;

	vk::CommandBufferBeginInfo begin_info {};
	begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;

	auto result = buffer.begin(&begin_info);

	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
	// A new command buffer starts without any pipeline or dynamic state.
	InvalidateGraphicsState();
}

void CommandBuffer::End() const {
	EndRendering();
	auto buffer = Handle();

	auto result = buffer.end();

	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
}

void CommandBuffer::SetDebugInfo(uint32_t op, uint64_t submit_id, uint32_t arg0, uint32_t arg1,
                                 uint32_t arg2, uint32_t arg3, uint64_t arg4) {
	m_debug_op        = op;
	m_debug_submit_id = submit_id;
	m_debug_arg0      = arg0;
	m_debug_arg1      = arg1;
	m_debug_arg2      = arg2;
	m_debug_arg3      = arg3;
	m_debug_arg4      = arg4;
}

void CommandBuffer::BeginRendering(const RenderState& state) const {
	FlushUploadDependency();
	if (m_rendering && m_render_state == state) {
		return;
	}
	EXIT_IF(state.width == 0 || state.height == 0 || state.num_layers == 0 ||
	        state.num_color_attachments > RENDER_COLOR_ATTACHMENTS_MAX);
	EndRendering();

	std::array<vk::RenderingAttachmentInfo, RENDER_COLOR_ATTACHMENTS_MAX> colors {};
	for (uint32_t i = 0; i < state.num_color_attachments; i++) {
		const auto& attachment = state.color_attachments[i];
		colors[i].imageView    = attachment.image_view;
		colors[i].imageLayout  = attachment.image_layout;
		colors[i].loadOp =
		    attachment.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
		colors[i].storeOp                 = vk::AttachmentStoreOp::eStore;
		colors[i].clearValue.color.uint32 = attachment.clear_value;
	}

	const auto&                 depth_stencil = state.depth_stencil_attachment;
	vk::RenderingAttachmentInfo depth {};
	depth.imageView   = depth_stencil.image_view;
	depth.imageLayout = depth_stencil.image_layout;
	depth.loadOp =
	    depth_stencil.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
	depth.storeOp                       = vk::AttachmentStoreOp::eStore;
	depth.clearValue.depthStencil.depth = std::bit_cast<float>(depth_stencil.clear_value[0]);

	vk::RenderingAttachmentInfo stencil {};
	stencil.imageView   = depth_stencil.image_view;
	stencil.imageLayout = depth_stencil.image_layout;
	stencil.loadOp =
	    depth_stencil.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
	stencil.storeOp                         = vk::AttachmentStoreOp::eStore;
	stencil.clearValue.depthStencil.stencil = depth_stencil.clear_value[1];

	vk::RenderingInfo rendering {};
	rendering.renderArea.extent    = {state.width, state.height};
	rendering.layerCount           = state.num_layers;
	rendering.colorAttachmentCount = state.num_color_attachments;
	rendering.pColorAttachments    = colors.data();
	rendering.pDepthAttachment     = depth_stencil.has_depth ? &depth : nullptr;
	rendering.pStencilAttachment   = depth_stencil.has_stencil ? &stencil : nullptr;
	const auto recorder = Handle();
	recorder.beginRendering(rendering);
	if (g_render_debug_counters.counting.load(std::memory_order_relaxed)) [[unlikely]] {
		g_render_debug_counters.render_begins.fetch_add(1, std::memory_order_relaxed);
	}
	m_render_state = state;
	m_rendering    = true;
	m_context.GetOcclusionQueries().OnBeginRendering(recorder);
}

void CommandBuffer::EndRendering() const {
	if (m_rendering) {
		const auto recorder = Handle();
		m_context.GetOcclusionQueries().OnEndRendering(recorder);
		recorder.endRendering();
		if (g_render_debug_counters.counting.load(std::memory_order_relaxed)) [[unlikely]] {
			g_render_debug_counters.render_ends.fetch_add(1, std::memory_order_relaxed);
		}
		m_rendering    = false;
		m_render_state = {};
	}
	if (m_pending_shader_writes) {
		ShaderWriteBarrier(Handle(), std::exchange(m_pending_shader_writes, {}));
		if (g_render_debug_counters.counting.load(std::memory_order_relaxed)) [[unlikely]] {
			g_render_debug_counters.image_barriers.fetch_add(1, std::memory_order_relaxed);
		}
	}
}

} // namespace Libs::Graphics
