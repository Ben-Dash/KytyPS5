// Public guest compiler and bounded native readback for packed buffer prefixes.
// Each byte is a host binding prefix, independent of the guest SRD address.
// The code budget counts actual shader-data DWORD reads, not helper syntax.
#include <map>

namespace Libs::Graphics {
namespace {
constexpr std::array<uint32_t, 5> PackedOffsetPrefixes{0u, 12u, 20u, 28u, 36u};
constexpr uint32_t PackedOffsetGuestBytes = 8u;
constexpr uint8_t PackedOffsetSentinel = 0x5au;

uint64_t PackedOffsetAddress(uint32_t index) {
  return FetchGpuGuest::Base + uint64_t{index + 1u} * 0x1000u;
}
uint32_t PackedOffsetValue(uint32_t index) { return 0x12345678u + index; }

struct PackedOffsetCompilation {
  ShaderComputeInputInfo compute{};
  std::vector<uint32_t> code;
  FetchGpuIR::ResourcePlan plan;
  FetchGpuIR::ResourceSnapshot resources;
  FetchGpuIR::ResourceSpecialization specialization;
  ShaderRecompiler::CompileResult compiled;
  uint32_t descriptors = 0;
  bool storage = false;

  PackedOffsetCompilation(uint32_t count, bool use_storage) : descriptors(count), storage(use_storage) {
    constexpr const char* name = "PackedBufferOffsets";
    Require(name, "bounded fixture", count <= PackedOffsetPrefixes.size(),
            "fixture has more than five real descriptors; not a cost RED");
    for (uint32_t index = 0; index < count; ++index) {
      const auto descriptor = FetchGpuDescriptor(PackedOffsetAddress(index), 0u, PackedOffsetGuestBytes);
      for (uint32_t word = 0; word < 4u; ++word) {
        code.push_back(0xbe8003ffu | ((index * 4u + word) << 16u)); // S_MOV_B32 literal
        code.push_back(descriptor[word]);
      }
      code.push_back((0x3fu << 25u) | (1u << 9u) | 255u); // V_MOV_B32 v0,literal
      code.push_back(PackedOffsetValue(index));
      code.push_back((0x38u << 26u) | (0x1cu << 18u)); // BUFFER_STORE_DWORD, no index/offset
      code.push_back((128u << 24u) | (index << 16u)); // srd=s[index*4:index*4+3]
    }
    code.push_back(0xbf810000u);
    compute.threads_num[0] = compute.threads_num[1] = compute.threads_num[2] = 1u;
    compute.dispatch_threads_num[0] = compute.dispatch_threads_num[1] =
        compute.dispatch_threads_num[2] = 1u;
    compute.host_subgroup_size = compute.wave_size = 32u;
    ShaderRecompiler::CompileOptions options;
    options.stage = ShaderType::Compute;
    options.wave_size = 32u;
    options.input_info.compute = &compute;
    options.dump_ir = false;
    auto translated = ShaderRecompiler::TranslateProgram(code, options);
    Require(name, "guest translation", !translated.skip_dispatch,
            "synthetic ISA did not translate; not the intended cost RED");
    plan = FetchGpuIR::ExtractResourcePlan(translated.program);
    const FetchGpuIR::SrtRuntime runtime{
        .shader_base = reinterpret_cast<uint64_t>(code.data()),
        .read_memory = ReadFetchGpuGuest, .read_specialization_memory = ReadFetchGpuGuest};
    Require(name, "public materialization",
            FetchGpuIR::MaterializeResources(plan, runtime, resources, specialization),
            "literal SRDs did not materialize; not the intended cost RED");
    compiled = ShaderRecompiler::CompileProgram(std::move(translated), options, specialization,
        storage ? FetchGpuIR::PushData::DwordCount : 0u);
    spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_3);
    validator.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t& pos,
                                    const char* message) {
      std::fprintf(stderr, "PackedBufferOffsets SPIR-V word%zu: %s\n", pos.index, message);
    });
    Require(name, "real SPIR-V validation", validator.Validate(compiled.spirv),
            "public compiler emitted an invalid module; not the intended cost RED");
    const auto& layout = compiled.program.bindings;
    Require(name, "actual offset count", layout.memory_offset_count == count && !layout.table,
            "fixture did not exercise its actual ordinary buffer offsets");
    if (count != 0u) {
      Require(name, "public shader-data placement", layout.UsesPushData() != storage &&
                  (FetchGpuIR::FindBinding(layout, FetchGpuIR::DescriptorBindingKind::ShaderData) != nullptr)
                      == storage,
              "public push cursor did not select the requested storage fallback");
    }
    uint32_t writes = 0u;
    for (const auto* block : compiled.program.blocks) for (const auto& inst : *block)
      if (FetchGpuIR::BufferAccessOf(inst.GetOpcode()) == FetchGpuIR::BufferAccess::Write)
        writes += FetchGpuIR::BufferComponentCount(inst.GetOpcode());
    Require(name, "all real stores remain live", writes == count,
            "compiler lost observable guest writes; not a cost RED");
  }

  std::vector<uint32_t> ShaderData() const {
    const auto& layout = compiled.program.bindings;
    std::vector<uint32_t> words(layout.ShaderDataDwords(), 0u);
    // Unused byte lanes of the last packed word must not create extra resources.
    for (uint32_t word = 0; word < (descriptors + 3u) / 4u; ++word)
      words.at(layout.memory_offset_dword + word) = 0xbebebebeu;
    for (uint32_t index = 0; index < descriptors; ++index) {
      const auto& descriptor = resources.buffers.at(index).dwords;
      const uint64_t address = descriptor[0] | (uint64_t{descriptor[1] & 0xffffu} << 32u);
      uint32_t original = 0u;
      while (original < descriptors && PackedOffsetAddress(original) != address) ++original;
      Require("PackedBufferOffsets", "actual resource identity", original < descriptors,
              "materialized descriptor has no fixture owner");
      auto& word = words.at(layout.memory_offset_dword + index / 4u);
      const uint32_t shift = (index % 4u) * 8u;
      word = (word & ~(0xffu << shift)) | (PackedOffsetPrefixes[original] << shift);
    }
    return words;
  }
};

