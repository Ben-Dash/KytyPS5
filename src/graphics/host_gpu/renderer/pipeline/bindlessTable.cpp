#include "graphics/host_gpu/renderer/pipeline/bindlessTable.h"
#include "graphics/host_gpu/renderer/pipeline/bindlessProvenance.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/switches.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/samplerCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/shaderBindings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <array>
#include <atomic>
#include <fmt/format.h>
#include <string>
#include <cstdlib>

namespace Libs::Graphics {

BindlessTable::BindlessTable(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_graphics(graphics), m_scheduler(scheduler) {
	if (!graphics.bindless_enabled) {
		return;
	}
	m_images_per_array =
	    std::min(MaxImagesPerArray, graphics.bindless_max_sampled_images / ImageArrays);
	if (m_images_per_array == 0) {
		LOGF("Bindless table: the device allows no update-after-bind sampled images\n");
		return;
	}

	constexpr auto image_flags = vk::DescriptorBindingFlagBits::ePartiallyBound |
	                             vk::DescriptorBindingFlagBits::eUpdateAfterBind |
	                             vk::DescriptorBindingFlagBits::eUpdateUnusedWhilePending;
	// The buffers are written once, here, so they need no update-after-bind.
	constexpr auto buffer_flags = vk::DescriptorBindingFlags {};
	// A device without update-after-bind samplers still gets a one-entry array, holding the
	// default sampler, so every bindless pipeline sees the same layout.
	m_samplers_per_array = std::min(MaxSamplers, graphics.bindless_max_samplers);
	const std::array<vk::DescriptorSetLayoutBinding, 11> bindings {{
	    {Images2D, vk::DescriptorType::eSampledImage, m_images_per_array, vk::ShaderStageFlagBits::eAll,
	     nullptr},
	    {Images2DArray, vk::DescriptorType::eSampledImage, m_images_per_array,
	     vk::ShaderStageFlagBits::eAll, nullptr},
	    {ImagesCube, vk::DescriptorType::eSampledImage, m_images_per_array,
	     vk::ShaderStageFlagBits::eAll, nullptr},
	    {Images3D, vk::DescriptorType::eSampledImage, m_images_per_array, vk::ShaderStageFlagBits::eAll,
	     nullptr},
	    {Translation, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eAll, nullptr},
	    {Feedback, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eAll, nullptr},
	    {Samplers, vk::DescriptorType::eSampler, std::max(m_samplers_per_array, 1u),
	     vk::ShaderStageFlagBits::eAll, nullptr},
	    {Images2DUint, vk::DescriptorType::eSampledImage, m_images_per_array,
	     vk::ShaderStageFlagBits::eAll, nullptr},
	    {Images2DArrayUint, vk::DescriptorType::eSampledImage, m_images_per_array,
	     vk::ShaderStageFlagBits::eAll, nullptr},
	    {Images2DSint, vk::DescriptorType::eSampledImage, m_images_per_array,
	     vk::ShaderStageFlagBits::eAll, nullptr},
	    {Images2DArraySint, vk::DescriptorType::eSampledImage, m_images_per_array,
	     vk::ShaderStageFlagBits::eAll, nullptr},
	}};
	const std::array<vk::DescriptorBindingFlags, 11> binding_flags {
	    image_flags, image_flags, image_flags, image_flags, buffer_flags, buffer_flags,
	    image_flags, image_flags, image_flags, image_flags, image_flags};
	vk::DescriptorSetLayoutBindingFlagsCreateInfo flags_info {};
	flags_info.bindingCount  = static_cast<uint32_t>(binding_flags.size());
	flags_info.pBindingFlags = binding_flags.data();
	vk::DescriptorSetLayoutCreateInfo layout_info {};
	layout_info.pNext        = &flags_info;
	layout_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool;
	layout_info.bindingCount = static_cast<uint32_t>(bindings.size());
	layout_info.pBindings    = bindings.data();
	RequireVulkanSuccess(graphics.device.createDescriptorSetLayout(&layout_info, nullptr, &m_layout),
	                     "create bindless descriptor layout");

	const std::array<vk::DescriptorPoolSize, 3> sizes {{
	    {vk::DescriptorType::eSampledImage, m_images_per_array * ImageArrays},
	    {vk::DescriptorType::eStorageBuffer, 2},
	    {vk::DescriptorType::eSampler, std::max(m_samplers_per_array, 1u)},
	}};
	vk::DescriptorPoolCreateInfo pool_info {};
	pool_info.flags         = vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind;
	pool_info.maxSets       = 1;
	pool_info.poolSizeCount = static_cast<uint32_t>(sizes.size());
	pool_info.pPoolSizes    = sizes.data();
	RequireVulkanSuccess(graphics.device.createDescriptorPool(&pool_info, nullptr, &m_pool),
	                     "create bindless descriptor pool");

	vk::DescriptorSetAllocateInfo allocate_info {};
	allocate_info.descriptorPool     = m_pool;
	allocate_info.descriptorSetCount = 1;
	allocate_info.pSetLayouts        = &m_layout;
	RequireVulkanSuccess(graphics.device.allocateDescriptorSets(&allocate_info, &m_set),
	                     "allocate bindless descriptor set");

	// Both live in host-visible device memory: the host writes translations and clears feedback
	// flags directly, while shaders access them at device speed. The host reads the flags from
	// a host-cached snapshot (RecordFeedbackSnapshot).
	m_translation = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::Stream, 0,
	                                         AllFlags, TranslationEntries * sizeof(uint32_t));
	m_feedback    = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::Stream, 0,
	                                         AllFlags, TranslationEntries * sizeof(uint32_t));
	m_feedback_snapshot = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::Download, 0,
	                                               AllFlags, TranslationEntries * sizeof(uint32_t));
	EXIT_IF(m_translation->Mapped().empty() || m_feedback->Mapped().empty() ||
	        m_feedback_snapshot->Mapped().empty());
	// A region allocated after a snapshot was recorded reads as "no requests" until the next.
	std::fill(m_feedback_snapshot->Mapped().begin(), m_feedback_snapshot->Mapped().end(),
	          uint8_t {0});
	m_feedback_snapshot->Flush(0, m_feedback_snapshot->Size());
	SetVulkanObjectNameF(graphics.device, m_translation->Handle(), "Bindless Translation");
	SetVulkanObjectNameF(graphics.device, m_feedback->Handle(), "Bindless Feedback");

	const std::array<vk::DescriptorBufferInfo, 2> buffer_infos {{
	    {m_translation->Handle(), 0, VK_WHOLE_SIZE},
	    {m_feedback->Handle(), 0, VK_WHOLE_SIZE},
	}};
	std::array<vk::WriteDescriptorSet, 2> writes {};
	for (uint32_t i = 0; i < writes.size(); i++) {
		writes[i].dstSet          = m_set;
		writes[i].dstBinding      = Translation + i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType  = vk::DescriptorType::eStorageBuffer;
		writes[i].pBufferInfo     = &buffer_infos[i];
	}
	graphics.device.updateDescriptorSets(static_cast<uint32_t>(writes.size()), writes.data(), 0,
	                                     nullptr);
	CreatePlaceholders(scheduler);
	graphics.bindless_layout = m_layout;
	graphics.bindless_set    = m_set;
	LOGF("Bindless table: %u images per array, %u translation entries, %u samplers\n",
	     m_images_per_array, TranslationEntries, m_samplers_per_array);
}

