// Bounded GPU proof of the internal vertex-value diagnostic protocol. This
// uses real public translation/materialization/compilation and one indexed
// point per request; no captured game shader or fabricated resource is used.
#include "graphics/shader/recompiler/ir/VertexValueTrace.h"

namespace Libs::Graphics {
namespace {
constexpr uint64_t TraceGpuHash = 0x56544750524f4f46ull;
constexpr uint32_t TraceGpuRaw = 0x001d0001u;
constexpr uint32_t TraceGpuLoadPc = 0x28u; // after inserted scalar EXEC instruction
constexpr uint32_t TraceGpuGuard = 0xcafef00du;
constexpr uint32_t TraceGpuGuardWords = 16u;
struct TraceGpuDraw { const char* label; uint32_t token, raw; bool active; };

void ConfigureVertexTraceGpuEnvironment() {
  // These settings are read once per process. Configure before VulkanHarness
  // construction, whose production FaultManager also reads CHECK.
  Require("VertexTraceGpu", "fixture environment setup",
          _putenv_s("KYTY_VTX_EXPLODE_CHECK", "1") == 0 &&
          _putenv_s("KYTY_VS_VALUE_TRACE", "56544750524f4f46:0x001d0001:0:0x28") == 0,
          "could not configure diagnostic selector; not a numeric RED");
}

void CheckTraceGpuGuards(const char* name, const uint32_t* words) {
  for (uint32_t i = 0; i < TraceGpuGuardWords; ++i)
    Require(name, "trace region boundary sentinel",
        words[FetchGpuIR::VertexTraceBaseWord - TraceGpuGuardWords + i] == TraceGpuGuard &&
        words[FetchGpuIR::VertexTraceBaseWord + FetchGpuIR::VertexTraceWords + i] == TraceGpuGuard,
        "trace wrote beyond its bounded appended region");
}

void CheckTraceGpuSlot(const char* name, const uint32_t* slot, uint32_t token,
                       uint32_t load_resource, uint32_t position_pc) {
  Require(name, "trace header ownership/token/counts", slot[0] == 1u && slot[1] == token &&
      slot[2] == 0u && slot[3] == 1u && slot[4] == 0u && slot[5] == 0u &&
      slot[6] == 3u && slot[7] == 1u && slot[8] == 1u,
      "expected one live load and distinct pre/post Position events for this token");
  const auto check_event = [&](uint32_t index, FetchGpuIR::VertexTraceKind kind,
                               uint32_t pc, uint32_t sequence) {
    const auto* event = slot + FetchGpuIR::VertexTraceHeaderWords +
        index * FetchGpuIR::VertexTraceEventWords;
    Require(name, "trace event marker/identity/EXEC", event[0] == FetchGpuIR::VertexTraceMarker &&
        event[1] == static_cast<uint32_t>(kind) && event[2] == pc && event[3] == sequence &&
        event[4] == TraceGpuRaw && event[5] == 0u && event[6] == 0u &&
        event[7] == 1u && event[10] == 1u,
        "event identity/sequence or actual buffer/export active predicate is wrong");
    return event;
  };
  const auto* load = check_event(0u, FetchGpuIR::VertexTraceKind::Buffer, TraceGpuLoadPc, 0u);
  Require(name, "actual live load index/result", load[8] == load_resource &&
      load[12] == 1u && load[13] == 0u && load[14] == 0u && load[15] == 0u &&
      load[16] == 0x11223344u && load[20] == 1u && load[21] == 32u && load[23] == 1u,
      "selected-PC event must report actual masked guest index1 and real input word");
  const auto* pre = check_event(30u, FetchGpuIR::VertexTraceKind::PreClip, position_pc, 1u);
  const auto* post = check_event(31u, FetchGpuIR::VertexTraceKind::PostClip, position_pc, 2u);
  const std::array<uint32_t, 4> before{0u, 0u, 0u, 0x3f800000u};
  // Window transform of (0,0): (3/2-1, 1/2-1) = (+0.5,-0.5).
  const std::array<uint32_t, 4> after{0x3f000000u, 0xbf000000u, 0u, 0x3f800000u};
  Require(name, "distinct exact pre/post clip payloads",
      std::equal(before.begin(), before.end(), pre + 12u) &&
      std::equal(after.begin(), after.end(), post + 12u),
      "preclip and postclip must retain the corresponding actual float bits");
  for (uint32_t i = 1u; i < 30u; ++i) {
    const auto* unused = slot + FetchGpuIR::VertexTraceHeaderWords + i * FetchGpuIR::VertexTraceEventWords;
    Require(name, "unused event sentinel", std::all_of(unused,
        unused + FetchGpuIR::VertexTraceEventWords, [](uint32_t word) { return word == 0u; }),
        "one bounded load wrote into another event's slot");
  }
}

void RunVertexTraceGpuModule(VulkanHarness& vulkan, FetchGpuBuffer& native_vertices,
                    FetchGpuBuffer& shader_input, FetchGpuBuffer& output,
                    FetchGpuBuffer& indices, FetchGpuBuffer& faults, bool tracing) {
  const char* name = tracing ? "VertexTraceGpuEnabled" : "VertexTraceGpuOff";
  auto& graphics = vulkan.RuntimeContext();
  const auto device = vulkan.Device();
  const auto input_descriptor = FetchGpuDescriptor(FetchGpuGuest::Input, 4u, 6u);
  const auto output_descriptor = FetchGpuDescriptor(FetchGpuGuest::Output, 0u, 16u);
  std::memcpy(reinterpret_cast<void*>(FetchGpuGuest::Base), input_descriptor.data(), sizeof(input_descriptor));
  std::array<uint32_t, 11> userdata{};
  userdata[8] = UINT32_MAX; // s16:s17 = runtime EXEC (low32 active, high32 zero)
  userdata[0] = static_cast<uint32_t>(FetchGpuGuest::Base);
  userdata[1] = static_cast<uint32_t>(FetchGpuGuest::Base >> 32u);
  std::copy(output_descriptor.begin(), output_descriptor.end(), userdata.begin() + 4u);
  ShaderVertexInputInfo vertex{};
  vertex.logical_stage = ShaderType::Vertex;
  vertex.fetch_embedded = true;
  vertex.fetch_buffer_reg = 0;
  vertex.fetch_attrib_reg = 2;
  vertex.pa_cl_vs_out_cntl = (1u << 21u) | (1u << 16u);
  vertex.clip_space.enabled = true;
  vertex.clip_space.scale[0] = vertex.clip_space.scale[1] = 1.0f;
  vertex.clip_space.offset[0] = 3.0f;
  vertex.clip_space.offset[1] = 1.0f;
  vertex.clip_space.half_extent[0] = vertex.clip_space.half_extent[1] = 2.0f;
  vertex.resources_num = 1;
  std::copy(input_descriptor.begin(), input_descriptor.end(), vertex.resources[0].fields);
  vertex.resources_dst[0].attr_id = 0;
  vertex.resources_dst[0].register_start = 9;
  vertex.resources_dst[0].registers_num = 1;
  ShaderRecompiler::CompileOptions options;
  options.stage = ShaderType::Vertex;
  options.wave_size = 32;
  options.user_data_base = 8;
  options.user_data = userdata;
  options.input_info.vertex = &vertex;
  options.dump_ir = false;
  options.shader_hash = tracing ? TraceGpuHash : TraceGpuHash + 1u;
  auto code = FetchGpuCode(true);
  // S_MOV_B64 EXEC,s[16:17] is ordinary guest ISA. Its user-data value
  // changes at draw time, retaining the real BufferRead's EXEC predicate.
  code.insert(code.begin() + 3u, 0xbefe0410u);
  auto translated = ShaderRecompiler::TranslateProgram(code, options);
  Require(name, "actual guest translation", !translated.skip_dispatch,
          "synthetic guest shader setup did not translate; not a semantic RED");
  auto plan = FetchGpuIR::ExtractResourcePlan(translated.program);
  FetchGpuIR::ResourceSnapshot resources;
  FetchGpuIR::ResourceSpecialization specialization;
  const FetchGpuIR::SrtRuntime runtime{.user_data = userdata,
      .shader_base = reinterpret_cast<uint64_t>(code.data()),
      .read_memory = ReadFetchGpuGuest, .read_specialization_memory = ReadFetchGpuGuest};
  Require(name, "actual resource materialization",
          FetchGpuIR::MaterializeResources(plan, runtime, resources, specialization),
          "tracked synthetic SRDs could not materialize; not a semantic RED");
  auto compiled = ShaderRecompiler::CompileProgram(std::move(translated), options, specialization);
  Require(name, "actual vertex module", compiled.program.stage == ShaderType::Vertex &&
              !compiled.spirv.empty(), "must execute the real compiled vertex stage");
  spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_3);
  validator.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t& position,
                                  const char* message) {
    std::fprintf(stderr, "VertexTraceGpu SPIR-V word%zu: %s\n", position.index, message);
  });
  Require(name, "SPIR-V validation before GPU", validator.Validate(compiled.spirv),
          "compiled vertex SPIR-V invalid; not a semantic RED");
  const auto& layout = compiled.program.bindings;
  Require(name, "bounded ordinary buffer profile", !layout.table,
          "this explicit native fixture requires ordinary buffers, not table/BDA mode");
  Require(name, "actual trace layout setup", layout.vertex_value_trace == tracing &&
              FetchGpuIR::FindBinding(layout, FetchGpuIR::DescriptorBindingKind::FaultBuffer) != nullptr,
          "CHECK/trace selector did not produce the required reflected layout; not a numeric RED");
  Require(name, "runtime EXEC shader-data input",
          std::find(layout.user_data_registers.begin(), layout.user_data_registers.end(), 16u) !=
              layout.user_data_registers.end(),
          "encoded EXEC must remain a real runtime user-data input; not a numeric RED");
  uint32_t load_resource = UINT32_MAX, load_count = 0, position_pc = UINT32_MAX;
  for (const auto* block : compiled.program.blocks) for (const auto& inst : *block) {
    if (FetchGpuIR::BufferAccessOf(inst.GetOpcode()) == FetchGpuIR::BufferAccess::Read &&
        inst.Flags<FetchGpuIR::MemoryFlags>().pc == TraceGpuLoadPc) {
      Require(name, "live buffer predicate setup", inst.NumArgs() == 5u &&
                  inst.Arg(4).GetType() == FetchGpuIR::Type::U1,
              "selected live read lacks the actual guest EXEC input; not a numeric RED");
      load_resource = compiled.program.memory_info.at(inst.Flags<FetchGpuIR::MemoryFlags>().index).resource;
      ++load_count;
    }
    if (inst.GetOpcode() == FetchGpuIR::ValueOpcode::SetAttribute) {
      const auto flags = inst.Flags<FetchGpuIR::ExportFlags>();
      const auto& exp = compiled.program.export_info.at(flags.index);
      if (exp.kind == FetchGpuIR::ExportTargetKind::Position && exp.index == 0u)
        position_pc = flags.pc;
    }
  }
  Require(name, "live selected fetch setup", load_count == 1u && position_pc != UINT32_MAX,
          "expected one selected actual buffer load and Position export; not a numeric RED");
  std::vector<uint32_t> shader_data;
  for (const auto reg : layout.user_data_registers)
    shader_data.push_back(resources.user_data.at(reg - compiled.program.user_data_base));
  shader_data.resize(layout.ShaderDataDwords()); // native binding byte offsets are0
  for (uint32_t k = 0; k < layout.buffer_strides.size(); ++k)
    shader_data[layout.BufferStrideDword() + k / 2u] |=
        (FetchGpuIR::CanonicalPackedStride(resources.buffers.at(layout.buffer_strides[k])) & 0x3fffu)
        << ((k % 2u) * 16u);
  for (uint32_t k = 0; k < layout.buffer_formats.size(); ++k)
    shader_data[layout.BufferFormatDword() + k] =
        FetchGpuIR::RuntimeBufferSlotWord(resources.buffers.at(layout.buffer_formats[k]));
  std::unique_ptr<FetchGpuBuffer> shader_data_buffer, flat_buffer;
  std::vector<vk::DescriptorSetLayoutBinding> bindings;
  std::vector<vk::DescriptorPoolSize> pool_sizes;
  using Kind = FetchGpuIR::DescriptorBindingKind;
  for (const auto& binding : layout.descriptors) {
    Require(name, "supported actual bindings", binding.kind == Kind::Buffers ||
                binding.kind == Kind::ShaderData || binding.kind == Kind::FlattenedSrt ||
                binding.kind == Kind::FaultBuffer,
            "unexpected actual binding needs explicit native fixture support; not a semantic RED");
    const auto count = NativeDescriptorCount(binding);
    const auto type = NativeDescriptorType(binding.kind);
    bindings.push_back({FetchGpuIR::NativeBinding(ShaderType::Vertex, binding.kind), type,
                        count, vk::ShaderStageFlagBits::eVertex, nullptr});
    auto found = std::find_if(pool_sizes.begin(), pool_sizes.end(),
                              [&](const auto& entry) { return entry.type == type; });
    if (found == pool_sizes.end()) pool_sizes.push_back({type, count});
    else found->descriptorCount += count;
    if (binding.kind == Kind::ShaderData) {
      Require(name, "actual shader data", !shader_data.empty(), "required shader data is missing");
      shader_data_buffer = std::make_unique<FetchGpuBuffer>(graphics, shader_data.size() * 4u,
                                                           vk::BufferUsageFlagBits::eStorageBuffer);
      std::memcpy(shader_data_buffer->data, shader_data.data(), shader_data_buffer->size);
      shader_data_buffer->Flush();
    }
    if (binding.kind == Kind::FlattenedSrt) {
      Require(name, "actual flattened SRT", !resources.flattened_srt.empty(), "required SRT data is missing");
      flat_buffer = std::make_unique<FetchGpuBuffer>(graphics, resources.flattened_srt.size() * 4u,
                                                    vk::BufferUsageFlagBits::eStorageBuffer);
      std::memcpy(flat_buffer->data, resources.flattened_srt.data(), flat_buffer->size);
      flat_buffer->Flush();
    }
  }
  vk::DescriptorSetLayout descriptor_layout = nullptr;
  vk::DescriptorSetLayoutCreateInfo descriptor_info{};
  descriptor_info.bindingCount = static_cast<uint32_t>(bindings.size());
  descriptor_info.pBindings = bindings.data();
  RequireVulkanSuccess(device.createDescriptorSetLayout(&descriptor_info, nullptr, &descriptor_layout),
                       "vertex-fetch reflected descriptor layout");
  vk::DescriptorPool pool = nullptr;
  vk::DescriptorPoolCreateInfo pool_info{};
  pool_info.maxSets = 1u;
  pool_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
  pool_info.pPoolSizes = pool_sizes.data();
  RequireVulkanSuccess(device.createDescriptorPool(&pool_info, nullptr, &pool),
                       "vertex-fetch descriptor pool");
  vk::DescriptorSet set = nullptr;
  vk::DescriptorSetAllocateInfo set_info{};
  set_info.descriptorPool = pool;
  set_info.descriptorSetCount = 1u;
  set_info.pSetLayouts = &descriptor_layout;
  RequireVulkanSuccess(device.allocateDescriptorSets(&set_info, &set), "vertex-fetch descriptor set");
  for (const auto& binding : layout.descriptors) {
    std::vector<vk::DescriptorBufferInfo> infos;
    if (binding.kind == Kind::Buffers) {
      for (const auto resource : binding.resources) {
        // MaterializeResources stores each ordinary buffer at the same info
        // resource index; this fixture explicitly excludes indirect children.
        Require(name, "actual ordinary-buffer layout", resource < plan.info.buffers.size() &&
                    resource < resources.buffers.size() &&
                    compiled.program.info.buffers.at(resource).source == plan.info.buffers[resource].source &&
                    compiled.program.info.buffers.at(resource).indirect_root ==
                        FetchGpuIR::BufferResource::NoIndirectBuffer,
                "reflected ordinary buffer does not match its materialized plan index");
        const auto& descriptor = resources.buffers.at(resource);
        const uint64_t address = descriptor.dwords[0] |
            (static_cast<uint64_t>(descriptor.dwords[1] & 0xffffu) << 32u);
        if (address == FetchGpuGuest::Input) infos.push_back({shader_input.buffer, 0u, shader_input.size});
        else {
          Require(name, "actual resource identity", address == FetchGpuGuest::Output,
                  "unexpected buffer SRD; must not fabricate a fallback resource");
          infos.push_back({output.buffer, 0u, output.size});
        }
      }
    } else {
      const auto* buffer = binding.kind == Kind::FaultBuffer ? &faults :
          binding.kind == Kind::ShaderData ? shader_data_buffer.get() : flat_buffer.get();
      Require(name, "actual reflected data buffer", buffer != nullptr, "required data buffer missing");
      infos.push_back({buffer->buffer, 0u, buffer->size});
    }
    Require(name, "native descriptor count", infos.size() == NativeDescriptorCount(binding),
            "actual reflected binding count differs from fixture resources");
    vk::WriteDescriptorSet write{};
    write.dstSet = set;
    write.dstBinding = FetchGpuIR::NativeBinding(ShaderType::Vertex, binding.kind);
    write.descriptorCount = static_cast<uint32_t>(infos.size());
    write.descriptorType = NativeDescriptorType(binding.kind);
    write.pBufferInfo = infos.data();
    device.updateDescriptorSets(1u, &write, 0u, nullptr);
  }
  vk::PipelineLayout pipeline_layout = nullptr;
  vk::PushConstantRange push_range{vk::ShaderStageFlagBits::eVertex, 0u, FetchGpuIR::NativePushConstantSize};
  vk::PipelineLayoutCreateInfo pipeline_layout_info{};
  pipeline_layout_info.setLayoutCount = 1u;
  pipeline_layout_info.pSetLayouts = &descriptor_layout;
  pipeline_layout_info.pushConstantRangeCount = layout.UsesPushData() ? 1u : 0u;
  pipeline_layout_info.pPushConstantRanges = layout.UsesPushData() ? &push_range : nullptr;
  RequireVulkanSuccess(device.createPipelineLayout(&pipeline_layout_info, nullptr, &pipeline_layout),
                       "vertex-fetch pipeline layout");
  vk::ShaderModule module = nullptr;
  vk::ShaderModuleCreateInfo module_info{};
  module_info.codeSize = compiled.spirv.size() * sizeof(uint32_t);
  module_info.pCode = compiled.spirv.data();
  RequireVulkanSuccess(device.createShaderModule(&module_info, nullptr, &module), "actual vertex module");
  vk::PipelineShaderStageCreateInfo stage{};
  stage.stage = vk::ShaderStageFlagBits::eVertex;
  stage.module = module;
  stage.pName = "main";
  vk::VertexInputBindingDescription vertex_binding{0u, 4u, vk::VertexInputRate::eVertex};
  vk::VertexInputAttributeDescription attribute{0u, 0u, vk::Format::eR32Uint, 0u};
  vk::PipelineVertexInputStateCreateInfo vertex_input{};
  vertex_input.vertexBindingDescriptionCount = vertex_input.vertexAttributeDescriptionCount = 1u;
  vertex_input.pVertexBindingDescriptions = &vertex_binding;
  vertex_input.pVertexAttributeDescriptions = &attribute;
  vk::PipelineInputAssemblyStateCreateInfo assembly{};
  assembly.topology = vk::PrimitiveTopology::ePointList;
  vk::PipelineRasterizationStateCreateInfo raster{};
  raster.rasterizerDiscardEnable = true;
  raster.polygonMode = vk::PolygonMode::eFill;
  raster.lineWidth = 1.0f;
  vk::PipelineRenderingCreateInfo pipeline_rendering{};
  vk::GraphicsPipelineCreateInfo pipeline_info{};
  pipeline_info.pNext = &pipeline_rendering;
  pipeline_info.stageCount = 1u;
  pipeline_info.pStages = &stage;
  pipeline_info.pVertexInputState = &vertex_input;
  pipeline_info.pInputAssemblyState = &assembly;
  pipeline_info.pRasterizationState = &raster;
  pipeline_info.layout = pipeline_layout;
  vk::Pipeline pipeline = nullptr;
  RequireVulkanSuccess(device.createGraphicsPipelines(nullptr, 1u, &pipeline_info, nullptr, &pipeline),
                       "real bounded vertex graphics pipeline");
  auto* fault_words = static_cast<uint32_t*>(faults.data);
  std::fill_n(fault_words, faults.size / 4u, 0u);
  for (uint32_t i = 0; i < TraceGpuGuardWords; ++i) {
    fault_words[FetchGpuIR::VertexTraceBaseWord - TraceGpuGuardWords + i] = TraceGpuGuard;
    fault_words[FetchGpuIR::VertexTraceBaseWord + FetchGpuIR::VertexTraceWords + i] = TraceGpuGuard;
  }
  faults.Flush();
  // All inactive controls start with genuinely empty slots. They cannot pass
  // merely because an earlier invocation already claimed the owner CAS.
  const std::array<TraceGpuDraw, 5> draws{{
      {"token0", 0u, TraceGpuRaw, true},
      {"wrong-vertex", 1u, TraceGpuRaw + 1u, true},
      {"inactive-EXEC", 1u, TraceGpuRaw, false},
      {"token1", 1u, TraceGpuRaw, true},
      {"token2-reused-pipeline", 2u, TraceGpuRaw, true}}};
  std::vector<uint32_t> first_slot;
  const uint32_t draw_count = tracing ? static_cast<uint32_t>(draws.size()) : 1u;
  for (uint32_t draw = 0; draw < draw_count; ++draw) {
    const auto& request = draws[draw];
    for (uint32_t k = 0; k < layout.user_data_registers.size(); ++k)
      if (layout.user_data_registers[k] == 16u) shader_data[k] = request.active ? UINT32_MAX : 0u;
    if (tracing) {
      shader_data[layout.VertexTraceDword()] = request.token;
      shader_data[layout.VertexTraceDword() + 1u] = 0u;
    }
    if (shader_data_buffer) {
      std::memcpy(shader_data_buffer->data, shader_data.data(), shader_data_buffer->size);
      shader_data_buffer->Flush();
    }
    std::memcpy(indices.data, &request.raw, sizeof(request.raw));
    indices.Flush();
    const std::array<uint32_t, 4> sentinel{0xdeadbeefu, 0xdeadbeefu, 0xdeadbeefu, 0xdeadbeefu};
    std::memcpy(output.data, sentinel.data(), sizeof(sentinel));
    output.Flush();
    auto& scheduler = vulkan.RuntimeRenderer().GetCommandScheduler();
    auto command = scheduler.Current().Handle();
    vk::MemoryBarrier ready{};
    ready.srcAccessMask = vk::AccessFlagBits::eHostWrite;
    ready.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite |
        vk::AccessFlagBits::eVertexAttributeRead | vk::AccessFlagBits::eIndexRead;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eHost,
        vk::PipelineStageFlagBits::eVertexInput | vk::PipelineStageFlagBits::eVertexShader,
        {}, 1u, &ready, 0u, nullptr, 0u, nullptr);
    vk::RenderingInfo rendering{};
    rendering.renderArea.extent = vk::Extent2D{1u, 1u};
    rendering.layerCount = 1u;
    command.beginRendering(rendering);
    command.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
    command.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout, 0u, 1u, &set, 0u, nullptr);
    if (layout.UsesPushData()) {
      FetchGpuIR::PushData push;
      std::copy(shader_data.begin(), shader_data.end(), push.dwords.begin() + layout.push_data_start_dword);
      command.pushConstants(pipeline_layout, vk::ShaderStageFlagBits::eVertex, 0u, sizeof(push), push.dwords.data());
    }
    const vk::DeviceSize zero = 0;
    command.bindVertexBuffers(0u, 1u, &native_vertices.buffer, &zero);
    command.bindIndexBuffer(indices.buffer, 0u, vk::IndexType::eUint32);
    FetchGpuIR::CompiledShaderInfo info{};
    info.user_data_base = compiled.program.user_data_base;
    info.info = compiled.program.info;
    vertex.stage.program = &info;
    vertex.stage.resources = &resources;
    const auto [base_vertex, first_instance] = ResolveDrawOffsets(0u, vertex);
    Require(name, "actual zero draw offsets", base_vertex == 0 && first_instance == 0u,
            "no-offset fixture must not hide draw-wide builtin changes");
    command.drawIndexed(1u, 1u, 0u, base_vertex, first_instance);
    command.endRendering();
    vk::MemoryBarrier done{};
    done.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
    done.dstAccessMask = vk::AccessFlagBits::eHostRead;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eVertexShader, vk::PipelineStageFlagBits::eHost,
        {}, 1u, &done, 0u, nullptr, 0u, nullptr);
    scheduler.Finish();
    output.Invalidate();
    faults.Invalidate();
    std::array<uint32_t, 4> actual{};
    std::memcpy(actual.data(), output.data, sizeof(actual));
    const std::array<uint32_t, 4> expected{request.raw == TraceGpuRaw ? 0x11223344u : 0x22334455u,
        request.raw, request.raw >> 16u, 0x13579bdfu};
    Require(name, "unchanged real payload/raw/high readback",
            actual == (request.active ? expected : sentinel),
            "trace must preserve actual output, including inactive guest stores");
    CheckTraceGpuGuards(name, fault_words);
    if (!tracing || draw < 3u) {
      Require(name, "empty trace for unselected/inactive invocation",
              std::all_of(fault_words + FetchGpuIR::VertexTraceBaseWord,
                          fault_words + FetchGpuIR::VertexTraceBaseWord + FetchGpuIR::VertexTraceWords,
                          [](uint32_t word) { return word == 0u; }),
              "token0, wrong vertex, inactive EXEC or unselected hash wrote trace events");
    } else {
      const auto* slot = fault_words + FetchGpuIR::VertexTraceBaseWord +
          (request.token - 1u) * FetchGpuIR::VertexTraceSlotWords;
      CheckTraceGpuSlot(name, slot, request.token, load_resource, position_pc);
      if (request.token == 1u) {
        first_slot.assign(slot, slot + FetchGpuIR::VertexTraceSlotWords);
        const auto* next = slot + FetchGpuIR::VertexTraceSlotWords;
        Require(name, "token1 leaves token2 slot empty",
                std::all_of(next, next + FetchGpuIR::VertexTraceSlotWords,
                            [](uint32_t word) { return word == 0u; }), "first draw crossed its immutable slot");
      } else {
        Require(name, "token2 preserves first cached draw",
                std::equal(first_slot.begin(), first_slot.end(),
                           fault_words + FetchGpuIR::VertexTraceBaseWord),
                "reused native pipeline rewrote the first token's immutable slot");
      }
    }
    std::printf("[gpu] %s %s payload=%08x raw=%08x high=%08x token=%u PASS\n",
                name, request.label, actual[0], actual[1], actual[2], request.token);
  }
  device.destroyPipeline(pipeline, nullptr);
  device.destroyShaderModule(module, nullptr);
  device.destroyPipelineLayout(pipeline_layout, nullptr);
  device.destroyDescriptorPool(pool, nullptr);
  device.destroyDescriptorSetLayout(descriptor_layout, nullptr);
}