uint32_t CountPackedOffsetDwordReads(const PackedOffsetCompilation& fixture) {
  const auto& words = fixture.compiled.spirv;
  const auto& layout = fixture.compiled.program.bindings;
  struct Pointer { uint32_t variable = 0u, dword = UINT32_MAX; };
  std::map<uint32_t, uint32_t> constants, variables, bindings;
  std::map<uint32_t, Pointer> pointers;
  uint32_t u32_type = 0u;
  for (size_t p = 5u; p < words.size();) {
    const uint32_t count = words[p] >> 16u;
    const auto op = static_cast<spv::Op>(words[p] & 0xffffu);
    Require("PackedBufferOffsets", "SPIR-V framing", count != 0u && count <= words.size() - p,
            "module framing invalid; not a cost RED");
    if (op == spv::OpTypeInt && count == 4u && words[p + 2u] == 32u && words[p + 3u] == 0u)
      u32_type = words[p + 1u];
    if (op == spv::OpConstant && count == 4u) constants[words[p + 2u]] = words[p + 3u];
    if (op == spv::OpVariable && count >= 4u) variables[words[p + 2u]] = words[p + 3u];
    if (op == spv::OpDecorate && count == 4u && words[p + 2u] == spv::DecorationBinding)
      bindings[words[p + 1u]] = words[p + 3u];
    if (op == spv::OpAccessChain && count == 6u && constants.contains(words[p + 5u]))
      pointers[words[p + 2u]] = {words[p + 3u], constants.at(words[p + 5u])};
    p += count;
  }
  uint32_t reads = 0u;
  const uint32_t first = layout.memory_offset_dword +
      (layout.UsesPushData() ? layout.push_data_start_dword : 0u);
  const uint32_t last = first + (layout.memory_offset_count + 3u) / 4u;
  for (size_t p = 5u; p < words.size();) {
    const uint32_t count = words[p] >> 16u;
    if ((words[p] & 0xffffu) == spv::OpLoad && count >= 4u && words[p + 1u] == u32_type) {
      const auto found = pointers.find(words[p + 3u]);
      if (found != pointers.end()) {
        const auto& pointer = found->second;
        const bool push = variables.contains(pointer.variable) &&
            variables.at(pointer.variable) == spv::StorageClassPushConstant;
        const bool storage = variables.contains(pointer.variable) &&
            variables.at(pointer.variable) == spv::StorageClassStorageBuffer &&
            bindings.contains(pointer.variable) && bindings.at(pointer.variable) ==
                FetchGpuIR::NativeBinding(ShaderType::Compute, FetchGpuIR::DescriptorBindingKind::ShaderData);
        if ((layout.UsesPushData() ? push : storage) && pointer.dword >= first && pointer.dword < last)
          ++reads;
      }
    }
    p += count;
  }
  return reads;
}

