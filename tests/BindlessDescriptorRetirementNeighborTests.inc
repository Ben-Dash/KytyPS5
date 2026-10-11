#include <sstream>

namespace Libs::Graphics {
namespace {

std::vector<uint32_t> FixedSlotRetirementShader(uint32_t images, uint32_t samplers, uint32_t slot) {
  // One actual sample, stored twice for the existing two-channel color oracle.
  // No translation variable and no region metadata occurs in this shader.
  std::ostringstream code;
  code << R"(OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" %output %images %samplers
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %out_array ArrayStride 16
OpDecorate %out_block Block
OpMemberDecorate %out_block 0 Offset 0
OpDecorate %output DescriptorSet 0
OpDecorate %output Binding 0
OpDecorate %images DescriptorSet 2
OpDecorate %images Binding 0
OpDecorate %samplers DescriptorSet 2
OpDecorate %samplers Binding 6
%void = OpTypeVoid
%fn = OpTypeFunction %void
%uint = OpTypeInt 32 0
%float = OpTypeFloat 32
%vec2 = OpTypeVector %float 2
%vec4 = OpTypeVector %float 4
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%two = OpConstant %uint 2
%float_zero = OpConstant %float 0
%half = OpConstant %float 0.5
%coord = OpConstantComposite %vec2 %half %half
%out_array = OpTypeArray %vec4 %two
%out_block = OpTypeStruct %out_array
%out_ptr = OpTypePointer StorageBuffer %out_block
%out_vec_ptr = OpTypePointer StorageBuffer %vec4
%output = OpVariable %out_ptr StorageBuffer
%image = OpTypeImage %float 2D 0 0 0 1 Unknown
)";
  code << "%image_count = OpConstant %uint " << images << '\n';
  code << "%sampler_count = OpConstant %uint " << samplers << '\n';
  code << "%fixed_slot = OpConstant %uint " << slot << '\n';
  code << R"(%image_array = OpTypeArray %image %image_count
%image_array_ptr = OpTypePointer UniformConstant %image_array
%image_ptr = OpTypePointer UniformConstant %image
%images = OpVariable %image_array_ptr UniformConstant
%sampler = OpTypeSampler
%sampler_array = OpTypeArray %sampler %sampler_count
%sampler_array_ptr = OpTypePointer UniformConstant %sampler_array
%sampler_ptr = OpTypePointer UniformConstant %sampler
%samplers = OpVariable %sampler_array_ptr UniformConstant
%sampled_image = OpTypeSampledImage %image
%main = OpFunction %void None %fn
%entry = OpLabel
%image_address = OpAccessChain %image_ptr %images %fixed_slot
%image_value = OpLoad %image %image_address
%sampler_address = OpAccessChain %sampler_ptr %samplers %zero
%sampler_value = OpLoad %sampler %sampler_address
%combined = OpSampledImage %sampled_image %image_value %sampler_value
%rgba = OpImageSampleExplicitLod %vec4 %combined %coord Lod %float_zero
%out0 = OpAccessChain %out_vec_ptr %output %zero %zero
%out1 = OpAccessChain %out_vec_ptr %output %zero %one
OpStore %out0 %rgba
OpStore %out1 %rgba
OpReturn
OpFunctionEnd
)";
  spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
  tools.SetMessageConsumer([](spv_message_level_t, const char *, const spv_position_t &p, const char *m) {
    std::fprintf(stderr, "FixedSlotRetirement SPIR-V word%zu: %s\n", p.index, m);
  });
  std::vector<uint32_t> words;
  Require("FixedSlotRetirement", "synthetic assembly", tools.Assemble(code.str(), &words),
          "fixed-slot synthetic shader assembly failed; not a semantic reproduction");
  Require("FixedSlotRetirement", "synthetic validation", tools.Validate(words),
          "fixed-slot shader failed Vulkan SPIR-V validation; not a semantic reproduction");
  return words;
}

