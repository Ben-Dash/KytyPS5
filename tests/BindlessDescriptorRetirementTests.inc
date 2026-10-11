// Synthetic native sampling readback for descriptor/translation retirement.
// Contract: a recorded guest consumer retains its original image until its use
// completes. Vulkan UPDATE_AFTER_BIND observes newer descriptors before submit;
// that host permission does not preserve an earlier translated guest use.
// https://docs.vulkan.org/refpages/latest/refpages/source/VkDescriptorBindingFlagBits.html
// https://docs.vulkan.org/spec/latest/chapters/descriptorsets.html#descriptorsets-binding
#include "graphics/shader/recompiler/backend/spirv/SpirvBuilder.h"
#include "spirv-tools/libspirv.hpp"

namespace Libs::Graphics {
namespace {

std::vector<uint32_t> DescriptorRetirementShader(uint32_t images, uint32_t samplers,
                                                uint32_t region, uint32_t fixed_slot) {
  using ShaderRecompiler::Spirv::Builder;
  Builder builder(0x00010500u);
  builder.RequireCapability(spv::CapabilityShader);
  builder.RequireCapability(spv::CapabilityShaderNonUniform);
  builder.RequireCapability(spv::CapabilitySampledImageArrayNonUniformIndexing);
  builder.RequireExtension("SPV_EXT_descriptor_indexing");
  builder.AddMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  const auto void_type = builder.Type(spv::OpTypeVoid);
  const auto bool_type = builder.Type(spv::OpTypeBool);
  const auto uint_type = builder.Type(spv::OpTypeInt, 32u, 0u);
  const auto float_type = builder.Type(spv::OpTypeFloat, 32u);
  const auto vec2 = builder.Type(spv::OpTypeVector, float_type, 2u);
  const auto vec4 = builder.Type(spv::OpTypeVector, float_type, 4u);
  const auto uint_constant = [&](uint32_t value) {
    return builder.Constant(spv::OpConstant, uint_type, value);
  };
  const auto zero = uint_constant(0u), one = uint_constant(1u), two = uint_constant(2u);
  const auto fzero = builder.Constant(spv::OpConstant, float_type, std::bit_cast<uint32_t>(0.0f));
  const auto fhalf = builder.Constant(spv::OpConstant, float_type, std::bit_cast<uint32_t>(0.5f));
  const auto coord = builder.Constant(spv::OpConstantComposite, vec2, fhalf, fhalf);
  const auto output_array = builder.Type(spv::OpTypeArray, vec4, two);
  builder.AddAnnotation(spv::OpDecorate, output_array, spv::DecorationArrayStride, 16u);
  const auto output_block = builder.Type(spv::OpTypeStruct, output_array);
  builder.AddAnnotation(spv::OpDecorate, output_block, spv::DecorationBlock);
  builder.AddAnnotation(spv::OpMemberDecorate, output_block, 0u, spv::DecorationOffset, 0u);
  const auto output_pointer = builder.Type(spv::OpTypePointer, spv::StorageClassStorageBuffer, output_block);
  const auto output_element_pointer = builder.Type(spv::OpTypePointer, spv::StorageClassStorageBuffer, vec4);
  const auto output = builder.DefineGlobalVariable(output_pointer, spv::StorageClassStorageBuffer);
  builder.AddAnnotation(spv::OpDecorate, output, spv::DecorationDescriptorSet, 0u);
  builder.AddAnnotation(spv::OpDecorate, output, spv::DecorationBinding, 0u);

  const auto words = builder.Type(spv::OpTypeRuntimeArray, uint_type);
  builder.AddAnnotation(spv::OpDecorate, words, spv::DecorationArrayStride, 4u);
  const auto translation_block = builder.Type(spv::OpTypeStruct, words);
  builder.AddAnnotation(spv::OpDecorate, translation_block, spv::DecorationBlock);
  builder.AddAnnotation(spv::OpMemberDecorate, translation_block, 0u, spv::DecorationOffset, 0u);
  const auto translation_pointer = builder.Type(spv::OpTypePointer,
      spv::StorageClassStorageBuffer, translation_block);
  const auto uint_pointer = builder.Type(spv::OpTypePointer, spv::StorageClassStorageBuffer, uint_type);
  const auto translation = builder.DefineGlobalVariable(translation_pointer, spv::StorageClassStorageBuffer);
  builder.AddAnnotation(spv::OpDecorate, translation, spv::DecorationDescriptorSet, 2u);
  builder.AddAnnotation(spv::OpDecorate, translation, spv::DecorationBinding, BindlessTable::Translation);
  builder.AddAnnotation(spv::OpDecorate, translation, spv::DecorationNonWritable);
  const auto image_type = builder.Type(spv::OpTypeImage, float_type, spv::Dim2D,
                                       0u, 0u, 0u, 1u, spv::ImageFormatUnknown);
  const auto image_array = builder.Type(spv::OpTypeArray, image_type, uint_constant(images));
  const auto image_array_pointer = builder.Type(spv::OpTypePointer,
      spv::StorageClassUniformConstant, image_array);
  const auto image_pointer = builder.Type(spv::OpTypePointer, spv::StorageClassUniformConstant, image_type);
  const auto image_variable = builder.DefineGlobalVariable(image_array_pointer, spv::StorageClassUniformConstant);
  builder.AddAnnotation(spv::OpDecorate, image_variable, spv::DecorationDescriptorSet, 2u);
  builder.AddAnnotation(spv::OpDecorate, image_variable, spv::DecorationBinding, BindlessTable::Images2D);
  const auto sampler_type = builder.Type(spv::OpTypeSampler);
  const auto sampler_array = builder.Type(spv::OpTypeArray, sampler_type, uint_constant(samplers));
  const auto sampler_array_pointer = builder.Type(spv::OpTypePointer,
      spv::StorageClassUniformConstant, sampler_array);
  const auto sampler_pointer = builder.Type(spv::OpTypePointer, spv::StorageClassUniformConstant, sampler_type);
  const auto sampler_variable = builder.DefineGlobalVariable(sampler_array_pointer, spv::StorageClassUniformConstant);
  builder.AddAnnotation(spv::OpDecorate, sampler_variable, spv::DecorationDescriptorSet, 2u);
  builder.AddAnnotation(spv::OpDecorate, sampler_variable, spv::DecorationBinding, BindlessTable::Samplers);
  const auto sampled_image_type = builder.Type(spv::OpTypeSampledImage, image_type);
  const auto function_type = builder.Type(spv::OpTypeFunction, void_type);
  const auto entry = builder.AllocateId();
  builder.AddEntryPoint(spv::ExecutionModelGLCompute, entry, "main",
                        {output, translation, image_variable, sampler_variable});
  builder.AddExecutionMode(entry, spv::ExecutionModeLocalSize, 1u, 1u, 1u);
  builder.AddFunction(spv::OpFunction, void_type, entry, spv::FunctionControlMaskNone, function_type);
  builder.AddFunction(spv::OpLabel, builder.AllocateId());
  const auto translation_address = builder.AllocateId(), raw_slot = builder.AllocateId();
  builder.AddFunction(spv::OpAccessChain, uint_pointer, translation_address,
                      translation, zero, uint_constant(region));
  builder.AddFunction(spv::OpLoad, uint_type, raw_slot, translation_address);
  const auto pending = builder.AllocateId(), selected_slot = builder.AllocateId();
  builder.AddFunction(spv::OpIEqual, bool_type, pending, raw_slot,
                      uint_constant(ShaderRecompiler::IR::BindlessPending));
  // Match the existing supported pending-key policy, without reading invalid index UINT32_MAX.
  builder.AddFunction(spv::OpSelect, uint_type, selected_slot, pending, two, raw_slot);
  const auto sampler_address = builder.AllocateId(), sampler = builder.AllocateId();
  builder.AddFunction(spv::OpAccessChain, sampler_pointer, sampler_address, sampler_variable, zero);
  builder.AddFunction(spv::OpLoad, sampler_type, sampler, sampler_address);
  const auto sample = [&](uint32_t slot, uint32_t result_index) {
    const auto address = builder.AllocateId(), image = builder.AllocateId(),
               combined = builder.AllocateId(), color = builder.AllocateId(), destination = builder.AllocateId();
    builder.AddAnnotation(spv::OpDecorate, slot, spv::DecorationNonUniform);
    builder.AddAnnotation(spv::OpDecorate, address, spv::DecorationNonUniform);
    builder.AddAnnotation(spv::OpDecorate, image, spv::DecorationNonUniform);
    builder.AddAnnotation(spv::OpDecorate, combined, spv::DecorationNonUniform);
    builder.AddFunction(spv::OpAccessChain, image_pointer, address, image_variable, slot);
    builder.AddFunction(spv::OpLoad, image_type, image, address);
    builder.AddFunction(spv::OpSampledImage, sampled_image_type, combined, image, sampler);
    builder.AddFunction(spv::OpImageSampleExplicitLod, vec4, color, combined,
                        coord, spv::ImageOperandsLodMask, fzero);
    builder.AddFunction(spv::OpAccessChain, output_element_pointer, destination,
                        output, zero, result_index);
    builder.AddFunction(spv::OpStore, destination, color);
  };
  sample(selected_slot, zero);
  sample(uint_constant(fixed_slot), one);
  builder.AddFunction(spv::OpReturn);
  builder.AddFunction(spv::OpFunctionEnd);
  return builder.Build();
}

void CheckBindlessDescriptorRetirementReadback(VulkanHarness &vulkan) {
  constexpr const char *name = "BindlessRecordedConsumerRetirementReadback";
  constexpr uint64_t base = 0x0000000200600000ull, bytes = 0x10000u, alignment = 0x10000u;
  int64_t direct_offset = -1;
  Require(name, "tracked guest allocation", LibKernel::Memory::KernelAllocateDirectMemory(
      0, LibKernel::Memory::KernelGetDirectMemorySize(), bytes, alignment, 0, &direct_offset) == 0,
      "actual guest direct-memory allocation failed; not a semantic RED");
  void *mapped = reinterpret_cast<void *>(base);
  Require(name, "tracked guest map", LibKernel::Memory::KernelMapDirectMemory(
      &mapped, bytes, 0x3, 0x10, direct_offset, alignment) == 0 && mapped == reinterpret_cast<void *>(base),
      "tracked guest map failed; not a semantic RED");
  // Independent pixel oracle: RGBA8 bytes {255,0,0,255} are exactly float {1,0,0,1}.
  constexpr uint32_t red_texel = 0xff0000ffu;
  std::memcpy(mapped, &red_texel, sizeof(red_texel));
  uint32_t backed = 0;
  Require(name, "actual red guest backing", LibKernel::Memory::TryReadBacking(base, &backed, sizeof(backed)) &&
              backed == red_texel, "actual tracked backing did not contain the synthetic red texel");
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
  TextureCache::ImageDesc desc{};
  desc.type = TextureCache::BindingType::Texture;
  desc.info.data = {base, sizeof(red_texel)};
  desc.info.pixel_format = vk::Format::eR8G8B8A8Unorm;
  desc.info.guest_format = Prospero::BufferFormat::k8_8_8_8UNorm;
  desc.info.type = Prospero::ImageType::kColor2D;
  desc.info.extent = {1u, 1u, 1u};
  desc.info.resources = {1u, 1u};
  desc.info.pitch = desc.info.samples = 1u;
  desc.info.bytes_per_block = sizeof(red_texel);
  desc.info.tile_mode = Prospero::TileMode::kLinear;
  desc.info.mip_layout[0] = {0u, sizeof(red_texel), 1u, 1u};
  desc.view_info.format = desc.info.pixel_format;
  desc.view_info.type = vk::ImageViewType::e2D;
  desc.view_info.aspect = vk::ImageAspectFlagBits::eColor;
  desc.view_info.usage = vk::ImageUsageFlagBits::eSampled;
  desc.view_info.level_count = desc.view_info.layer_count = 1u;
  const auto image_id = cache.FindImage(desc);
  const auto view = cache.FindTexture(image_id, desc);
  Require(name, "actual native red image", cache.PeekImage(image_id) != nullptr &&
              cache.PeekImage(image_id)->registered && view != nullptr,
          "native cache red image/view setup failed; not a semantic RED");
  cache.GetImage(image_id).Transit(vk::ImageLayout::eShaderReadOnlyOptimal,
      vk::AccessFlagBits2::eShaderRead, {}, scheduler.Current().Handle());
  const auto slot = table.SharedSlot(BindlessTable::Images2D, image_id, view,
                                   vk::ImageLayout::eShaderReadOnlyOptimal);
  const auto resource = NeighborImageResource();
  auto *heap = table.FindOrCreateHeap(base + 0x8000u, 0u, 32u,
                                     BindlessTable::Images2D, 1u, resource);
  Require(name, "actual slot and heap", slot != 0u && heap != nullptr,
          "actual shared slot/heap allocation failed; not a semantic RED");
  heap->valid_keys = 1u;
  heap->slots[0] = slot;
  heap->settled[0] = heap->listed[0] = 1u;
  heap->settled_keys.push_back(0u);
  heap->resolved.push_back(image_id);
  table.SetTranslation(*heap, 0u, slot);
  table.AddImageReference(image_id, *heap, 0u);
  cache.GetImage(image_id).bindless_pinned = true;
  const auto region = heap->region;

  vk::SamplerCreateInfo sampler_info{};
  sampler_info.magFilter = sampler_info.minFilter = vk::Filter::eNearest;
  sampler_info.mipmapMode = vk::SamplerMipmapMode::eNearest;
  sampler_info.addressModeU = sampler_info.addressModeV = sampler_info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
  sampler_info.maxAnisotropy = 1.0f;
  vk::Sampler sampler = nullptr;
  RequireVulkanSuccess(vulkan.Device().createSampler(&sampler_info, nullptr, &sampler),
                       "native retirement sampler");
  table.WriteDefaultSampler(sampler);
  const auto spirv = DescriptorRetirementShader(table.ImagesPerArray(),
      std::min(BindlessTable::MaxSamplers, graphics.bindless_max_samplers), region, slot);
  spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_3);
  validator.SetMessageConsumer([](spv_message_level_t, const char *, const spv_position_t &position,
                                  const char *message) {
    std::fprintf(stderr, "DescriptorRetirement SPIR-V word%zu: %s\n", position.index, message);
  });
  Require(name, "synthetic bounded SPIR-V", validator.Validate(spirv),
          "synthetic shader failed Vulkan SPIR-V validation; not a semantic RED");
  vk::ShaderModuleCreateInfo module_info{};
  module_info.codeSize = spirv.size() * sizeof(uint32_t);
  module_info.pCode = spirv.data();
  vk::ShaderModule module = nullptr;
  RequireVulkanSuccess(vulkan.Device().createShaderModule(&module_info, nullptr, &module),
                       "native retirement shader module");

