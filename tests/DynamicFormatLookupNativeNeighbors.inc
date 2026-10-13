// Ignored/uncompiled native numerical proposal. Include globally AFTER
// DynamicFormatLookupSpirvCostTests.inc, which defines the independent FormatLookupGolden.
// Root owns tracked integration, native build/test and process control.
// Probe keeps every original emitted instruction/initializer unchanged. No GPU golden table.
#include <map>
#include <span>

namespace Libs::Graphics {
namespace {
void ConfigureDynamicFormatLookupNativeNeighbors(bool enabled) {
  Require("DynamicFormatLookupNative", "configuration before static compiler getters",
      _putenv_s("KYTY_FORMAT_LOOKUP_SWITCH", enabled ? "1" : "0") == 0,
      "could not pin actual lookup policy; not numerical proof");
  ConfigureGenericBufferTests(false);
}

struct FormatProbeModule {
  std::vector<uint32_t> words;
  bool actual_switch = false;
  uint32_t output_binding = 0u;
};

FormatProbeModule AddProductionFormatProbe(const std::vector<uint32_t>& original) {
  constexpr const char* name = "DynamicFormatLookupNative";
  using Operands = std::vector<uint32_t>;
  Require(name, "production module header", original.size() >= 5u && original[0] == 0x07230203u,
      "missing actual compiler module; not numerical proof");
  struct Instruction { uint32_t op; Operands a; };
  std::vector<Instruction> instructions;
  std::map<uint32_t, Operands> types, constants, composites;
  std::vector<Operands> variables;
  uint32_t lookup = 0u, lookup_result = 0u, active = 0u;
  uint32_t u32 = 0u, boolean = 0u, void_type = 0u, output_binding = 0u;
  const auto text = [](std::span<const uint32_t> words) {
    std::string value;
    for (const auto word : words) for (uint32_t shift = 0u; shift < 32u; shift += 8u) {
      const char c = static_cast<char>(word >> shift); if (c == 0) return value; value += c;
    }
    return value;
  };
  for (size_t p = 5u; p < original.size();) {
    const uint32_t n = original[p] >> 16u, op = original[p] & 65535u;
    Require(name, "actual module framing", n != 0u && n <= original.size() - p,
        "production SPIR-V framing failed; not numerical proof");
    Operands a(original.begin() + p + 1u, original.begin() + p + n);
    instructions.push_back({op, a});
    if (op == spv::OpName && a.size() >= 2u &&
        text(std::span<const uint32_t>{a}.subspan(1u)) == "dynamic_buffer_format_lookup") lookup = a[0];
    if (op == spv::OpTypeInt || op == spv::OpTypeVector || op == spv::OpTypeArray ||
        op == spv::OpTypePointer || op == spv::OpTypeBool || op == spv::OpTypeVoid)
      types[a.at(0)] = a;
    if (op == spv::OpTypeInt && a.size() == 3u && a[1] == 32u && a[2] == 0u) u32 = a[0];
    if (op == spv::OpTypeBool) boolean = a.at(0);
    if (op == spv::OpTypeVoid) void_type = a.at(0);
    if (op == spv::OpConstant) constants[a.at(1)] = a;
    if (op == spv::OpConstantComposite) composites[a.at(1)] = a;
    if (op == spv::OpVariable) variables.push_back(a);
    if (op == spv::OpDecorate && a.size() == 3u && a[1] == spv::DecorationBinding) {
      Require(name, "fresh probe binding", a[2] != UINT32_MAX, "no unused finite probe binding");
      output_binding = std::max(output_binding, a[2] + 1u);
    }
    if (op == spv::OpFunction) { active = a.at(1); if (active == lookup) lookup_result = a.at(0); }
    if (op == spv::OpFunctionEnd) active = 0u;
    p += n;
  }
  (void)active;
  Require(name, "existing scalar types", u32 != 0u && boolean != 0u && void_type != 0u,
      "actual module scalar types unavailable; not numerical proof");
  std::vector<uint32_t> table_variables;
  for (const auto& v : variables) {
    if (v.size() != 4u || v[2] != spv::StorageClassPrivate) continue;
    const auto pointer = types.find(v[0]);
    if (pointer == types.end() || pointer->second.size() != 3u) continue;
    const auto array = types.find(pointer->second[2]);
    if (array == types.end() || array->second.size() != 3u || array->second[1] != u32) continue;
    const auto length = constants.find(array->second[2]);
    if (length == constants.end() || length->second.size() != 3u || length->second[2] != 128u) continue;
    const auto init = composites.find(v[3]);
    if (init == composites.end() || init->second.size() != 130u) continue;
    table_variables.push_back(v[1]);
  }
  const bool actual_switch = lookup != 0u;
  Require(name, "actual production lookup path", actual_switch ?
      table_variables.empty() && lookup_result != 0u : table_variables.size() == 4u,
      "probe needs one actual shared helper OR four actual immutable Private arrays; not a mirrored path");
  if (actual_switch) {
    const auto t = types.find(lookup_result);
    Require(name, "actual helper four-word ABI", t != types.end() && t->second.size() == 3u &&
        t->second[1] == u32 && t->second[2] == 4u,
        "shared helper is not the actual u32x4-return ABI");
  }
  uint32_t next_id = original[3];
  const auto id = [&] { return next_id++; };
  const auto emit = [](Operands& into, spv::Op op, std::initializer_list<uint32_t> a) {
    into.push_back((static_cast<uint32_t>(a.size()) + 1u) << 16u | static_cast<uint32_t>(op));
    into.insert(into.end(), a);
  };
  const auto emit_span = [](Operands& into, spv::Op op, std::span<const uint32_t> a) {
    into.push_back((static_cast<uint32_t>(a.size()) + 1u) << 16u | static_cast<uint32_t>(op));
    into.insert(into.end(), a.begin(), a.end());
  };
  Operands declarations, annotations, entry, function;
  const auto reuse_type = [&](spv::Op op, std::initializer_list<uint32_t> operands) {
    for (const auto& inst : instructions)
      if (inst.op == static_cast<uint32_t>(op) && inst.a.size() == operands.size() + 1u &&
          std::equal(operands.begin(), operands.end(), inst.a.begin() + 1u)) return inst.a[0];
    const uint32_t fresh = id(); Operands a{fresh}; a.insert(a.end(), operands);
    emit_span(declarations, op, a); return fresh;
  };
  const uint32_t vec3 = reuse_type(spv::OpTypeVector, {u32, 3u});
  const uint32_t ptr_gid = reuse_type(spv::OpTypePointer, {spv::StorageClassInput, vec3});
  const uint32_t gid_var = id();
  emit(declarations, spv::OpVariable, {ptr_gid, gid_var, spv::StorageClassInput});
  const uint32_t array = id(), block = id(), ptr_block = id(), output_var = id();
  emit(declarations, spv::OpTypeRuntimeArray, {array, u32});
  emit(declarations, spv::OpTypeStruct, {block, array});
  emit(declarations, spv::OpTypePointer, {ptr_block, spv::StorageClassStorageBuffer, block});
  emit(declarations, spv::OpVariable, {ptr_block, output_var, spv::StorageClassStorageBuffer});
  const uint32_t ptr_output_word = reuse_type(spv::OpTypePointer, {spv::StorageClassStorageBuffer, u32});
  const uint32_t ptr_private_word = reuse_type(spv::OpTypePointer, {spv::StorageClassPrivate, u32});
  const uint32_t fn_type = reuse_type(spv::OpTypeFunction, {void_type}), probe = id();
  emit(annotations, spv::OpDecorate, {gid_var, spv::DecorationBuiltIn, spv::BuiltInGlobalInvocationId});
  emit(annotations, spv::OpDecorate, {array, spv::DecorationArrayStride, 4u});
  emit(annotations, spv::OpDecorate, {block, spv::DecorationBlock});
  emit(annotations, spv::OpMemberDecorate, {block, 0u, spv::DecorationOffset, 0u});
  emit(annotations, spv::OpDecorate, {output_var, spv::DecorationDescriptorSet, 0u});
  emit(annotations, spv::OpDecorate, {output_var, spv::DecorationBinding, output_binding});
  std::map<uint32_t, uint32_t> literals;
  const auto literal = [&](uint32_t value) {
    const auto found = literals.find(value); if (found != literals.end()) return found->second;
    const auto value_id = id(); literals.emplace(value, value_id);
    emit(declarations, spv::OpConstant, {u32, value_id, value}); return value_id;
  };
  const uint32_t zero = literal(0u), c127 = literal(127u), c128 = literal(128u);
  const uint32_t c256 = literal(256u), c384 = literal(384u), c385 = literal(385u);
  const uint32_t c386 = literal(386u), c387 = literal(387u), cases = literal(388u);
  const uint32_t prefix = literal(0xffffff80u), maximum = literal(UINT32_MAX), c4 = literal(4u);
  const uint32_t label = id(), body = id(), done = id();
  emit(function, spv::OpFunction, {void_type, probe, spv::FunctionControlMaskNone, fn_type});
  emit(function, spv::OpLabel, {label});
  const uint32_t gid_vector = id(), index = id(), in_bounds = id();
  emit(function, spv::OpLoad, {vec3, gid_vector, gid_var});
  emit(function, spv::OpCompositeExtract, {u32, index, gid_vector, 0u});
  emit(function, spv::OpULessThan, {boolean, in_bounds, index, cases});
  emit(function, spv::OpSelectionMerge, {done, spv::SelectionControlMaskNone});
  emit(function, spv::OpBranchConditional, {in_bounds, body, done});
  emit(function, spv::OpLabel, {body});
  const auto binary = [&](spv::Op op, uint32_t type, uint32_t a, uint32_t b) {
    const uint32_t value = id(); emit(function, op, {type, value, a, b}); return value;
  };
  const auto select = [&](uint32_t condition, uint32_t a, uint32_t b) {
    const uint32_t value = id(); emit(function, spv::OpSelect, {u32, value, condition, a, b}); return value;
  };
  const uint32_t masked_index = binary(spv::OpBitwiseAnd, u32, index, c127);
  uint32_t raw = select(binary(spv::OpULessThan, boolean, index, c256), index,
      binary(spv::OpBitwiseOr, u32, masked_index, prefix));
  const uint32_t is_max = binary(spv::OpLogicalOr, boolean,
      binary(spv::OpIEqual, boolean, index, c384), binary(spv::OpIEqual, boolean, index, c386));
  const uint32_t is_wrap = binary(spv::OpLogicalOr, boolean,
      binary(spv::OpIEqual, boolean, index, c385), binary(spv::OpIEqual, boolean, index, c387));
  raw = select(is_max, maximum, select(is_wrap, c128, raw));
  uint32_t normalized = binary(spv::OpBitwiseAnd, u32, raw, c127);
  std::array<uint32_t, 4> values{};
  if (actual_switch) {
    // Last two cases independently cover the new pure helper's default branch.
    // Every guest-call case keeps production's existing seven-bit normalization.
    const uint32_t default_case = binary(spv::OpUGreaterThanEqual, boolean, index, c386);
    normalized = select(default_case, raw, normalized);
    const uint32_t tuple = id();
    emit(function, spv::OpFunctionCall, {lookup_result, tuple, lookup, normalized});
    for (uint32_t field = 0u; field < 4u; ++field) {
      values[field] = id();
      emit(function, spv::OpCompositeExtract, {u32, values[field], tuple, field});
    }
  } else {
    for (uint32_t field = 0u; field < 4u; ++field) {
      const uint32_t pointer = id(); values[field] = id();
      emit(function, spv::OpAccessChain, {ptr_private_word, pointer, table_variables[field], normalized});
      emit(function, spv::OpLoad, {u32, values[field], pointer});
    }
  }
  const uint32_t word_index = binary(spv::OpIMul, u32, index, c4);
  for (uint32_t field = 0u; field < 4u; ++field) {
    const uint32_t address = binary(spv::OpIAdd, u32, word_index, literal(4u + field));
    const uint32_t pointer = id();
    emit(function, spv::OpAccessChain, {ptr_output_word, pointer, output_var, zero, address});
    emit(function, spv::OpStore, {pointer, values[field]});
  }
  emit(function, spv::OpBranch, {done});
  emit(function, spv::OpLabel, {done});
  emit(function, spv::OpReturn, {}); emit(function, spv::OpFunctionEnd, {});
  Operands entry_operands {spv::ExecutionModelGLCompute, probe};
  constexpr char probe_name[] = "format_probe";
  for (size_t offset = 0u; offset < sizeof(probe_name); offset += 4u) {
    uint32_t word = 0u;
    for (uint32_t byte = 0u; byte < 4u && offset + byte < sizeof(probe_name); ++byte)
      word |= uint32_t(static_cast<unsigned char>(probe_name[offset + byte])) << (byte * 8u);
    entry_operands.push_back(word);
  }
  entry_operands.push_back(gid_var);
  if (original[1] >= 0x00010400u) {
    entry_operands.push_back(output_var);
    entry_operands.insert(entry_operands.end(), table_variables.begin(), table_variables.end());
  }
  emit_span(entry, spv::OpEntryPoint, entry_operands);
  emit(entry, spv::OpExecutionMode, {probe, spv::ExecutionModeLocalSize, 64u, 1u, 1u});
  Operands result(original.begin(), original.begin() + 5u);
  bool entry_added = false, annotation_added = false, declaration_added = false;
  for (const auto& inst : instructions) {
    if (!entry_added && inst.op == spv::OpExecutionMode) {
      result.insert(result.end(), entry.begin(), entry.end()); entry_added = true;
    }
    if (!annotation_added && inst.op >= spv::OpTypeVoid && inst.op <= spv::OpTypeForwardPointer) {
      result.insert(result.end(), annotations.begin(), annotations.end()); annotation_added = true;
    }
    if (!declaration_added && inst.op == spv::OpFunction) {
      result.insert(result.end(), declarations.begin(), declarations.end()); declaration_added = true;
    }
    emit_span(result, static_cast<spv::Op>(inst.op), inst.a);
  }
  Require(name, "SPIR-V logical-section insertion", entry_added && annotation_added && declaration_added,
      "unsupported compiler module layout; no native probe may run");
  result.insert(result.end(), function.begin(), function.end()); result[3] = next_id;
  spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_3);
  validator.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t& pos, const char* message) {
    std::fprintf(stderr, "format-probe SPIR-V word%zu: %s\n", pos.index, message);
  });
  Require(name, "actual-helper probe validation before GPU", validator.Validate(result),
      "probe did not preserve a valid actual production module; not numerical proof");
  return {std::move(result), actual_switch, output_binding};
}