class RetirementNeighborReader {
public:
  RetirementNeighborReader(VulkanHarness &vulkan, const std::vector<uint32_t> &spirv,
                           BindlessTable::Heap *heap, uint64_t heap_base)
      : m_vulkan(vulkan), m_heap(heap), m_heap_base(heap_base) {
    auto device = vulkan.Device();
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
    Require("RetirementNeighbors", "bounded shader validation", tools.Validate(spirv),
            "native sampling shader failed validation before pipeline/dispatch");
    vk::ShaderModuleCreateInfo module{};
    module.codeSize = spirv.size() * sizeof(uint32_t);
    module.pCode = spirv.data();
    RequireVulkanSuccess(device.createShaderModule(&module, nullptr, &m_module), "neighbor sampled module");
    vk::BufferCreateInfo buffer{};
    buffer.size = 32u;
    buffer.usage = vk::BufferUsageFlagBits::eStorageBuffer;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    allocation.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    allocation.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    VmaAllocationInfo mapped{};
    RequireVulkanSuccess(static_cast<vk::Result>(vmaCreateBuffer(vulkan.RuntimeContext().allocator,
        reinterpret_cast<const VkBufferCreateInfo *>(&buffer), &allocation, &m_buffer,
        &m_allocation, &mapped)), "real neighbor sampled output");
    m_mapped = mapped.pMappedData;
    Require("RetirementNeighbors", "real mapped output", m_mapped != nullptr, "output allocation is not mapped");
    const vk::DescriptorSetLayoutBinding binding{0u, vk::DescriptorType::eStorageBuffer,
        1u, vk::ShaderStageFlagBits::eCompute, nullptr};
    vk::DescriptorSetLayoutCreateInfo output_layout{};
    output_layout.bindingCount = 1u;
    output_layout.pBindings = &binding;
    m_layouts[2] = vulkan.RuntimeRenderer().GetBindlessTable().Layout();
    RequireVulkanSuccess(device.createDescriptorSetLayout(&output_layout, nullptr, &m_layouts[0]), "neighbor output layout");
    vk::DescriptorSetLayoutCreateInfo empty{};
    RequireVulkanSuccess(device.createDescriptorSetLayout(&empty, nullptr, &m_layouts[1]), "neighbor intermediate layout");
    const vk::DescriptorPoolSize size{vk::DescriptorType::eStorageBuffer, 1u};
    vk::DescriptorPoolCreateInfo pool{};
    pool.maxSets = pool.poolSizeCount = 1u;
    pool.pPoolSizes = &size;
    RequireVulkanSuccess(device.createDescriptorPool(&pool, nullptr, &m_pool), "neighbor output pool");
    vk::DescriptorSetAllocateInfo allocate{};
    allocate.descriptorPool = m_pool;
    allocate.descriptorSetCount = 1u;
    allocate.pSetLayouts = &m_layouts[0];
    RequireVulkanSuccess(device.allocateDescriptorSets(&allocate, &m_set), "neighbor output set");
    const vk::DescriptorBufferInfo info{m_buffer, 0u, 32u};
    vk::WriteDescriptorSet write{};
    write.dstSet = m_set;
    write.dstBinding = 0u;
    write.descriptorCount = 1u;
    write.descriptorType = vk::DescriptorType::eStorageBuffer;
    write.pBufferInfo = &info;
    device.updateDescriptorSets(1u, &write, 0u, nullptr);
    vk::PipelineLayoutCreateInfo layout{};
    layout.setLayoutCount = 3u;
    layout.pSetLayouts = m_layouts.data();
    RequireVulkanSuccess(device.createPipelineLayout(&layout, nullptr, &m_pipeline_layout), "neighbor sampled pipeline layout");
    vk::ComputePipelineCreateInfo pipeline{};
    pipeline.layout = m_pipeline_layout;
    pipeline.stage.stage = vk::ShaderStageFlagBits::eCompute;
    pipeline.stage.module = m_module;
    pipeline.stage.pName = "main";
    RequireVulkanSuccess(device.createComputePipelines(nullptr, 1u, &pipeline, nullptr, &m_pipeline), "neighbor sampled pipeline");
  }
  ~RetirementNeighborReader() {
    auto device = m_vulkan.Device();
    device.destroyPipeline(m_pipeline, nullptr);
    device.destroyPipelineLayout(m_pipeline_layout, nullptr);
    device.destroyDescriptorPool(m_pool, nullptr);
    device.destroyDescriptorSetLayout(m_layouts[1], nullptr);
    device.destroyDescriptorSetLayout(m_layouts[0], nullptr);
    device.destroyShaderModule(m_module, nullptr);
    vmaDestroyBuffer(m_vulkan.RuntimeContext().allocator, m_buffer, m_allocation);
  }
  void Record() {
    std::array<uint32_t, 8> sentinel;
    sentinel.fill(0xdeadbeefu);
    std::memcpy(m_mapped, sentinel.data(), 32u);
    RequireVulkanSuccess(static_cast<vk::Result>(vmaFlushAllocation(m_vulkan.RuntimeContext().allocator,
        m_allocation, 0u, 32u)), "neighbor sentinel flush");
    auto &context = m_vulkan.RuntimeRenderer();
    auto &scheduler = context.GetCommandScheduler();
    auto &table = context.GetBindlessTable();
    BindlessTable::PreparationScope operation(table);
    if (m_heap != nullptr) {
      (void)table.FindOrCreateHeap(m_heap_base, 0u, 32u, BindlessTable::Images2D, 1u,
                                  NeighborImageResource());
    } // Fixed-slot-only reader deliberately has no heap lookup or region lease.
    auto command = scheduler.Current().Handle();
    vk::BufferMemoryBarrier ready{};
    ready.srcAccessMask = vk::AccessFlagBits::eHostWrite;
    ready.dstAccessMask = vk::AccessFlagBits::eShaderWrite;
    ready.srcQueueFamilyIndex = ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ready.buffer = m_buffer;
    ready.size = 32u;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eHost, vk::PipelineStageFlagBits::eComputeShader,
                            {}, 0u, nullptr, 1u, &ready, 0u, nullptr);
    command.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline);
    command.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_pipeline_layout, 0u, 1u, &m_set, 0u, nullptr);
    const auto bindless = table.Set();
    command.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_pipeline_layout, 2u, 1u, &bindless, 0u, nullptr);
    table.MarkPreparationUsed(scheduler.CurrentTick());
    command.dispatch(1u, 1u, 1u);
    auto done = ready;
    done.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
    done.dstAccessMask = vk::AccessFlagBits::eHostRead;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eHost,
                            {}, 0u, nullptr, 1u, &done, 0u, nullptr);
  }
  void RequireColor(const char *stage, const std::array<uint32_t, 4> &rgba) {
    RequireVulkanSuccess(static_cast<vk::Result>(vmaInvalidateAllocation(m_vulkan.RuntimeContext().allocator,
        m_allocation, 0u, 32u)), "neighbor actual sampled readback");
    std::array<uint32_t, 8> actual{};
    std::memcpy(actual.data(), m_mapped, 32u);
    std::array<uint32_t, 8> expected{};
    std::copy(rgba.begin(), rgba.end(), expected.begin());
    std::copy(rgba.begin(), rgba.end(), expected.begin() + 4u);
    if (actual != expected)
      std::printf("[gpu] %s readback=%08x,%08x,%08x,%08x / %08x,%08x,%08x,%08x\n",
                  stage, actual[0], actual[1], actual[2], actual[3], actual[4], actual[5], actual[6], actual[7]);
    Require("RetirementNeighbors", stage, actual == expected,
            "actual sampled pixels lost the independently specified guest color");
  }