  vk::BufferCreateInfo output_info{};
  output_info.size = 8u * sizeof(uint32_t);
  output_info.usage = vk::BufferUsageFlagBits::eStorageBuffer;
  VmaAllocationCreateInfo output_memory{};
  output_memory.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
  output_memory.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
  output_memory.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
  VkBuffer native_output = VK_NULL_HANDLE;
  VmaAllocation output_allocation = nullptr;
  VmaAllocationInfo output_mapping{};
  RequireVulkanSuccess(static_cast<vk::Result>(vmaCreateBuffer(graphics.allocator,
      reinterpret_cast<const VkBufferCreateInfo *>(&output_info), &output_memory,
      &native_output, &output_allocation, &output_mapping)), "real retirement output allocation");
  Require(name, "real mapped readback", output_mapping.pMappedData != nullptr,
          "shader output is not actually mapped; not a semantic RED");
  const vk::Buffer output = native_output;
  const vk::DescriptorSetLayoutBinding output_binding{0u, vk::DescriptorType::eStorageBuffer,
      1u, vk::ShaderStageFlagBits::eCompute, nullptr};
  vk::DescriptorSetLayoutCreateInfo output_layout_info{};
  output_layout_info.bindingCount = 1u;
  output_layout_info.pBindings = &output_binding;
  std::array<vk::DescriptorSetLayout, 3> layouts{nullptr, nullptr, table.Layout()};
  RequireVulkanSuccess(vulkan.Device().createDescriptorSetLayout(&output_layout_info, nullptr, &layouts[0]),
                       "retirement output descriptor layout");
  vk::DescriptorSetLayoutCreateInfo empty_layout{};
  RequireVulkanSuccess(vulkan.Device().createDescriptorSetLayout(&empty_layout, nullptr, &layouts[1]),
                       "retirement intermediate descriptor layout");
  const vk::DescriptorPoolSize pool_size{vk::DescriptorType::eStorageBuffer, 1u};
  vk::DescriptorPoolCreateInfo pool_info{};
  pool_info.maxSets = pool_info.poolSizeCount = 1u;
  pool_info.pPoolSizes = &pool_size;
  vk::DescriptorPool pool = nullptr;
  RequireVulkanSuccess(vulkan.Device().createDescriptorPool(&pool_info, nullptr, &pool),
                       "retirement output descriptor pool");
  vk::DescriptorSetAllocateInfo set_info{};
  set_info.descriptorPool = pool;
  set_info.descriptorSetCount = 1u;
  set_info.pSetLayouts = &layouts[0];
  vk::DescriptorSet output_set = nullptr;
  RequireVulkanSuccess(vulkan.Device().allocateDescriptorSets(&set_info, &output_set),
                       "retirement output descriptor set");
  const vk::DescriptorBufferInfo output_descriptor{output, 0u, output_info.size};
  vk::WriteDescriptorSet output_write{};
  output_write.dstSet = output_set;
  output_write.dstBinding = 0u;
  output_write.descriptorCount = 1u;
  output_write.descriptorType = vk::DescriptorType::eStorageBuffer;
  output_write.pBufferInfo = &output_descriptor;
  vulkan.Device().updateDescriptorSets(1u, &output_write, 0u, nullptr);
  vk::PipelineLayoutCreateInfo pipeline_layout_info{};
  pipeline_layout_info.setLayoutCount = static_cast<uint32_t>(layouts.size());
  pipeline_layout_info.pSetLayouts = layouts.data();
  vk::PipelineLayout pipeline_layout = nullptr;
  RequireVulkanSuccess(vulkan.Device().createPipelineLayout(&pipeline_layout_info, nullptr, &pipeline_layout),
                       "retirement sampling pipeline layout");
  vk::ComputePipelineCreateInfo pipeline_info{};
  pipeline_info.layout = pipeline_layout;
  pipeline_info.stage.stage = vk::ShaderStageFlagBits::eCompute;
  pipeline_info.stage.module = module;
  pipeline_info.stage.pName = "main";
  vk::Pipeline pipeline = nullptr;
  RequireVulkanSuccess(vulkan.Device().createComputePipelines(nullptr, 1u, &pipeline_info, nullptr, &pipeline),
                       "bounded retirement sampling pipeline");