BindlessTable::SamplerHeap* BindlessTable::FindOrCreateSamplerHeap(uint64_t base,
                                                                   uint32_t table_offset,
                                                                   uint32_t flags) {
	for (auto& heap: m_sampler_heaps) {
		if (heap.base == base && heap.table_offset == table_offset && heap.flags == flags) {
			return &heap;
		}
	}
	auto& heap        = m_sampler_heaps.emplace_back();
	heap.base         = base;
	heap.table_offset = table_offset;
	heap.flags        = flags;
	return &heap;
}

void BindlessTable::WriteDefaultSampler(vk::Sampler sampler) {
	if (m_default_sampler_written || m_set == nullptr) {
		return;
	}
	vk::DescriptorImageInfo info {sampler, nullptr, vk::ImageLayout::eUndefined};
	vk::WriteDescriptorSet  write {};
	write.dstSet          = m_set;
	write.dstBinding      = Samplers;
	write.dstArrayElement = 0;
	write.descriptorCount = 1;
	write.descriptorType  = vk::DescriptorType::eSampler;
	write.pImageInfo      = &info;
	m_graphics.device.updateDescriptorSets(1, &write, 0, nullptr);
	m_default_sampler_written = true;
}

bool BindlessTable::MirrorSamplerHeap(SamplerHeap&                              heap,
                                      std::span<const std::array<uint32_t, 4>> records,
                                      SamplerCache&                             cache) {
	const auto same_prefix =
	    records.size() >= heap.records.size() &&
	    std::equal(heap.records.begin(), heap.records.end(), records.begin());
	if (same_prefix && records.size() == heap.records.size()) {
		return true;
	}
	uint32_t first = static_cast<uint32_t>(heap.records.size());
	if (!same_prefix || records.size() > heap.capacity || heap.region == 0) {
		const auto capacity = std::max<uint32_t>(64u, static_cast<uint32_t>(records.size()) * 2u);
		if (m_samplers_per_array <= 1u || capacity > m_samplers_per_array - m_next_sampler_slot) {
			static std::atomic<uint32_t> reported = 0;
			if (reported.fetch_add(1) < 8) {
				LOGF("Bindless samplers: array full (%u of %u slots), heap 0x%016" PRIx64
				     " keeps %zu records\n",
				     m_next_sampler_slot, m_samplers_per_array, heap.base, heap.records.size());
			}
			return false;
		}
		heap.region   = m_next_sampler_slot;
		heap.capacity = capacity;
		m_next_sampler_slot += capacity;
		first = 0;
	}
	std::vector<vk::DescriptorImageInfo> infos;
	infos.reserve(records.size() - first);
	for (size_t key = first; key < records.size(); key++) {
		ShaderSamplerResource descriptor;
		std::copy(records[key].begin(), records[key].end(), descriptor.fields);
		if ((heap.flags & SamplerDepthCompare) == 0u) {
			descriptor.fields[0] &= ~(0x7u << 12u);
		}
		if ((heap.flags & SamplerPointFiltering) != 0u) {
			descriptor.SetPointFiltering();
		}
		infos.push_back({cache.GetSampler(descriptor, (heap.flags & SamplerIntegerBorder) != 0u),
		                 nullptr, vk::ImageLayout::eUndefined});
	}
	if (!infos.empty()) {
		vk::WriteDescriptorSet write {};
		write.dstSet          = m_set;
		write.dstBinding      = Samplers;
		write.dstArrayElement = heap.region + first;
		write.descriptorCount = static_cast<uint32_t>(infos.size());
		write.descriptorType  = vk::DescriptorType::eSampler;
		write.pImageInfo      = infos.data();
		m_graphics.device.updateDescriptorSets(1, &write, 0, nullptr);
	}
	static std::atomic<uint32_t> logged = 0;
	if (logged.fetch_add(1) < 32) {
		LOGF("Bindless samplers: heap 0x%016" PRIx64 "+0x%x flags=%u region=%u records=%zu"
		     " (%u new)\n",
		     heap.base, heap.table_offset, heap.flags, heap.region, records.size(),
		     static_cast<uint32_t>(infos.size()));
	}
	heap.records.assign(records.begin(), records.end());
	return true;
}

