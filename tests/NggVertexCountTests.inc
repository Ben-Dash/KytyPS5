// Loop-free native NGG entry-state regression. Initial EXEC is captured before
// any guest instruction changes it or reads SGPR3 to reconstruct a mask.
// Vulkan can invoke a vertex more than once, so atomic tickets give every
// invocation a separate record instead of racing on VertexIndex.
namespace Libs::Graphics {
namespace {
constexpr uint32_t NggCountCapacity = 8192u;
constexpr uint32_t NggCountHeaderWords = 4u, NggCountRecordWords = 8u;
constexpr uint32_t NggCountWords = NggCountHeaderWords + NggCountCapacity * NggCountRecordWords;
constexpr uint32_t NggCountSentinel = 0xdeadbeefu, NggCountMarker = 0x13579bdfu;

void ConfigureNggVertexCountTests() {
  Require("NggVertexCount", "pin default production profile",
      _putenv_s("KYTY_NGG_VERTEX_COUNT", "0") == 0 &&
      _putenv_s("KYTY_WAVE_BRANCH_FULL", "0") == 0 &&
      _putenv_s("KYTY_DEBUG_WAVE_HALVES", "1") == 0 &&
      _putenv_s("KYTY_VTX_EXPLODE_CHECK", "0") == 0,
      "could not pin the unfixed default before the first getter; not a semantic RED");
  ShaderRecompiler::SetTableMode(false);
  ShaderRecompiler::SetHardwareStorageBufferBounds(false);
  ShaderRecompiler::SetNativeFloat16(false);
}

std::vector<uint32_t> NggCountGuestCode() {
  const auto mov = [](uint32_t dst, uint32_t src) {
    return (0x3fu << 25u) | (dst << 17u) | (1u << 9u) | src;
  };
  std::vector<uint32_t> code{
      0xbe8e047eu, // FIRST: S_MOV_B64 s[14:15], EXEC. It cannot depend on SGPR3.
      0xbe8c0303u, // S_MOV_B32 s12,s3, preserve the complete entry ABI word.
      mov(0u, 12u), mov(1u, 14u), mov(2u, 15u), mov(3u, 256u + 5u),
      mov(4u, 255u), NggCountMarker,
      mov(8u, 129u), // One bounded atomic increment, return old ticket in v8.
      (0x38u << 26u) | (0x32u << 18u) | (1u << 14u),
      (128u << 24u) | (2u << 16u) | (8u << 8u),
      (0x1au << 25u) | (30u << 17u) | (8u << 9u) | 133u, // v30=ticket*32
      (0x25u << 25u) | (30u << 17u) | (30u << 9u) | 144u, // +16-byte header
      (0x38u << 26u) | (0x1eu << 18u) | (1u << 12u),
      (128u << 24u) | (2u << 16u) | 30u, // S3,initialEXEClo/hi,VertexIndex
      (0x38u << 26u) | (0x1cu << 18u) | (1u << 12u) | 16u,
      (128u << 24u) | (2u << 16u) | (4u << 8u) | 30u, // Marker; last3words remain sentinels.
      mov(20u, 128u), mov(21u, 128u), mov(22u, 128u),
      mov(23u, 255u), 0x3f800000u,
      (0x3eu << 26u) | (0x0cu << 4u) | 0xfu | (1u << 11u),
      20u | (21u << 8u) | (22u << 16u) | (23u << 24u), 0xbf810000u};
  return code;
}

struct NggCountCompilation {
  ShaderVertexInputInfo vertex{};
  std::array<uint32_t, 4> userdata{};
  std::vector<uint32_t> code = NggCountGuestCode();
  FetchGpuIR::ResourcePlan plan;
  FetchGpuIR::ResourceSnapshot resources;
  FetchGpuIR::ResourceSpecialization specialization;
  ShaderRecompiler::CompileResult compiled;
  uint32_t wave = 0u;
  explicit NggCountCompilation(uint32_t wave_size) : wave(wave_size) {
    constexpr const char* name = "NggVertexCount";
    userdata = FetchGpuDescriptor(FetchGpuGuest::Output, 0u, NggCountWords * 4u);
    vertex.logical_stage = ShaderType::Vertex;
    vertex.wave_size = wave;
    vertex.pa_cl_vs_out_cntl = (1u << 21u) | (1u << 16u);
    ShaderRecompiler::CompileOptions options;
    options.stage = ShaderType::Vertex;
    options.wave_size = wave;
    options.user_data_base = 8u; // Keep initialized NGG system SGPR0..7 untouched.
    options.user_data = userdata;
    options.input_info.vertex = &vertex;
    options.dump_ir = false;
    auto translated = ShaderRecompiler::TranslateProgram(code, options);
    Require(name, "synthetic guest translation", !translated.skip_dispatch,
            "loop-free guest fixture did not translate; not the intended semantic RED");
    plan = FetchGpuIR::ExtractResourcePlan(translated.program);
    const FetchGpuIR::SrtRuntime runtime{
        .user_data = userdata, .shader_base = reinterpret_cast<uint64_t>(code.data()),
        .read_memory = ReadFetchGpuGuest, .read_specialization_memory = ReadFetchGpuGuest};
    Require(name, "real ordinary output materialization",
            FetchGpuIR::MaterializeResources(plan, runtime, resources, specialization),
            "actual output SRD failed materialization; not a semantic RED");
    compiled = ShaderRecompiler::CompileProgram(std::move(translated), options, specialization);
    spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_3);
    validator.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t& p,
                                    const char* message) {
      std::fprintf(stderr, "NggVertexCount SPIR-V word%zu: %s\n", p.index, message);
    });
    Require(name, "validated ordinary native vertex module", validator.Validate(compiled.spirv) &&
                compiled.program.stage == ShaderType::Vertex && compiled.program.wave_size == wave &&
                !compiled.program.bindings.table &&
                ShaderWorkgroupInput(ShaderType::Vertex, options.input_info) == nullptr,
            "must execute the actual ordinary Vertex path, not emulated compute/mesh halves");
    for (size_t word = 5u; word < compiled.spirv.size();) {
      Require(name, "no shader loops", (compiled.spirv[word] & 0xffffu) != spv::OpLoopMerge,
              "fixture unexpectedly contains a native loop; not a bounded regression");
      word += compiled.spirv[word] >> 16u;
    }
    Require(name, "ordinary single buffer", compiled.program.info.buffers.size() == 1u &&
                resources.buffers.size() == 1u && compiled.program.bindings.memory_offset_count == 1u,
            "unexpected resource profile requires explicit fixture support");
  }
  std::vector<uint32_t> ShaderData() const {
    const auto& layout = compiled.program.bindings;
    std::vector<uint32_t> data;
    for (const auto reg : layout.user_data_registers)
      data.push_back(resources.user_data.at(reg - compiled.program.user_data_base));
    data.resize(layout.ShaderDataDwords()); // Output native prefix is genuinely zero.
    for (uint32_t k = 0; k < layout.buffer_strides.size(); ++k)
      data[layout.BufferStrideDword() + k / 2u] |=
          (FetchGpuIR::CanonicalPackedStride(resources.buffers.at(layout.buffer_strides[k])) & 0x3fffu)
              << ((k % 2u) * 16u);
    for (uint32_t k = 0; k < layout.buffer_formats.size(); ++k)
      data[layout.BufferFormatDword() + k] =
          FetchGpuIR::RuntimeBufferSlotWord(resources.buffers.at(layout.buffer_formats[k]));
    return data;
  }
};