void ConfigurePackedBufferOffsetTests() {
  ConfigureGenericBufferTests();
  GenericCompilerProfile();
}
void CheckPackedBufferOffsetCodegenCost(VulkanHarness&) {
  std::array<uint32_t, 8> reads{};
  size_t result = 0u;
  for (const bool storage : {false, true}) for (const uint32_t count : {0u, 1u, 4u, 5u}) {
    PackedOffsetCompilation fixture(count, storage);
    fixture.ShaderData();
    reads[result++] = CountPackedOffsetDwordReads(fixture);
    Require("PackedBufferOffsets", "positive reflected load observation",
            count == 0u || reads[result - 1u] != 0u,
            "packed-data load observer found no real live reads; not the intended cost RED");
    std::printf("[packed-offset-cost] descriptors=%u actualOffsetCount=%u storage=%u reads=%u budget=%u words=%zu allStoresLive=1\n",
        count, fixture.compiled.program.bindings.memory_offset_count, storage, reads[result - 1u],
        (count + 3u) / 4u, fixture.compiled.spirv.size());
  }
  result = 0u;
  for (const bool storage : {false, true}) for (const uint32_t count : {0u, 1u, 4u, 5u}) {
    const uint32_t actual = reads[result++];
    Require("PackedBufferOffsets", "shared packed offset load budget", actual <= (count + 3u) / 4u,
            "actual shader data repeats packed DWORD reads instead of sharing each up-to-four byte prefixes");
    (void)storage;
  }
}