void BindlessTable::CreatePlaceholders(CommandScheduler& /*scheduler*/) {
	struct Kind {
		vk::ImageType        type;
		vk::ImageViewType    view;
		uint32_t             layers;
		vk::ImageCreateFlags flags;
		vk::Format           format;
	};
	// In the order of ShaderRecompiler::IR::BindlessImageArrayBindings.
	const std::array<Kind, ImageArrays> kinds {{
	    {vk::ImageType::e2D, vk::ImageViewType::e2D, 1, {}, vk::Format::eR8G8B8A8Unorm},
	    {vk::ImageType::e2D, vk::ImageViewType::e2DArray, 1, {}, vk::Format::eR8G8B8A8Unorm},
	    {vk::ImageType::e2D, vk::ImageViewType::eCube, 6, vk::ImageCreateFlagBits::eCubeCompatible,
	     vk::Format::eR8G8B8A8Unorm},
	    {vk::ImageType::e3D, vk::ImageViewType::e3D, 1, {}, vk::Format::eR8G8B8A8Unorm},
	    {vk::ImageType::e2D, vk::ImageViewType::e2D, 1, {}, vk::Format::eR8G8B8A8Uint},
	    {vk::ImageType::e2D, vk::ImageViewType::e2DArray, 1, {}, vk::Format::eR8G8B8A8Uint},
	    {vk::ImageType::e2D, vk::ImageViewType::e2D, 1, {}, vk::Format::eR8G8B8A8Sint},
	    {vk::ImageType::e2D, vk::ImageViewType::e2DArray, 1, {}, vk::Format::eR8G8B8A8Sint},
	}};
	const auto integer = [](uint32_t i) { return i % ImageArrays >= 4u; };
	for (uint32_t i = 0; i < Placeholders; i++) {
		vk::ImageCreateInfo info {};
		info.flags         = kinds[i % ImageArrays].flags;
		info.imageType     = kinds[i % ImageArrays].type;
		info.format        = kinds[i % ImageArrays].format;
		info.extent        = vk::Extent3D {1, 1, 1};
		info.mipLevels     = 1;
		info.arrayLayers   = kinds[i % ImageArrays].layers;
		info.samples       = vk::SampleCountFlagBits::e1;
		info.tiling        = vk::ImageTiling::eOptimal;
		info.usage         = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
		info.initialLayout = vk::ImageLayout::eUndefined;
		EXIT_IF(!m_graphics.CreateImage(info, m_placeholders[i]));
		vk::ImageViewCreateInfo view {};
		view.image            = m_placeholders[i].image;
		view.viewType         = kinds[i % ImageArrays].view;
		view.format           = info.format;
		view.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, kinds[i % ImageArrays].layers};
		RequireVulkanSuccess(
		    m_graphics.device.createImageView(&view, nullptr, &m_placeholder_views[i]),
		    "create bindless placeholder view");
	}

	// One fenced submission, before any rendering: clears and a buffer fill are not allowed
	// inside the render passes draws record into.
	vk::CommandPoolCreateInfo pool_info {};
	pool_info.flags            = vk::CommandPoolCreateFlagBits::eTransient;
	pool_info.queueFamilyIndex = m_graphics.queue_family;
	vk::CommandPool pool       = nullptr;
	RequireVulkanSuccess(m_graphics.device.createCommandPool(&pool_info, nullptr, &pool),
	                     "create bindless init pool");
	vk::CommandBufferAllocateInfo allocate {};
	allocate.commandPool        = pool;
	allocate.level              = vk::CommandBufferLevel::ePrimary;
	allocate.commandBufferCount = 1;
	vk::CommandBuffer command   = nullptr;
	RequireVulkanSuccess(m_graphics.device.allocateCommandBuffers(&allocate, &command),
	                     "allocate bindless init command buffer");
	vk::CommandBufferBeginInfo begin {};
	begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
	RequireVulkanSuccess(command.begin(&begin), "begin bindless init command buffer");

	std::array<vk::ImageMemoryBarrier, Placeholders> to_transfer {};
	std::array<vk::ImageMemoryBarrier, Placeholders> to_read {};
	for (uint32_t i = 0; i < Placeholders; i++) {
		const vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eColor, 0, 1, 0,
		                                       kinds[i % ImageArrays].layers};
		to_transfer[i].srcAccessMask       = {};
		to_transfer[i].dstAccessMask       = vk::AccessFlagBits::eTransferWrite;
		to_transfer[i].oldLayout           = vk::ImageLayout::eUndefined;
		to_transfer[i].newLayout           = vk::ImageLayout::eTransferDstOptimal;
		to_transfer[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_transfer[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_transfer[i].image               = m_placeholders[i].image;
		to_transfer[i].subresourceRange    = range;
		to_read[i]                         = to_transfer[i];
		to_read[i].srcAccessMask           = vk::AccessFlagBits::eTransferWrite;
		to_read[i].dstAccessMask           = vk::AccessFlagBits::eShaderRead;
		to_read[i].oldLayout               = vk::ImageLayout::eTransferDstOptimal;
		to_read[i].newLayout               = vk::ImageLayout::eShaderReadOnlyOptimal;
	}
	command.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
	                        vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 0, nullptr,
	                        Placeholders, to_transfer.data());
	// Transparent black by default. KYTY_BINDLESS_DEBUG_COLORS=1: slot 0 grey (not resident), slot
	// 1 red (key outside its heap), slot 2 blue (pending), to see which case a surface is in.
	const bool debug_colors = std::getenv("KYTY_BINDLESS_DEBUG_COLORS") != nullptr;
	const auto color        = [debug_colors](float r, float g, float b) {
        return debug_colors ? vk::ClearColorValue {std::array<float, 4> {r, g, b, 1.0f}}
		                           : vk::ClearColorValue {std::array<float, 4> {0.0f, 0.0f, 0.0f, 0.0f}};
	};
	const std::array<vk::ClearColorValue, PlaceholderColors> colors {
	    color(0.5f, 0.5f, 0.5f), color(0.8f, 0.1f, 0.1f), color(0.1f, 0.2f, 0.8f)};
	// The integer arrays read zero.
	const vk::ClearColorValue zero {}; // all bits zero
	for (uint32_t i = 0; i < Placeholders; i++) {
		command.clearColorImage(m_placeholders[i].image, vk::ImageLayout::eTransferDstOptimal,
		                        integer(i) ? &zero : &colors[i / ImageArrays], 1,
		                        &to_transfer[i].subresourceRange);
	}
	// translation[0], the entry of keys outside their heap, is the red slot.
	command.fillBuffer(m_translation->Handle(), 4, VK_WHOLE_SIZE, 0u);
	command.fillBuffer(m_translation->Handle(), 0, 4, 1u);
	command.fillBuffer(m_feedback->Handle(), 0, VK_WHOLE_SIZE, 0u);
	vk::BufferMemoryBarrier translation_ready {};
	translation_ready.srcAccessMask       = vk::AccessFlagBits::eTransferWrite;
	translation_ready.dstAccessMask       = vk::AccessFlagBits::eShaderRead;
	translation_ready.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	translation_ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	translation_ready.buffer              = m_translation->Handle();
	translation_ready.offset              = 0;
	translation_ready.size                = VK_WHOLE_SIZE;
	auto feedback_ready                   = translation_ready;
	feedback_ready.dstAccessMask          = vk::AccessFlagBits::eShaderRead |
	                                        vk::AccessFlagBits::eShaderWrite |
	                                        vk::AccessFlagBits::eHostRead;
	feedback_ready.buffer                 = m_feedback->Handle();
	const std::array<vk::BufferMemoryBarrier, 2> buffers_ready {translation_ready, feedback_ready};
	command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                        vk::PipelineStageFlagBits::eAllCommands |
	                            vk::PipelineStageFlagBits::eHost,
	                        {}, 0, nullptr, static_cast<uint32_t>(buffers_ready.size()),
	                        buffers_ready.data(), Placeholders, to_read.data());
	RequireVulkanSuccess(command.end(), "end bindless init command buffer");

	vk::FenceCreateInfo fence_info {};
	vk::Fence           fence = nullptr;
	RequireVulkanSuccess(m_graphics.device.createFence(&fence_info, nullptr, &fence),
	                     "create bindless init fence");
	vk::SubmitInfo submit {};
	submit.commandBufferCount = 1;
	submit.pCommandBuffers    = &command;
	{
		Common::LockGuard lock(m_graphics.queue_mutex);
		RequireVulkanSuccess(m_graphics.queue.submit(1, &submit, fence), "submit bindless init");
	}
	RequireVulkanSuccess(m_graphics.device.waitForFences(1, &fence, VK_TRUE, UINT64_MAX),
	                     "wait for bindless init");
	m_graphics.device.destroyFence(fence, nullptr);
	m_graphics.device.destroyCommandPool(pool, nullptr);

	std::array<vk::DescriptorImageInfo, Placeholders> infos {};
	std::array<vk::WriteDescriptorSet, Placeholders> writes {};
	for (uint32_t i = 0; i < Placeholders; i++) {
		infos[i]                  = {nullptr, m_placeholder_views[i],
		                             vk::ImageLayout::eShaderReadOnlyOptimal};
		writes[i].dstSet          = m_set;
		writes[i].dstBinding      = ShaderRecompiler::IR::BindlessImageArrayBindings[i % ImageArrays];
		writes[i].dstArrayElement = i / ImageArrays;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType  = vk::DescriptorType::eSampledImage;
		writes[i].pImageInfo      = &infos[i];
	}
	m_graphics.device.updateDescriptorSets(Placeholders, writes.data(), 0, nullptr);
}