struct NggCountObservation {
  uint32_t wave = 0u, vertices = 0u;
  std::vector<uint32_t> words;
};

NggCountObservation RunNggCountNative(VulkanHarness& vulkan, NggCountCompilation& fixture,
                                    uint32_t vertices) {
  constexpr const char* name = "NggVertexCountGpu";
  const auto device = vulkan.Device();
  auto& graphics = vulkan.RuntimeContext();
  const auto& layout = fixture.compiled.program.bindings;
  const auto data = fixture.ShaderData();
  NggCountObservation observed{fixture.wave, vertices, {}};
  {
    FetchGpuBuffer output(graphics, uint64_t{NggCountWords} * 4u, vk::BufferUsageFlagBits::eStorageBuffer);
    std::fill_n(static_cast<uint32_t*>(output.data), NggCountWords, NggCountSentinel);
    static_cast<uint32_t*>(output.data)[0] = 0u;
    output.Flush();
    std::unique_ptr<FetchGpuBuffer> shader_data;
    std::vector<vk::DescriptorSetLayoutBinding> bindings;
    uint32_t descriptor_count = 0u;
    for (const auto& binding : layout.descriptors) {
      Require(name, "supported reflected native bindings",
          binding.kind == FetchGpuIR::DescriptorBindingKind::Buffers ||
          binding.kind == FetchGpuIR::DescriptorBindingKind::ShaderData,
          "unexpected descriptor kind; do not fabricate fallback resources");
      bindings.push_back({FetchGpuIR::NativeBinding(ShaderType::Vertex, binding.kind),
          NativeDescriptorType(binding.kind), NativeDescriptorCount(binding),
          vk::ShaderStageFlagBits::eVertex, nullptr});
      descriptor_count += NativeDescriptorCount(binding);
      if (binding.kind == FetchGpuIR::DescriptorBindingKind::ShaderData) {
        shader_data = std::make_unique<FetchGpuBuffer>(graphics, data.size() * 4u,
            vk::BufferUsageFlagBits::eStorageBuffer);
        std::memcpy(shader_data->data, data.data(), shader_data->size);
        shader_data->Flush();
      }
    }
    vk::DescriptorSetLayout descriptor_layout = nullptr;
    vk::DescriptorSetLayoutCreateInfo descriptor_info{};
    descriptor_info.bindingCount = static_cast<uint32_t>(bindings.size());
    descriptor_info.pBindings = bindings.data();
    RequireVulkanSuccess(device.createDescriptorSetLayout(&descriptor_info, nullptr, &descriptor_layout), "NGG count descriptor layout");
    vk::DescriptorPool pool = nullptr;
    vk::DescriptorPoolSize pool_size{vk::DescriptorType::eStorageBuffer, descriptor_count};
    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.maxSets = pool_info.poolSizeCount = 1u;
    pool_info.pPoolSizes = &pool_size;
    RequireVulkanSuccess(device.createDescriptorPool(&pool_info, nullptr, &pool), "NGG count descriptor pool");
    vk::DescriptorSet set = nullptr;
    vk::DescriptorSetAllocateInfo allocation{};
    allocation.descriptorPool = pool;
    allocation.descriptorSetCount = 1u;
    allocation.pSetLayouts = &descriptor_layout;
    RequireVulkanSuccess(device.allocateDescriptorSets(&allocation, &set), "NGG count descriptor set");
    for (const auto& binding : layout.descriptors) {
      Require(name, "one real descriptor per reflected kind", NativeDescriptorCount(binding) == 1u,
              "fixture only owns one actual output and optional shader data");
      const auto* buffer = binding.kind == FetchGpuIR::DescriptorBindingKind::Buffers ? &output : shader_data.get();
      Require(name, "actual data-buffer owner", buffer != nullptr, "required ShaderData buffer is absent");
      vk::DescriptorBufferInfo info{buffer->buffer, 0u, buffer->size};
      vk::WriteDescriptorSet write{};
      write.dstSet = set;
      write.dstBinding = FetchGpuIR::NativeBinding(ShaderType::Vertex, binding.kind);
      write.descriptorType = NativeDescriptorType(binding.kind);
      write.descriptorCount = 1u;
      write.pBufferInfo = &info;
      device.updateDescriptorSets(1u, &write, 0u, nullptr);
    }
    vk::PushConstantRange push{vk::ShaderStageFlagBits::eVertex, 0u, FetchGpuIR::NativePushConstantSize};
    vk::PipelineLayout pipeline_layout = nullptr;
    vk::PipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.setLayoutCount = 1u;
    pipeline_layout_info.pSetLayouts = &descriptor_layout;
    pipeline_layout_info.pushConstantRangeCount = layout.UsesPushData() ? 1u : 0u;
    pipeline_layout_info.pPushConstantRanges = layout.UsesPushData() ? &push : nullptr;
    RequireVulkanSuccess(device.createPipelineLayout(&pipeline_layout_info, nullptr, &pipeline_layout), "NGG count pipeline layout");
    vk::ShaderModule module = nullptr;
    vk::ShaderModuleCreateInfo module_info{};
    module_info.codeSize = fixture.compiled.spirv.size() * 4u;
    module_info.pCode = fixture.compiled.spirv.data();
    RequireVulkanSuccess(device.createShaderModule(&module_info, nullptr, &module), "NGG count actual module");
    vk::PipelineShaderStageCreateInfo stage{};
    stage.stage = vk::ShaderStageFlagBits::eVertex;
    stage.module = module;
    stage.pName = "main";
    vk::PipelineVertexInputStateCreateInfo vertex_input{};
    vk::PipelineInputAssemblyStateCreateInfo assembly{};
    assembly.topology = vk::PrimitiveTopology::ePointList;
    vk::PipelineRasterizationStateCreateInfo raster{};
    raster.rasterizerDiscardEnable = true;
    raster.polygonMode = vk::PolygonMode::eFill;
    raster.lineWidth = 1.0f;
    vk::PipelineRenderingCreateInfo rendering_info{};
    vk::GraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.pNext = &rendering_info;
    pipeline_info.stageCount = 1u;
    pipeline_info.pStages = &stage;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &assembly;
    pipeline_info.pRasterizationState = &raster;
    pipeline_info.layout = pipeline_layout;
    vk::Pipeline pipeline = nullptr;
    RequireVulkanSuccess(device.createGraphicsPipelines(nullptr, 1u, &pipeline_info, nullptr, &pipeline), "NGG count point-list pipeline");
    auto& scheduler = vulkan.RuntimeRenderer().GetCommandScheduler();
    auto command = scheduler.Current().Handle();
    vk::MemoryBarrier ready{};
    ready.srcAccessMask = vk::AccessFlagBits::eHostWrite;
    ready.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eHost, vk::PipelineStageFlagBits::eVertexShader,
        {}, 1u, &ready, 0u, nullptr, 0u, nullptr);
    vk::RenderingInfo rendering{};
    rendering.renderArea.extent = vk::Extent2D{1u, 1u};
    rendering.layerCount = 1u;
    command.beginRendering(rendering);
    command.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
    command.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout, 0u, 1u, &set, 0u, nullptr);
    if (layout.UsesPushData()) {
      FetchGpuIR::PushData constants;
      std::copy(data.begin(), data.end(), constants.dwords.begin() + layout.push_data_start_dword);
      command.pushConstants(pipeline_layout, vk::ShaderStageFlagBits::eVertex, 0u, sizeof(constants), constants.dwords.data());
    }
    command.draw(vertices, 1u, 0u, 0u);
    command.endRendering();
    vk::MemoryBarrier done{};
    done.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
    done.dstAccessMask = vk::AccessFlagBits::eHostRead;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eVertexShader, vk::PipelineStageFlagBits::eHost,
        {}, 1u, &done, 0u, nullptr, 0u, nullptr);
    scheduler.Finish();
    output.Invalidate();
    const auto* words = static_cast<const uint32_t*>(output.data);
    observed.words.assign(words, words + NggCountWords);
    device.destroyPipeline(pipeline, nullptr);
    device.destroyShaderModule(module, nullptr);
    device.destroyPipelineLayout(pipeline_layout, nullptr);
    device.destroyDescriptorPool(pool, nullptr);
    device.destroyDescriptorSetLayout(descriptor_layout, nullptr);
  }
  return observed; // All fixture GPU objects were retired before semantic checks.
}