private:
  VulkanHarness &m_vulkan;
  BindlessTable::Heap *m_heap;
  uint64_t m_heap_base;
  VkBuffer m_buffer = VK_NULL_HANDLE;
  VmaAllocation m_allocation = nullptr;
  void *m_mapped = nullptr;
  std::array<vk::DescriptorSetLayout, 3> m_layouts{};
  vk::DescriptorPool m_pool = nullptr;
  vk::DescriptorSet m_set = nullptr;
  vk::ShaderModule m_module = nullptr;
  vk::PipelineLayout m_pipeline_layout = nullptr;
  vk::Pipeline m_pipeline = nullptr;
};

void CheckBindlessDescriptorRetirementNeighbors(VulkanHarness &vulkan) {
  constexpr const char *name = "BindlessSampledRetirementNeighbors";
  constexpr uint64_t base = 0x0000000200800000ull, bytes = 0x10000u, alignment = 0x10000u;
  int64_t offset = -1;
  Require(name, "real allocation", LibKernel::Memory::KernelAllocateDirectMemory(0,
      LibKernel::Memory::KernelGetDirectMemorySize(), bytes, alignment, 0, &offset) == 0,
      "actual guest allocation failed");
  void *mapped = reinterpret_cast<void *>(base);
  Require(name, "real map", LibKernel::Memory::KernelMapDirectMemory(&mapped, bytes, 0x3, 0x10,
      offset, alignment) == 0 && mapped == reinterpret_cast<void *>(base), "actual guest map failed");
  constexpr std::array<uint32_t, 5> texels{0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xff00ff00u, 0xffff0000u};
  for (uint32_t i = 0; i < texels.size(); ++i) {
    const auto address = base + uint64_t{i} * 0x1000u;
    std::memcpy(reinterpret_cast<void *>(address), &texels[i], 4u);
    uint32_t backed = 0;
    Require(name, "actual backing", LibKernel::Memory::TryReadBacking(address, &backed, 4u) && backed == texels[i],
            "synthetic color is not in actual tracked guest backing");
  }
  auto &context = vulkan.RuntimeRenderer();
  auto &graphics = vulkan.RuntimeContext();
  auto &scheduler = context.GetCommandScheduler();
  auto &cache = context.GetTextureCache();
  auto &table = context.GetBindlessTable();
  HW::Context registers{};
  HW::UserConfig user_config{};
  HW::Shader shaders{};
  scheduler.Begin(registers, user_config, shaders);
  LibKernel::Memory::InstallGpuResources(&context);
  context.MapMemory(base, bytes);
  const auto image = [&](uint32_t index) {
    TextureCache::ImageDesc desc{};
    desc.type = TextureCache::BindingType::Texture;
    desc.info.data = {base + uint64_t{index} * 0x1000u, 4u};
    desc.info.pixel_format = vk::Format::eR8G8B8A8Unorm;
    desc.info.guest_format = Prospero::BufferFormat::k8_8_8_8UNorm;
    desc.info.type = Prospero::ImageType::kColor2D;
    desc.info.extent = {1u, 1u, 1u};
    desc.info.resources = {1u, 1u};
    desc.info.pitch = desc.info.samples = 1u;
    desc.info.bytes_per_block = 4u;
    desc.info.tile_mode = Prospero::TileMode::kLinear;
    desc.info.mip_layout[0] = {0u, 4u, 1u, 1u};
    desc.view_info.format = desc.info.pixel_format;
    desc.view_info.type = vk::ImageViewType::e2D;
    desc.view_info.aspect = vk::ImageAspectFlagBits::eColor;
    desc.view_info.usage = vk::ImageUsageFlagBits::eSampled;
    desc.view_info.level_count = desc.view_info.layer_count = 1u;
    const auto id = cache.FindImage(desc);
    const auto view = cache.FindTexture(id, desc);
    Require(name, "actual colored cache image", view != nullptr && cache.PeekImage(id) != nullptr && cache.PeekImage(id)->registered,
            "real colored cache image/view acquisition failed");
    cache.GetImage(id).Transit(vk::ImageLayout::eShaderReadOnlyOptimal,
        vk::AccessFlagBits2::eShaderRead, {}, scheduler.Current().Handle());
    return std::pair{id, view};
  };
  const auto [red_id, red_view] = image(0u);
  const auto [green_id, green_view] = image(1u);
  const auto [unused_id, unused_view] = image(2u);
  (void)unused_view;
  const auto unused_tick = scheduler.CurrentTick();
  TextureCacheTestAccess::FreeImage(cache, unused_id);
  table.OnImageUnregistered(unused_id); // Actual known ID with no shared slot/backrefs.
  Require(name, "no-use/no-ref avoids wait", scheduler.CurrentTick() == unused_tick,
          "retiring an actual unreferenced image submitted unrelated fresh-global work");
  vk::SamplerCreateInfo sampler_info{};
  sampler_info.magFilter = sampler_info.minFilter = vk::Filter::eNearest;
  sampler_info.mipmapMode = vk::SamplerMipmapMode::eNearest;
  sampler_info.addressModeU = sampler_info.addressModeV = sampler_info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
  sampler_info.maxAnisotropy = 1.0f;
  vk::Sampler sampler = nullptr;
  RequireVulkanSuccess(vulkan.Device().createSampler(&sampler_info, nullptr, &sampler), "neighbor real sampler");
  table.WriteDefaultSampler(sampler);
  const auto attach = [&](ImageId id, vk::ImageView view, uint64_t heap_base, BindlessTable::Heap *existing = nullptr) {
    const auto slot = table.SharedSlot(BindlessTable::Images2D, id, view, vk::ImageLayout::eShaderReadOnlyOptimal);
    Require(name, "actual shared slot", slot != 0u, "actual shared slot allocation failed");
    auto *heap = existing != nullptr ? existing : table.FindOrCreateHeap(heap_base, 0u, 32u,
        BindlessTable::Images2D, 1u, NeighborImageResource());
    Require(name, "actual heap", heap != nullptr, "actual heap allocation failed");
    heap->valid_keys = 1u;
    heap->slots[0] = slot;
    heap->settled[0] = 1u;
    heap->resolved = {id};
    table.SetTranslation(*heap, 0u, slot);
    table.AddImageReference(id, *heap, 0u);
    cache.GetImage(id).bindless_pinned = true;
    return std::pair{heap, slot};
  };
  constexpr uint64_t red_heap_base = base + 0x8000u, green_heap_base = base + 0x9000u;
  const auto [red_heap, red_slot] = attach(red_id, red_view, red_heap_base);
  const auto [green_heap, green_slot] = attach(green_id, green_view, green_heap_base);
  const auto image_count = table.ImagesPerArray();
  const auto sampler_count = std::min(BindlessTable::MaxSamplers, graphics.bindless_max_samplers);
  constexpr std::array<uint32_t, 4> red{0x3f800000u, 0u, 0u, 0x3f800000u};
  constexpr std::array<uint32_t, 4> green{0u, 0x3f800000u, 0u, 0x3f800000u};
  constexpr std::array<uint32_t, 4> blue{0u, 0u, 0x3f800000u, 0x3f800000u};
  {
    RetirementNeighborReader red_reader(vulkan, DescriptorRetirementShader(image_count, sampler_count,
        red_heap->region, red_slot), red_heap, red_heap_base);
    RetirementNeighborReader green_reader(vulkan, DescriptorRetirementShader(image_count, sampler_count,
        green_heap->region, green_slot), green_heap, green_heap_base);
    red_reader.Record();
    green_reader.Record();
    scheduler.Finish();
    red_reader.RequireColor("batch red positive control", red);
    green_reader.RequireColor("batch green positive control", green);
    red_reader.Record();
    green_reader.Record();
    uint32_t callback_count = 0;
    scheduler.DeferOperation([&callback_count] { ++callback_count; });
    const auto old_tick = scheduler.CurrentTick();
    TextureCacheTestAccess::FreeImage(cache, red_id);
    const auto after_first = scheduler.CurrentTick();
    Require(name, "single real completion wait", after_first == old_tick + 1u && scheduler.IsFree(old_tick) && callback_count == 0u,
            "first invalidation did not finish recorded batch exactly once, or drained general callbacks under preparation/cache lock");
    TextureCacheTestAccess::FreeImage(cache, green_id);
    Require(name, "same-batch repeat avoids wait", scheduler.CurrentTick() == after_first && callback_count == 0u,
            "second invalidation waited again for completed batch or drained general callbacks");
    red_reader.RequireColor("recorded batch retained red", red);
    green_reader.RequireColor("recorded batch retained green", green);
    const auto [replacement_id, replacement_view] = image(3u);
    const auto [replacement_heap, replacement_slot] = attach(replacement_id, replacement_view, red_heap_base, red_heap);
    RetirementNeighborReader replacement(vulkan, DescriptorRetirementShader(image_count, sampler_count,
        replacement_heap->region, replacement_slot), replacement_heap, red_heap_base);
    replacement.Record();
    const auto replacement_tick = scheduler.CurrentTick();
    table.OnImageUnregistered(red_id); // Old ID has no remaining refs; never clears its replacement.
    Require(name, "old ID repeat leaves fresh consumer", scheduler.CurrentTick() == replacement_tick &&
                BindlessLifecycleTestAccess::Translation(table, replacement_heap->region, 0u) == replacement_slot,
            "old retirement cleared or waited on fresh replacement ownership");
    scheduler.Finish();
    replacement.RequireColor("fresh replacement sampled green", green);
    Require(name, "callback normal boundary", callback_count == 1u,
            "general callback did not run at a normal completed-operation boundary");
    scheduler.Finish(); // Complete both old physical image-view deletion callbacks.
    Require(name, "old physical image retirement", cache.PeekImage(red_id) == nullptr && cache.PeekImage(green_id) == nullptr,
            "actual old physical image deletion callbacks did not complete");
    replacement.Record();
    scheduler.Finish();
    replacement.RequireColor("replacement survives old deletion callbacks", green);
    const auto completed_tick = scheduler.CurrentTick();
    TextureCacheTestAccess::FreeImage(cache, replacement_id);
    Require(name, "already completed use avoids wait", scheduler.CurrentTick() == completed_tick,
            "completed last use caused an unnecessary new submission/wait");
    scheduler.Finish();
    scheduler.Finish();
  }
  {
    const auto [fixed_id, fixed_view] = image(4u);
    const auto fixed_slot = table.SharedSlot(BindlessTable::Images2D, fixed_id, fixed_view,
                                           vk::ImageLayout::eShaderReadOnlyOptimal);
    Require(name, "fixed-only actual slot", fixed_slot != 0u, "fixed-only shared slot allocation failed");
    cache.GetImage(fixed_id).bindless_pinned = true;
    RetirementNeighborReader fixed(vulkan, FixedSlotRetirementShader(image_count, sampler_count, fixed_slot), nullptr, 0u);
    fixed.Record();
    scheduler.Finish();
    fixed.RequireColor("fixed-only blue positive control", blue);
    fixed.Record(); // No translation shader resource, heap lookup, or region lease.
    const auto fixed_tick = scheduler.CurrentTick();
    TextureCacheTestAccess::FreeImage(cache, fixed_id);
    Require(name, "fixed-only recorded use completion", scheduler.CurrentTick() == fixed_tick + 1u && scheduler.IsFree(fixed_tick),
            "whole bindless-set recorded use was lost when there were no region leases");
    fixed.RequireColor("fixed-only recorded blue retained", blue);
    scheduler.Finish();
    scheduler.Finish();
  }
  vulkan.Device().destroySampler(sampler, nullptr);
  context.UnmapMemory(base, bytes);
  LibKernel::Memory::InstallGpuResources(nullptr);
  Require(name, "actual unmap", LibKernel::Memory::KernelMunmap(base, bytes) == 0, "actual guest unmap failed");
  Require(name, "actual direct allocation cleanup", LibKernel::Memory::KernelReleaseDirectMemory(offset, bytes) == 0,
          "actual direct allocation cleanup failed");
  std::printf("[gpu] %-32s ok (real red/green/blue pixels; batch/no-ref/completed/replacement/fixed-only; no gates)\n", name);
}
} // namespace
} // namespace Libs::Graphics