BindlessTable::~BindlessTable() {
	for (uint32_t i = 0; i < Placeholders; i++) {
		if (m_placeholder_views[i] != nullptr) {
			m_graphics.device.destroyImageView(m_placeholder_views[i], nullptr);
		}
		if (m_placeholders[i].image != nullptr) {
			m_graphics.DeleteImage(m_placeholders[i]);
		}
	}
	m_translation.reset();
	m_feedback.reset();
	m_feedback_snapshot.reset();
	if (m_pool != nullptr) {
		m_graphics.device.destroyDescriptorPool(m_pool, nullptr);
	}
	if (m_layout != nullptr) {
		m_graphics.device.destroyDescriptorSetLayout(m_layout, nullptr);
	}
}

BindlessTable::PreparationScope::PreparationScope(BindlessTable& table): m_table(table) {
	m_table.BeginPreparation();
}

BindlessTable::PreparationScope::~PreparationScope() {
	m_table.EndPreparation();
}

void BindlessTable::BeginPreparation() {
	if (m_preparation_depth++ == 0) {
		EXIT_IF(++m_preparation_serial == 0);
		m_preparation_used = false;
	}
}

void BindlessTable::EndPreparation() {
	EXIT_IF(m_preparation_depth == 0);
	if (--m_preparation_depth != 0) return;
	if (m_preparation_used)
		m_last_recorded_use_tick = std::max(m_last_recorded_use_tick, m_scheduler.CurrentTick());
	for (auto* region: m_prepared_regions) {
		// A ring wrap may have advanced the recording tick since descriptor commit.
		if (m_preparation_used)
			region->last_gpu_tick = std::max(region->last_gpu_tick, m_scheduler.CurrentTick());
		region->preparation = 0;
	}
	m_prepared_regions.clear();
}