void RunPackedOffsetNativeCase(VulkanHarness& vulkan, uint32_t count, bool storage) {
  constexpr const char* name = "PackedBufferOffsetsGpu";
  PackedOffsetCompilation fixture(count, storage);
  const auto& layout = fixture.compiled.program.bindings;
  const auto data = fixture.ShaderData();
  const auto device = vulkan.Device();
  auto& graphics = vulkan.RuntimeContext();
  std::array<std::vector<uint8_t>, 5> actual;
  {
    std::array<std::unique_ptr<FetchGpuBuffer>, 5> outputs;
    for (uint32_t index = 0; index < count; ++index) {
      outputs[index] = std::make_unique<FetchGpuBuffer>(graphics,
          PackedOffsetPrefixes[index] + PackedOffsetGuestBytes, vk::BufferUsageFlagBits::eStorageBuffer);
      std::memset(outputs[index]->data, PackedOffsetSentinel, outputs[index]->size);
      outputs[index]->Flush();
    }
    std::unique_ptr<FetchGpuBuffer> shader_data;
    if (storage) {
      shader_data = std::make_unique<FetchGpuBuffer>(graphics, data.size() * 4u,
          vk::BufferUsageFlagBits::eStorageBuffer);
      std::memcpy(shader_data->data, data.data(), shader_data->size);
      shader_data->Flush();
    }
    std::vector<vk::DescriptorSetLayoutBinding> reflected;
    uint32_t native_count = 0u;
    for (const auto& binding : layout.descriptors) {
      Require(name, "reflected ordinary bindings", binding.kind == FetchGpuIR::DescriptorBindingKind::Buffers ||
                  binding.kind == FetchGpuIR::DescriptorBindingKind::ShaderData,
              "unexpected descriptor kind requires explicit fixture support");
      reflected.push_back({FetchGpuIR::NativeBinding(ShaderType::Compute, binding.kind),
          NativeDescriptorType(binding.kind), NativeDescriptorCount(binding), vk::ShaderStageFlagBits::eCompute, nullptr});
      native_count += NativeDescriptorCount(binding);
    }
    vk::DescriptorSetLayout descriptor_layout = nullptr;
    vk::DescriptorSetLayoutCreateInfo descriptor_info{};
    descriptor_info.bindingCount = static_cast<uint32_t>(reflected.size());
    descriptor_info.pBindings = reflected.data();
    RequireVulkanSuccess(device.createDescriptorSetLayout(&descriptor_info, nullptr, &descriptor_layout), "packed-offset descriptor layout");
    vk::DescriptorPool pool = nullptr;
    vk::DescriptorPoolSize pool_size{vk::DescriptorType::eStorageBuffer, native_count};
    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.maxSets = pool_info.poolSizeCount = 1u;
    pool_info.pPoolSizes = &pool_size;
    RequireVulkanSuccess(device.createDescriptorPool(&pool_info, nullptr, &pool), "packed-offset descriptor pool");
    vk::DescriptorSet set = nullptr;
    vk::DescriptorSetAllocateInfo allocation{};
    allocation.descriptorPool = pool;
    allocation.descriptorSetCount = 1u;
    allocation.pSetLayouts = &descriptor_layout;
    RequireVulkanSuccess(device.allocateDescriptorSets(&allocation, &set), "packed-offset descriptor set");
    for (const auto& binding : layout.descriptors) {
      std::vector<vk::DescriptorBufferInfo> infos;
      if (binding.kind == FetchGpuIR::DescriptorBindingKind::ShaderData) {
        infos.push_back({shader_data->buffer, 0u, shader_data->size});
      } else for (const auto resource : binding.resources) {
        const auto& descriptor = fixture.resources.buffers.at(resource).dwords;
        const uint64_t address = descriptor[0] | (uint64_t{descriptor[1] & 0xffffu} << 32u);
        uint32_t owner = 0u;
        while (owner < count && PackedOffsetAddress(owner) != address) ++owner;
        Require(name, "real descriptor owner", owner < count, "unexpected SRD is not a fallback buffer");
        infos.push_back({outputs[owner]->buffer, 0u, outputs[owner]->size});
      }
      Require(name, "actual descriptor count", infos.size() == NativeDescriptorCount(binding), "reflected native count differs");
      vk::WriteDescriptorSet write{};
      write.dstSet = set;
      write.dstBinding = FetchGpuIR::NativeBinding(ShaderType::Compute, binding.kind);
      write.descriptorType = NativeDescriptorType(binding.kind);
      write.descriptorCount = static_cast<uint32_t>(infos.size());
      write.pBufferInfo = infos.data();
      device.updateDescriptorSets(1u, &write, 0u, nullptr);
    }
    vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0u, FetchGpuIR::NativePushConstantSize};
    vk::PipelineLayout pipeline_layout = nullptr;
    vk::PipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.setLayoutCount = 1u;
    pipeline_layout_info.pSetLayouts = &descriptor_layout;
    pipeline_layout_info.pushConstantRangeCount = layout.UsesPushData() ? 1u : 0u;
    pipeline_layout_info.pPushConstantRanges = layout.UsesPushData() ? &push : nullptr;
    RequireVulkanSuccess(device.createPipelineLayout(&pipeline_layout_info, nullptr, &pipeline_layout), "packed-offset pipeline layout");
    vk::ShaderModule module = nullptr;
    vk::ShaderModuleCreateInfo module_info{};
    module_info.codeSize = fixture.compiled.spirv.size() * 4u;
    module_info.pCode = fixture.compiled.spirv.data();
    RequireVulkanSuccess(device.createShaderModule(&module_info, nullptr, &module), "packed-offset real compute module");
    vk::Pipeline pipeline = nullptr;
    vk::ComputePipelineCreateInfo pipeline_info{};
    pipeline_info.stage.stage = vk::ShaderStageFlagBits::eCompute;
    pipeline_info.stage.module = module;
    pipeline_info.stage.pName = "main";
    pipeline_info.layout = pipeline_layout;
    RequireVulkanSuccess(device.createComputePipelines(nullptr, 1u, &pipeline_info, nullptr, &pipeline), "packed-offset bounded pipeline");
    auto& scheduler = vulkan.RuntimeRenderer().GetCommandScheduler();
    auto command = scheduler.Current().Handle();
    vk::MemoryBarrier ready{};
    ready.srcAccessMask = vk::AccessFlagBits::eHostWrite;
    ready.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eHost, vk::PipelineStageFlagBits::eComputeShader,
        {}, 1u, &ready, 0u, nullptr, 0u, nullptr);
    command.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
    command.bindDescriptorSets(vk::PipelineBindPoint::eCompute, pipeline_layout, 0u, 1u, &set, 0u, nullptr);
    if (layout.UsesPushData()) {
      FetchGpuIR::PushData constants;
      std::copy(data.begin(), data.end(), constants.dwords.begin() + layout.push_data_start_dword);
      command.pushConstants(pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0u, sizeof(constants), constants.dwords.data());
    }
    command.dispatch(1u, 1u, 1u);
    vk::MemoryBarrier done{};
    done.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
    done.dstAccessMask = vk::AccessFlagBits::eHostRead;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eHost,
        {}, 1u, &done, 0u, nullptr, 0u, nullptr);
    scheduler.Finish();
    for (uint32_t index = 0; index < count; ++index) {
      outputs[index]->Invalidate();
      const auto* bytes = static_cast<const uint8_t*>(outputs[index]->data);
      actual[index].assign(bytes, bytes + outputs[index]->size);
    }
    device.destroyPipeline(pipeline, nullptr);
    device.destroyShaderModule(module, nullptr);
    device.destroyPipelineLayout(pipeline_layout, nullptr);
    device.destroyDescriptorPool(pool, nullptr);
    device.destroyDescriptorSetLayout(descriptor_layout, nullptr);
  }
  // Compare only after the completed dispatch and all native fixture resources are released.
  for (uint32_t index = 0; index < count; ++index) {
    std::vector<uint8_t> expected(PackedOffsetPrefixes[index] + PackedOffsetGuestBytes, PackedOffsetSentinel);
    const uint32_t value = PackedOffsetValue(index);
    for (uint32_t byte = 0u; byte < 4u; ++byte)
      expected[PackedOffsetPrefixes[index] + byte] = static_cast<uint8_t>(value >> (byte * 8u));
    std::printf("[packed-offset-gpu] count=%u storage=%u owner=%u prefix=%u bytes=%zu equal=%u\n",
        count, storage, index, PackedOffsetPrefixes[index], actual[index].size(), actual[index] == expected);
    Require(name, "independent complete byte prefix/readback", actual[index] == expected,
            "guest store failed to preserve prefix/suffix bytes or wrote another native offset");
  }
}
void CheckPackedBufferOffsetGpuReadback(VulkanHarness& vulkan) {
  // Bind real scheduler context before Current(); the fixture's direct native
  // dispatch does not pass through the guest command processor's Begin path.
  HW::Context registers{};
  HW::UserConfig user_config{};
  HW::Shader shaders{};
  vulkan.RuntimeRenderer().GetCommandScheduler().Begin(registers, user_config, shaders);
  for (const bool storage : {false, true}) for (const uint32_t count : {1u, 4u, 5u})
    RunPackedOffsetNativeCase(vulkan, count, storage);
}
} // namespace
} // namespace Libs::Graphics