void CheckNggVertexCountGpu(VulkanHarness& vulkan) {
  constexpr const char* name = "NggVertexCountGpu";
  auto& graphics = vulkan.RuntimeContext();
  Require(name, "actual vertex stores/atomics capability",
          graphics.physical_device.getFeatures().vertexPipelineStoresAndAtomics,
          "native device does not support the bounded vertex observation; not a passing regression");
  vk::PhysicalDeviceSubgroupProperties subgroup{};
  vk::PhysicalDeviceProperties2 properties{};
  properties.pNext = &subgroup;
  graphics.physical_device.getProperties2(&properties);
  Require(name, "actual native32 vertex ballot", subgroup.subgroupSize == 32u &&
              static_cast<bool>(subgroup.supportedStages & vk::ShaderStageFlagBits::eVertex) &&
              static_cast<bool>(subgroup.supportedOperations & vk::SubgroupFeatureFlagBits::eBallot),
          "fixture explicitly covers native32 ordinary Vertex, not virtual compute/mesh halves");
  HW::Context registers{};
  HW::UserConfig user_config{};
  HW::Shader shaders{};
  vulkan.RuntimeRenderer().GetCommandScheduler().Begin(registers, user_config, shaders);
  std::vector<NggCountObservation> observations;
  for (const uint32_t wave : {32u, 64u}) {
    NggCountCompilation fixture(wave);
    for (const uint32_t vertices : {1u, 17u, 31u, 33u, 64u, 65u})
      observations.push_back(RunNggCountNative(vulkan, fixture, vertices));
  }
  uint32_t mismatches = 0u;
  for (const auto& observed : observations) {
    const uint32_t records = observed.words.at(0u);
    Require(name, "bounded complete observation capacity", records >= observed.vertices && records <= NggCountCapacity,
            "missing native vertex invocation or observation capacity exhausted; not the intended semantic RED");
    for (uint32_t word = 1u; word < NggCountHeaderWords; ++word)
      Require(name, "header sentinel", observed.words[word] == NggCountSentinel, "vertex wrote another header field");
    std::vector<bool> seen(observed.vertices, false);
    uint32_t case_mismatches = 0u;
    for (uint32_t record = 0u; record < records; ++record) {
      const auto* words = observed.words.data() + NggCountHeaderWords + record * NggCountRecordWords;
      Require(name, "complete independently owned record", words[3] < observed.vertices && words[4] == NggCountMarker,
              "atomic ticket did not publish a complete bounded record; not a count RED");
      seen[words[3]] = true;
      for (uint32_t padding = 5u; padding < NggCountRecordWords; ++padding)
        Require(name, "record padding sentinel", words[padding] == NggCountSentinel, "vertex wrote beyond its record payload");
      uint32_t active = 0u;
      for (uint32_t bit = 0u; bit < 32u; ++bit) {
        active += (words[1] >> bit) & 1u;
        active += (words[2] >> bit) & 1u;
      }
      Require(name, "initial native mask positive control", active != 0u && active <= subgroup.subgroupSize && words[2] == 0u,
              "initial EXEC was not the independent native32 entry ballot; not a count RED");
      // Existing upper system fields are a neighboring preservation control.
      // The native64-host two-guest-wave packing contract is separate coverage.
      const uint32_t upper = observed.wave == 32u ? 0x20000000u : 0x10000000u;
      Require(name, "upper merged-wave fields unchanged", (words[0] & ~0xffu) == upper,
              "count correction altered neighboring NGG system fields");
      const bool equal = (words[0] & 0xffu) == active;
      case_mismatches += !equal;
      if (record < 3u || (!equal && case_mismatches <= 3u))
        std::printf("[ngg-count-record] wave=%u draw=%u ticket=%u vertex=%u s3=%08x initialEXEC=%08x/%08x CPUactive=%u equal=%u\n",
            observed.wave, observed.vertices, record, words[3], words[0], words[1], words[2], active, equal);
    }
    Require(name, "all requested vertex IDs observed", std::all_of(seen.begin(), seen.end(), [](bool value) { return value; }),
            "native fixture did not observe every requested vertex");
    const size_t unused = NggCountHeaderWords + size_t{records} * NggCountRecordWords;
    Require(name, "unused output sentinel", std::all_of(observed.words.begin() + unused, observed.words.end(),
              [](uint32_t word) { return word == NggCountSentinel; }), "vertex store exceeded its allocated observation tickets");
    std::printf("[ngg-count-case] guestWave=%u draw=%u nativeRecords=%u observedAllIDs=1 mismatchRecords=%u\n",
        observed.wave, observed.vertices, records, case_mismatches);
    mismatches += case_mismatches;
  }
  Require(name, "ESVertCount equals independent initial active mask", mismatches == 0u,
          "SGPR3 ESVertCount reports full wave capacity instead of actual native input vertices");
}
} // namespace
} // namespace Libs::Graphics