  const auto record = [&] {
    std::array<uint32_t, 8> sentinel;
    sentinel.fill(0xdeadbeefu);
    std::memcpy(output_mapping.pMappedData, sentinel.data(), output_info.size);
    RequireVulkanSuccess(static_cast<vk::Result>(vmaFlushAllocation(graphics.allocator,
        output_allocation, 0u, output_info.size)), "retirement output sentinel flush");
    BindlessTable::PreparationScope operation(table);
    (void)table.FindOrCreateHeap(base + 0x8000u, 0u, 32u,
                                BindlessTable::Images2D, 1u, resource);
    auto command = scheduler.Current().Handle();
    vk::BufferMemoryBarrier output_ready{};
    output_ready.srcAccessMask = vk::AccessFlagBits::eHostWrite;
    output_ready.dstAccessMask = vk::AccessFlagBits::eShaderWrite;
    output_ready.srcQueueFamilyIndex = output_ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    output_ready.buffer = output;
    output_ready.size = output_info.size;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eHost, vk::PipelineStageFlagBits::eComputeShader,
                            {}, 0u, nullptr, 1u, &output_ready, 0u, nullptr);
    command.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
    command.bindDescriptorSets(vk::PipelineBindPoint::eCompute, pipeline_layout, 0u,
                               1u, &output_set, 0u, nullptr);
    const auto bindless_set = table.Set();
    command.bindDescriptorSets(vk::PipelineBindPoint::eCompute, pipeline_layout, 2u,
                               1u, &bindless_set, 0u, nullptr);
    table.MarkPreparationUsed(scheduler.CurrentTick());
    command.dispatch(1u, 1u, 1u);
    auto output_done = output_ready;
    output_done.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
    output_done.dstAccessMask = vk::AccessFlagBits::eHostRead;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eHost,
                            {}, 0u, nullptr, 1u, &output_done, 0u, nullptr);
  };
  const auto readback = [&] {
    RequireVulkanSuccess(static_cast<vk::Result>(vmaInvalidateAllocation(graphics.allocator,
        output_allocation, 0u, output_info.size)), "real sampled retirement readback invalidate");
    std::array<uint32_t, 8> words{};
    std::memcpy(words.data(), output_mapping.pMappedData, output_info.size);
    return words;
  };
  const std::array<uint32_t, 8> expected{0x3f800000u, 0u, 0u, 0x3f800000u,
                                       0x3f800000u, 0u, 0u, 0x3f800000u};
  record();
  scheduler.Finish();
  Require(name, "actual sampled positive control", readback() == expected,
          "real translation-selected/fixed-slot sampling did not read red before invalidation; not a semantic RED");
  std::printf("[gpu] positive control sampled original opaque red through translation and fixed slot\n");
  record();
  const auto recorded_tick = scheduler.CurrentTick();
  Require(name, "real recorded-unsubmitted consumer", !scheduler.IsFree(recorded_tick) &&
              cache.PeekImage(image_id) != nullptr && cache.PeekImage(image_id)->registered,
          "fixture did not retain a genuine recorded unsubmitted native consumer");
  // Actual native cache retirement invokes actual OnImageUnregistered. The image
  // object/view has its existing double-deferred lifetime; this tests the earlier
  // descriptor and mapped-translation mutation, not a dangling Vulkan handle.
  TextureCacheTestAccess::FreeImage(cache, image_id);
  Require(name, "actual deferred native retirement", cache.PeekImage(image_id) != nullptr &&
              !cache.PeekImage(image_id)->registered,
          "native cache retirement did not reach OnImageUnregistered before old consumer submission");
  scheduler.Finish();
  const auto actual = readback();
  std::printf("[gpu] retirement readback tick=%llu region=%u slot=%u translation_sample=%08x,%08x,%08x,%08x fixed_slot_sample=%08x,%08x,%08x,%08x\n",
              static_cast<unsigned long long>(recorded_tick), region, slot,
              actual[0], actual[1], actual[2], actual[3], actual[4], actual[5], actual[6], actual[7]);
  Require(name, "recorded consumer retains original sampled resource", actual == expected,
          "OnImageUnregistered replaced descriptor/translation used by a recorded earlier guest consumer before it executed");
  scheduler.Finish();
  vulkan.Device().destroyPipeline(pipeline, nullptr);
  vulkan.Device().destroyPipelineLayout(pipeline_layout, nullptr);
  vulkan.Device().destroyDescriptorPool(pool, nullptr);
  vulkan.Device().destroyDescriptorSetLayout(layouts[1], nullptr);
  vulkan.Device().destroyDescriptorSetLayout(layouts[0], nullptr);
  vulkan.Device().destroyShaderModule(module, nullptr);
  vulkan.Device().destroySampler(sampler, nullptr);
  vmaDestroyBuffer(graphics.allocator, native_output, output_allocation);
  context.UnmapMemory(base, bytes);
  LibKernel::Memory::InstallGpuResources(nullptr);
  Require(name, "actual guest unmap", LibKernel::Memory::KernelMunmap(base, bytes) == 0,
          "tracked guest unmap failed");
  Require(name, "actual direct memory cleanup", LibKernel::Memory::KernelReleaseDirectMemory(direct_offset, bytes) == 0,
          "direct-memory cleanup failed");
  std::printf("[gpu] %-32s ok (two bounded sampled dispatches; real readback; no GPU gate)\n", name);
}
} // namespace
} // namespace Libs::Graphics