void BindlessTable::PinRegion(Region& region) {
	if (m_preparation_depth != 0 && region.preparation != m_preparation_serial) {
		region.preparation = m_preparation_serial;
		m_prepared_regions.push_back(&region);
	}
}

void BindlessTable::PinRegion(const Heap& heap) {
	if (heap.region_generation == 0) return;
	const auto found = std::ranges::find(m_regions, heap.region_generation, &Region::generation);
	EXIT_IF(found == m_regions.end());
	PinRegion(*found);
}

void BindlessTable::MarkPreparationUsed(uint64_t tick) {
	EXIT_IF(tick > m_scheduler.CurrentTick());
	m_last_recorded_use_tick = std::max(m_last_recorded_use_tick, tick);
	for (auto* region: m_prepared_regions)
		region->last_gpu_tick = std::max(region->last_gpu_tick, tick);
	m_preparation_used = true;
}

bool BindlessTable::IsPreparationRegion(const RegionUse& use) const {
	return m_preparation_depth != 0 && std::ranges::any_of(m_regions, [&](const Region& region) {
		return region.generation == use.generation && region.offset == use.region &&
		       use.entries <= region.capacity && region.preparation == m_preparation_serial;
	});
}

bool BindlessTable::CanAllocateRegion(uint32_t capacity) const {
	return capacity <= ShaderRecompiler::IR::WatchdogReportBase - m_next_region ||
	       std::ranges::any_of(m_free_extents, [&](const FreeExtent& extent) {
		       return capacity <= extent.capacity;
	       });
}

uint32_t BindlessTable::AcquireRegion(uint32_t capacity) {
	for (auto it = m_free_extents.begin(); it != m_free_extents.end(); ++it) {
		if (capacity > it->capacity) continue;
		const auto offset = it->offset;
		it->offset += capacity;
		it->capacity -= capacity;
		if (it->capacity == 0) m_free_extents.erase(it);
		return offset;
	}
	if (capacity > ShaderRecompiler::IR::WatchdogReportBase - m_next_region) return 0;
	const auto offset = m_next_region;
	m_next_region += capacity;
	return offset;
}

void BindlessTable::FreeRegion(const Region& region) {
	m_free_extents.push_back({region.offset, region.capacity});
	std::ranges::sort(m_free_extents, {}, &FreeExtent::offset);
	for (size_t index = 1; index < m_free_extents.size();) {
		auto& previous = m_free_extents[index - 1];
		const auto end = uint64_t {previous.offset} + previous.capacity;
		EXIT_IF(end > m_free_extents[index].offset);
		if (end == m_free_extents[index].offset) {
			previous.capacity += m_free_extents[index].capacity;
			m_free_extents.erase(m_free_extents.begin() + index);
		} else {
			++index;
		}
	}
}

void BindlessTable::RemoveImageReferences(const Heap& heap) {
	for (auto it = m_image_refs.begin(); it != m_image_refs.end();) {
		std::erase_if(it->second, [&](const auto& reference) { return reference.first == &heap; });
		if (it->second.empty()) it = m_image_refs.erase(it);
		else ++it;
	}
}

void BindlessTable::ReclaimRegions(const Heap& protected_heap, uint32_t required_entries) {
	const auto completed = [&](const Region& region) {
		return region.preparation == 0 &&
		       (region.last_gpu_tick == 0 || m_scheduler.IsFree(region.last_gpu_tick));
	};
	for (auto it = m_regions.begin(); it != m_regions.end();) {
		if (it->retired && completed(*it)) {
			FreeRegion(*it);
			it = m_regions.erase(it);
		} else ++it;
	}
	if (CanAllocateRegion(required_entries)) return;
	std::vector<Heap*> inactive;
	for (auto& heap: m_heaps) {
		if (&heap == &protected_heap || heap.region_generation == 0) continue;
		const bool busy = std::ranges::any_of(m_regions, [&](const Region& region) {
			return region.owner == heap.identity && !completed(region);
		});
		if (!busy) inactive.push_back(&heap);
	}
	std::ranges::sort(inactive, [](const Heap* a, const Heap* b) {
		if (a->checked_frame != b->checked_frame) return a->checked_frame < b->checked_frame;
		if (a->last_preparation != b->last_preparation)
			return a->last_preparation < b->last_preparation;
		return a->identity < b->identity;
	});
	for (const auto* heap: inactive) {
		RemoveImageReferences(*heap);
		for (auto it = m_regions.begin(); it != m_regions.end();) {
			if (it->owner == heap->identity) {
				FreeRegion(*it);
				it = m_regions.erase(it);
			} else ++it;
		}
		const auto found = std::ranges::find_if(m_heaps, [&](const Heap& candidate) {
			return &candidate == heap;
		});
		EXIT_IF(found == m_heaps.end());
		m_heaps.erase(found);
		if (CanAllocateRegion(required_entries)) break;
	}
}