void CheckVertexValueTraceGpu(VulkanHarness& vulkan) {
  constexpr const char* name = "VertexTraceGpu";
  const auto& selected = FetchGpuIR::VertexValueTrace();
  Require(name, "diagnostic selector setup", selected.enabled && selected.hash == TraceGpuHash &&
      selected.vertex == TraceGpuRaw && selected.instance == 0u && selected.pc_count == 1u &&
      selected.pcs[0] == TraceGpuLoadPc,
      "trace environment parsed before fixture setup; not a numeric RED");
  auto& graphics = vulkan.RuntimeContext();
  const auto features = graphics.physical_device.getFeatures();
  Require(name, "vertex stores capability", features.vertexPipelineStoresAndAtomics,
          "selected bootstrap device cannot store vertex results; unsupported is not a passing test");
  vk::PhysicalDeviceSubgroupProperties subgroup{};
  vk::PhysicalDeviceProperties2 properties{};
  properties.pNext = &subgroup;
  graphics.physical_device.getProperties2(&properties);
  Require(name, "actual vertex subgroup", subgroup.subgroupSize == 32u &&
              static_cast<bool>(subgroup.supportedStages & vk::ShaderStageFlagBits::eVertex) &&
              static_cast<bool>(subgroup.supportedOperations & vk::SubgroupFeatureFlagBits::eBallot),
          "fixture requires actual32-lane vertex ballot support; unsupported is not a passing test");
  const auto format = graphics.physical_device.getFormatProperties(vk::Format::eR32Uint);
  Require(name, "native UInt attribute", static_cast<bool>(format.bufferFeatures & vk::FormatFeatureFlagBits::eVertexBuffer),
          "R32Uint vertex input unsupported; not a passing regression");
  ShaderRecompiler::SetTableMode(false);
  ShaderRecompiler::SetHardwareStorageBufferBounds(false);
  FetchGpuGuest guest;
  constexpr uint32_t raw = 0x001d0001u;
  const std::array<uint32_t, 6> words{0x01020304u, 0x11223344u, 0x22334455u,
                                     0x33445566u, 0x44556677u, 0x66778899u};
  std::memcpy(reinterpret_cast<void*>(FetchGpuGuest::Input), words.data(), sizeof(words));
  FetchGpuBuffer native_vertices(graphics, (uint64_t{raw} + 2u) * 4u, vk::BufferUsageFlagBits::eVertexBuffer);
  FetchGpuBuffer shader_input(graphics, sizeof(words), vk::BufferUsageFlagBits::eStorageBuffer);
  FetchGpuBuffer output(graphics, 16u, vk::BufferUsageFlagBits::eStorageBuffer);
  FetchGpuBuffer indices(graphics, 4u, vk::BufferUsageFlagBits::eIndexBuffer);
  FetchGpuBuffer faults(graphics,
      (uint64_t{FetchGpuIR::VertexTraceBaseWord} + FetchGpuIR::VertexTraceWords + TraceGpuGuardWords) * 4u,
      vk::BufferUsageFlagBits::eStorageBuffer);
  std::memcpy(native_vertices.data, words.data(), sizeof(words));
  static_cast<uint32_t*>(native_vertices.data)[raw] = 0x55667788u;
  std::memcpy(shader_input.data, words.data(), sizeof(words));
  native_vertices.Flush();
  shader_input.Flush();
  HW::Context registers{};
  HW::UserConfig user_config{};
  HW::Shader shaders{};
  vulkan.RuntimeRenderer().GetCommandScheduler().Begin(registers, user_config, shaders);
  std::printf("[host] bounded native VB=%llu bytes, guest shader range=%zu bytes; loop-free1indexed vertex\n",
              static_cast<unsigned long long>(native_vertices.size), sizeof(words));
  RunVertexTraceGpuModule(vulkan, native_vertices, shader_input, output, indices, faults, false);
  RunVertexTraceGpuModule(vulkan, native_vertices, shader_input, output, indices, faults, true);
}
} // namespace
} // namespace Libs::Graphics