void CheckDynamicFormatLookupNativeNeighbors(VulkanHarness& vulkan, bool expect_switch) {
  constexpr const char* name = "DynamicFormatLookupNative";
  constexpr uint32_t Cases = 388u, Launched = ((Cases + 63u) / 64u) * 64u;
  constexpr uint32_t Words = 4u + Launched * 4u + 4u, Guard = 0xa195f00du;
  std::array<uint32_t, Words> observed{};
  bool actual_switch = false;
  {
    GenericCompilerProfile(); FetchGpuGuest guest;
    GenericCompilation shader(1u, 32u);
    const auto probe = AddProductionFormatProbe(shader.compiled.spirv);
    actual_switch = probe.actual_switch;
    Require(name, "requested actual emitted path", actual_switch == expect_switch,
        "requested ON/OFF emitter path absent; setup mismatch is not numeric proof");
    FetchGpuBuffer output(vulkan.RuntimeContext(), Words * 4u, vk::BufferUsageFlagBits::eStorageBuffer);
    std::fill_n(static_cast<uint32_t*>(output.data), Words, Guard); output.Flush();
    const auto device = vulkan.Device();
    vk::ShaderModule module = nullptr;
    vk::ShaderModuleCreateInfo shader_info{}; shader_info.codeSize = probe.words.size() * 4u;
    shader_info.pCode = probe.words.data();
    RequireVulkanSuccess(device.createShaderModule(&shader_info, nullptr, &module), "actual format-probe shader module");
    const vk::DescriptorSetLayoutBinding binding{probe.output_binding, vk::DescriptorType::eStorageBuffer, 1u,
        vk::ShaderStageFlagBits::eCompute, nullptr};
    vk::DescriptorSetLayout set_layout = nullptr;
    vk::DescriptorSetLayoutCreateInfo set_info{}; set_info.bindingCount = 1u; set_info.pBindings = &binding;
    RequireVulkanSuccess(device.createDescriptorSetLayout(&set_info, nullptr, &set_layout), "format-probe actual layout");
    const vk::DescriptorPoolSize size{vk::DescriptorType::eStorageBuffer, 1u};
    vk::DescriptorPool pool = nullptr;
    vk::DescriptorPoolCreateInfo pool_info{}; pool_info.maxSets = 1u; pool_info.poolSizeCount = 1u; pool_info.pPoolSizes = &size;
    RequireVulkanSuccess(device.createDescriptorPool(&pool_info, nullptr, &pool), "format-probe actual descriptor pool");
    vk::DescriptorSet set = nullptr;
    vk::DescriptorSetAllocateInfo allocation{}; allocation.descriptorPool = pool;
    allocation.descriptorSetCount = 1u; allocation.pSetLayouts = &set_layout;
    RequireVulkanSuccess(device.allocateDescriptorSets(&allocation, &set), "format-probe actual descriptor set");
    const vk::DescriptorBufferInfo buffer_info{output.buffer, 0u, output.size};
    vk::WriteDescriptorSet write{}; write.dstSet = set; write.dstBinding = probe.output_binding;
    write.descriptorCount = 1u; write.descriptorType = vk::DescriptorType::eStorageBuffer; write.pBufferInfo = &buffer_info;
    device.updateDescriptorSets(1u, &write, 0u, nullptr);
    vk::PipelineLayout layout = nullptr;
    vk::PipelineLayoutCreateInfo layout_info{}; layout_info.setLayoutCount = 1u; layout_info.pSetLayouts = &set_layout;
    RequireVulkanSuccess(device.createPipelineLayout(&layout_info, nullptr, &layout), "format-probe pipeline layout");
    vk::Pipeline pipeline = nullptr;
    vk::ComputePipelineCreateInfo create{}; create.layout = layout;
    create.stage.stage = vk::ShaderStageFlagBits::eCompute; create.stage.module = module; create.stage.pName = "format_probe";
    RequireVulkanSuccess(device.createComputePipelines(nullptr, 1u, &create, nullptr, &pipeline), "format-probe actual compute pipeline");
    vk::CommandPool commands = nullptr;
    vk::CommandPoolCreateInfo command_pool{}; command_pool.queueFamilyIndex = vulkan.RuntimeContext().queue_family;
    RequireVulkanSuccess(device.createCommandPool(&command_pool, nullptr, &commands), "format-probe owned command pool");
    vk::CommandBuffer command = nullptr;
    vk::CommandBufferAllocateInfo command_allocation{}; command_allocation.commandPool = commands;
    command_allocation.level = vk::CommandBufferLevel::ePrimary; command_allocation.commandBufferCount = 1u;
    RequireVulkanSuccess(device.allocateCommandBuffers(&command_allocation, &command), "format-probe owned command buffer");
    vk::CommandBufferBeginInfo begin{}; begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    RequireVulkanSuccess(command.begin(&begin), "format-probe begin");
    vk::BufferMemoryBarrier before{}; before.srcAccessMask = vk::AccessFlagBits::eHostWrite;
    before.dstAccessMask = vk::AccessFlagBits::eShaderWrite;
    before.srcQueueFamilyIndex = before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    before.buffer = output.buffer; before.offset = 0u; before.size = output.size;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eHost, vk::PipelineStageFlagBits::eComputeShader,
        {}, 0u, nullptr, 1u, &before, 0u, nullptr);
    command.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
    command.bindDescriptorSets(vk::PipelineBindPoint::eCompute, layout, 0u, 1u, &set, 0u, nullptr);
    command.dispatch(Launched / 64u, 1u, 1u);
    auto after = before; after.srcAccessMask = vk::AccessFlagBits::eShaderWrite; after.dstAccessMask = vk::AccessFlagBits::eHostRead;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eHost,
        {}, 0u, nullptr, 1u, &after, 0u, nullptr);
    RequireVulkanSuccess(command.end(), "format-probe end");
    vk::Fence fence = nullptr; vk::FenceCreateInfo fence_info{};
    RequireVulkanSuccess(device.createFence(&fence_info, nullptr, &fence), "format-probe owned finite fence");
    vk::SubmitInfo submit{}; submit.commandBufferCount = 1u; submit.pCommandBuffers = &command;
    RequireVulkanSuccess(vulkan.RuntimeContext().queue.submit(1u, &submit, fence), "format-probe actual native submission");
    Require(name, "bounded actual native completion", device.waitForFences(1u, &fence, VK_TRUE, 5'000'000'000ull) == vk::Result::eSuccess,
        "finite native lookup failed/timed out; no driver reset or repeated hang allowed");
    output.Invalidate(); std::memcpy(observed.data(), output.data, observed.size() * 4u);
    device.destroyFence(fence, nullptr); device.destroyCommandPool(commands, nullptr);
    device.destroyPipeline(pipeline, nullptr); device.destroyPipelineLayout(layout, nullptr);
    device.destroyDescriptorPool(pool, nullptr); device.destroyDescriptorSetLayout(set_layout, nullptr);
    device.destroyShaderModule(module, nullptr);
  } // GPU work, Vulkan resources, host buffer and borrowed synthetic guest mapping retired.
  uint32_t errors = 0u;
  for (uint32_t index = 0u; index < Cases; ++index) {
    uint32_t raw = index < 256u ? index : (index & 127u) | 0xffffff80u;
    if (index == 384u || index == 386u) raw = UINT32_MAX;
    if (index == 385u || index == 387u) raw = 128u;
    for (uint32_t field = 0u; field < 4u; ++field) {
      const uint32_t actual = observed[4u + index * 4u + field], expected = FormatLookupGolden[raw & 127u][field];
      if (actual != expected && errors++ < 8u)
        std::printf("[format-native] index=%u raw=%08x field=%u actual=%08x expected=%08x\n", index, raw, field, actual, expected);
    }
  }
  bool guards = true;
  for (uint32_t index = 0u; index < observed.size(); ++index)
    if (index < 4u || index >= 4u + Cases * 4u) guards &= observed[index] == Guard;
  std::printf("[format-native] sharedSwitch=%u ALL128=%u wrapped128/high=%u max/default=%u "
      "launched=%u guardedCases=%u guards=%u cleanup=PASS\n", actual_switch, errors == 0u,
      errors == 0u, errors == 0u, Launched, Cases, guards);
  Require(name, "actual production GPU metadata/all128/wrap/max/default/guard/cleanup",
      errors == 0u && guards, "native production lookup changed independent metadata or wrote beyond bounded output");
}
} // namespace
} // namespace Libs::Graphics