bool BindlessTable::AllocateRegion(Heap& heap, uint32_t entries, uint32_t required_entries) {
	// Headroom so a growing heap rarely moves; a move re-writes the keys resolved so far.
	const auto preferred = std::max<uint64_t>(entries, std::min<uint64_t>(uint64_t {entries} * 2u, 1u << 16u));
	if (required_entries == 0 || required_entries >= ShaderRecompiler::IR::WatchdogReportBase)
		return false;
	if (!CanAllocateRegion(static_cast<uint32_t>(preferred)))
		ReclaimRegions(heap, required_entries);
	if (!CanAllocateRegion(required_entries) && m_scheduler.Active()) {
		// Wait never drains general resource callbacks while a draw's views are prepared.
		m_scheduler.Wait(m_scheduler.CurrentTick());
		ReclaimRegions(heap, required_entries);
	}
	const auto capacity = CanAllocateRegion(static_cast<uint32_t>(preferred))
	                          ? static_cast<uint32_t>(preferred) : required_entries;
	const auto region = AcquireRegion(capacity);
	if (region == 0) return false;
	const auto old_region  = heap.region;
	const auto old_entries = heap.entries;
	if (heap.region_generation != 0) {
		const auto old = std::ranges::find(m_regions, heap.region_generation, &Region::generation);
		EXIT_IF(old == m_regions.end());
		old->retired = true;
	}
	EXIT_IF(++m_next_region_generation == 0);
	auto& ownership = m_regions.emplace_back(Region {.offset = region, .capacity = capacity,
	                                                .generation = m_next_region_generation,
	                                                .owner = heap.identity});
	PinRegion(ownership);
	heap.region_generation = ownership.generation;
	heap.region            = region;
	heap.entries           = capacity;
	heap.region_snapshot   = m_snapshot_count;
	heap.slots.resize(capacity, 0u);
	heap.settled.resize(capacity, 0u);
	heap.record_hash.resize(capacity, 0u);
	heap.listed.resize(capacity, 0u);
	auto* translation = reinterpret_cast<uint32_t*>(m_translation->Mapped().data());
	auto* feedback    = reinterpret_cast<uint32_t*>(m_feedback->Mapped().data());
	for (uint32_t key = 0; key < capacity; key++) {
		uint32_t value = ShaderRecompiler::IR::BindlessPending;
		if (key < old_entries && heap.settled[key] != 0) {
			value = heap.slots[key];
		}
		translation[heap.region + key] = value;
		feedback[heap.region + key]    = 0;
	}
	if (!m_translation->IsCoherent()) {
		m_translation->Flush(heap.region * sizeof(uint32_t), capacity * sizeof(uint32_t));
	}
	LOGF("Bindless heap region: base=0x%016" PRIx64 " binding=%u region=%u entries=%u (was %u at "
	     "%u)\n",
	     heap.base, heap.binding, heap.region, capacity, old_entries, old_region);
	return true;
}

BindlessTable::Heap* BindlessTable::FindOrCreateHeap(
    uint64_t base, uint32_t table_offset, uint32_t record_stride, uint32_t binding,
    uint32_t entries,
    const ShaderRecompiler::IR::ImageResource& resource, uint32_t pointer_offset,
    uint32_t sub_stride, uint32_t sub_keys, uint64_t pointer_mask) {
	if (entries == 0) {
		return nullptr;
	}
	for (auto& heap: m_heaps) {
		if (heap.base == base && heap.table_offset == table_offset &&
		    heap.record_stride == record_stride && heap.binding == binding &&
		    heap.resource.r128 == resource.r128 && heap.pointer_offset == pointer_offset &&
		    heap.sub_stride == sub_stride && heap.sub_keys == sub_keys &&
		    heap.pointer_mask == pointer_mask) {
			if (m_preparation_depth != 0) heap.last_preparation = m_preparation_serial;
			PinRegion(heap);
			if (entries > heap.entries && !AllocateRegion(heap, entries, entries))
				EXIT("Bindless translation arena cannot hold %u required keys without retiring a live region\n", entries);
			return &heap;
		}
	}
	auto& heap        = m_heaps.emplace_back();
	EXIT_IF(++m_next_heap_identity == 0);
	heap.identity = m_next_heap_identity;
	if (m_preparation_depth != 0) heap.last_preparation = m_preparation_serial;
	heap.base         = base;
	heap.table_offset = table_offset;
	heap.record_stride = record_stride;
	heap.binding      = binding;
	heap.resource     = resource;
	heap.pointer_offset = pointer_offset;
	heap.pointer_mask   = pointer_mask;
	heap.sub_stride     = sub_stride;
	heap.sub_keys       = sub_keys;
	// A plain T# heap grows as the guest streams textures in; a table of larger records (one
	// per material) has a fixed size.
	// KYTY_BINDLESS_KEY_BOUNDS: a small table (Ghost of Yotei: 131 tables of 8 T#s per frame for
	// 6cc64dee) gets room for four times its keys instead of 16384, so the regions do not fill up
	// and start over every few frames.
	const auto initial = record_stride != 32u ? std::max(entries, 16u)
	                     : KeyBoundsEnabled()  ? std::max(entries, std::min(entries * 4u, 1u << 14u))
	                                           : std::max(entries, 1u << 14u);
	if (!AllocateRegion(heap, initial, entries)) {
		m_heaps.pop_back();
		EXIT("Bindless translation arena cannot hold %u required keys without retiring a live region\n", entries);
	}
	return &heap;
}

uint32_t BindlessTable::SharedSlot(uint32_t binding, ImageId id, vk::ImageView view,
                                   vk::ImageLayout layout) {
	const auto key =
	    (static_cast<uint64_t>(binding) << 60u) ^ reinterpret_cast<uint64_t>(static_cast<VkImageView>(view));
	if (const auto found = m_shared_slots.find(key); found != m_shared_slots.end()) {
		return found->second;
	}
	const auto slot = AllocateSlot(binding);
	if (slot == 0) {
		return 0;
	}
	WriteSlot(binding, slot, view, layout);
	m_shared_slots.emplace(key, slot);
	m_image_shared_slots[ImageKey(id)].push_back({binding, slot, key});
	return slot;
}

uint32_t BindlessTable::AllocateSlot(uint32_t binding) {
	const auto index = ShaderRecompiler::IR::BindlessImageArrayIndex(binding);
	if (index >= ImageArrays || m_next_slot[index] >= m_images_per_array) {
		return 0;
	}
	return m_next_slot[index]++;
}

void BindlessTable::WriteSlot(uint32_t binding, uint32_t slot, vk::ImageView view,
                              vk::ImageLayout layout) {
	const vk::DescriptorImageInfo info {nullptr, view, layout};
	vk::WriteDescriptorSet        write {};
	write.dstSet          = m_set;
	write.dstBinding      = binding;
	write.dstArrayElement = slot;
	write.descriptorCount = 1;
	write.descriptorType  = vk::DescriptorType::eSampledImage;
	write.pImageInfo      = &info;
	m_graphics.device.updateDescriptorSets(1, &write, 0, nullptr);
}

void BindlessTable::SetTranslation(const Heap& heap, uint32_t key, uint32_t slot) {
	auto* translation = reinterpret_cast<uint32_t*>(m_translation->Mapped().data());
	translation[heap.region + key] = slot;
	if (!m_translation->IsCoherent()) {
		m_translation->Flush((heap.region + key) * sizeof(uint32_t), sizeof(uint32_t));
	}
}

void BindlessTable::RecordFeedbackSnapshot(CommandScheduler& scheduler) {
	// Regions are allocated contiguously after word 0, so one copy covers every heap.
	const vk::DeviceSize size = vk::DeviceSize {m_next_region} * sizeof(uint32_t);
	scheduler.EndRendering();
	auto                    command = scheduler.Current().Handle();
	vk::BufferMemoryBarrier flags_written {};
	flags_written.srcAccessMask       = vk::AccessFlagBits::eShaderWrite;
	flags_written.dstAccessMask       = vk::AccessFlagBits::eTransferRead;
	flags_written.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	flags_written.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	flags_written.buffer              = m_feedback->Handle();
	flags_written.offset              = 0;
	flags_written.size                = size;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                        vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 1,
	                        &flags_written, 0, nullptr);
	const vk::BufferCopy copy {0, 0, size};
	command.copyBuffer(m_feedback->Handle(), m_feedback_snapshot->Handle(), 1, &copy);
	auto snapshot_written          = flags_written;
	snapshot_written.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	snapshot_written.dstAccessMask = vk::AccessFlagBits::eHostRead;
	snapshot_written.buffer        = m_feedback_snapshot->Handle();
	command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                        vk::PipelineStageFlagBits::eHost, {}, 0, nullptr, 1,
	                        &snapshot_written, 0, nullptr);
	m_snapshot_tick = scheduler.CurrentTick();
	// The copy reads every allocated feedback extent, including retired growth regions.
	for (auto& region: m_regions)
		region.last_gpu_tick = std::max(region.last_gpu_tick, m_snapshot_tick);
	m_snapshot_recorded = ++m_snapshot_count;
	if (BindlessProvenance::Enabled()) m_diagnostic_snapshot_tick = m_snapshot_tick;
}

bool BindlessTable::SnapshotReady(CommandScheduler& scheduler) {
	return m_snapshot_tick != 0 && scheduler.IsFree(m_snapshot_tick);
}

// KYTY_BINDLESS_KEY_BOUNDS (default on, =0 turns it off): requests only for keys a draw's descriptor covers, and none from a
// snapshot recorded before the heap's region was allocated: a new heap may reuse a retired
// region whose flags the older snapshot still holds (Ghost of Yotei: 6cc64dee's
// 8-key table got requests for keys near 18000, other data whose resolution lost the device).
bool BindlessTable::KeyBoundsEnabled() {
	static const bool enabled = [] {
		const char* text = KYTY_GETENV("KYTY_BINDLESS_KEY_BOUNDS");
		return text == nullptr || std::strcmp(text, "0") != 0;
	}();
	return enabled;
}

void BindlessTable::TakeRequests(const Heap& heap, std::vector<uint32_t>& keys) {
	if (heap.region_snapshot >= m_snapshot_recorded) {
		return; // the snapshot predates this region: its words belong to another heap
	}
	const auto* snapshot = reinterpret_cast<const uint32_t*>(m_feedback_snapshot->Mapped().data());
	auto*       feedback = reinterpret_cast<uint32_t*>(m_feedback->Mapped().data());
	m_feedback_snapshot->Invalidate(heap.region * sizeof(uint32_t),
	                                heap.entries * sizeof(uint32_t));
	bool cleared = false;
	const auto limit = KeyBoundsEnabled() && heap.valid_keys != 0 ? std::min(heap.valid_keys, heap.entries)
	                                                              : heap.entries;
	static uint64_t dropped = 0;
	static auto     next_report = std::chrono::steady_clock::now();
	if (limit < heap.entries) {
		for (uint32_t key = limit; key < heap.entries; key++) {
			if (snapshot[heap.region + key] != 0) {
				feedback[heap.region + key] = 0;
				dropped++;
			}
		}
		if (const auto now = std::chrono::steady_clock::now(); dropped != 0 && now >= next_report) {
			next_report = now + std::chrono::seconds(30);
			std::printf("\x1b[91mbindless: %llu requests for keys past their descriptor dropped (heap 0x%llx "
			            "covers %u keys)\x1b[0m\n",
			            static_cast<unsigned long long>(dropped),
			            static_cast<unsigned long long>(heap.base), limit);
			std::fflush(stdout);
			dropped = 0;
		}
	}
	for (uint32_t key = 0; key < limit; key++) {
		if (snapshot[heap.region + key] != 0) {
			// A flag the GPU sets again after the snapshot may be lost here; a key that is
			// still pending is flagged again by the next draw that samples it.
			feedback[heap.region + key] = 0;
			keys.push_back(key);
			cleared = true;
		}
	}
	if (cleared) {
		m_feedback->Flush(heap.region * sizeof(uint32_t), heap.entries * sizeof(uint32_t));
	}
}

void BindlessTable::DrainWatchdogReports(uint64_t frame) {
	using ShaderRecompiler::IR::WatchdogReportBase;
	using ShaderRecompiler::IR::WatchdogReportPhis;
	using ShaderRecompiler::IR::WatchdogReportSlots;
	using ShaderRecompiler::IR::WatchdogReportWords;
	static_assert(WatchdogReportBase + WatchdogReportWords * (WatchdogReportSlots + 1) <=
	              TranslationEntries);
	auto* feedback = reinterpret_cast<uint32_t*>(m_feedback->Mapped().data());
	if (!m_feedback->IsCoherent()) {
		m_feedback->Invalidate(WatchdogReportBase * sizeof(uint32_t),
		                       WatchdogReportWords * (WatchdogReportSlots + 1) * sizeof(uint32_t));
	}
	const auto count = feedback[WatchdogReportBase];
	if (count == 0) {
		return;
	}
	const auto stored = std::min(count, WatchdogReportSlots);
	// Per (shader, loop): how many tripped, and the first report in full.
	std::unordered_map<uint64_t, std::pair<uint32_t, const uint32_t*>> loops;
	for (uint32_t n = 0; n < stored; n++) {
		const auto* report = feedback + WatchdogReportBase + WatchdogReportWords * (n + 1);
		if ((report[0] >> 16u) != 0x5744u) {
			continue;
		}
		const auto key = (static_cast<uint64_t>(report[2]) << 32u | report[1]) ^
		                 (static_cast<uint64_t>(report[0] & 0xffffu) << 48u);
		auto& entry = loops[key];
		if (entry.first++ == 0) {
			entry.second = report;
		}
	}
	static std::atomic<uint32_t> logged = 0;
	LOGF("WatchdogTrips: frame=%" PRIu64 " reports=%u loops=%zu\n", frame, count, loops.size());
	for (const auto& [key, entry]: loops) {
		if (logged.fetch_add(1) >= 256) {
			break;
		}
		const auto* r = entry.second;
		std::string phis;
		for (uint32_t half = 0; half < 2; half++) {
			phis += half == 0 ? " lo:" : " hi:";
			for (uint32_t i = 0; i < WatchdogReportPhis; i++) {
				phis += fmt::format(" {:x}", r[16 + half * WatchdogReportPhis + i]);
			}
		}
		LOGF("WatchdogTrip: hash=0x%08x%08x loop_block=%u trips=%u counter=%u group=%u lane=%u "
		     "ud=[%08x %08x %08x %08x %08x %08x %08x %08x]%s\n",
		     r[2], r[1], r[0] & 0xffffu, entry.first, r[3], r[4], r[5], r[6], r[7], r[8], r[9],
		     r[10], r[11], r[12], r[13], phis.c_str());
	}
	feedback[WatchdogReportBase] = 0;
	if (!m_feedback->IsCoherent()) {
		m_feedback->Flush(WatchdogReportBase * sizeof(uint32_t), sizeof(uint32_t));
	}
}

void BindlessTable::AddImageReference(ImageId id, Heap& heap, uint32_t key) {
	m_image_refs[ImageKey(id)].emplace_back(&heap, key);
}

void BindlessTable::OnImageUnregistered(ImageId id) {
	const auto image_key = ImageKey(id);
	if (!m_image_shared_slots.contains(image_key) && !m_image_refs.contains(image_key)) return;
	// Preserve recorded consumers before changing their descriptors or translation words.
	// Wait submits the current tick if needed, without draining resource callbacks mid-prepare.
	if (m_last_recorded_use_tick != 0 && !m_scheduler.IsFree(m_last_recorded_use_tick))
		m_scheduler.Wait(m_last_recorded_use_tick);
	if (const auto shared = m_image_shared_slots.find(ImageKey(id));
	    shared != m_image_shared_slots.end()) {
		for (const auto& [binding, slot, key]: shared->second) {
			WriteSlot(static_cast<uint32_t>(binding), static_cast<uint32_t>(slot),
			          m_placeholder_views[ShaderRecompiler::IR::BindlessImageArrayIndex(binding)], vk::ImageLayout::eShaderReadOnlyOptimal);
			m_shared_slots.erase(key);
		}
		m_image_shared_slots.erase(shared);
	}
	const auto found = m_image_refs.find(ImageKey(id));
	if (found == m_image_refs.end()) {
		return;
	}
	for (const auto& [heap, key]: found->second) {
		if (heap->slots[key] != 0) {
			// The entry may name another array than the heap's (2D in 3D, 3D in 2D, typed kinds).
			const auto binding = ShaderRecompiler::IR::BindlessEntryBinding(heap->slots[key], heap->binding);
			WriteSlot(binding, heap->slots[key] & ~ShaderRecompiler::IR::BindlessSlotFlags,
			          m_placeholder_views[ShaderRecompiler::IR::BindlessImageArrayIndex(binding)],
			          vk::ImageLayout::eShaderReadOnlyOptimal);
		}
		heap->slots[key]   = 0;
		heap->settled[key] = 0;
		SetTranslation(*heap, key, ShaderRecompiler::IR::BindlessPending);
		std::erase(heap->resolved, id);
	}
	m_image_refs.erase(found);
}

} // namespace Libs::Graphics
