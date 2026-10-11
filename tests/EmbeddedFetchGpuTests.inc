// Real vertex-stage execution of wholly synthetic guest fetch ISA. The native
// vertex buffer extends through the large raw index, so a wrong attribute
// rewrite reads a distinct valid sentinel instead of invoking host OOB behavior.
#include "graphics/host_gpu/renderer/renderDraw.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/ir/passes/BindingLayout.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "spirv-tools/libspirv.hpp"

#include <span>
#include <utility>

namespace Libs::Graphics {
namespace {
namespace FetchGpuIR = ShaderRecompiler::IR;

class FetchGpuBuffer {
public:
  FetchGpuBuffer(GraphicContext& graphics, uint64_t bytes, vk::BufferUsageFlags usage)
      : size(bytes), m_graphics(graphics) {
    vk::BufferCreateInfo info{};
    info.size = bytes;
    info.usage = usage;
    VmaAllocationCreateInfo memory{};
    memory.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    memory.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    memory.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    VkBuffer native = VK_NULL_HANDLE;
    VmaAllocationInfo mapping{};
    RequireVulkanSuccess(static_cast<vk::Result>(vmaCreateBuffer(graphics.allocator,
        reinterpret_cast<const VkBufferCreateInfo*>(&info), &memory, &native, &allocation, &mapping)),
        "vertex-fetch native buffer allocation");
    buffer = native;
    data = mapping.pMappedData;
    Require("EmbeddedFetchGpu", "real native mapping", data != nullptr,
            "VMA buffer is not mapped; not a semantic RED");
    std::memset(data, 0, bytes);
  }
  ~FetchGpuBuffer() {
    if (buffer != nullptr) vmaDestroyBuffer(m_graphics.allocator, static_cast<VkBuffer>(buffer), allocation);
  }
  FetchGpuBuffer(const FetchGpuBuffer&) = delete;
  FetchGpuBuffer& operator=(const FetchGpuBuffer&) = delete;
  void Flush() {
    RequireVulkanSuccess(static_cast<vk::Result>(vmaFlushAllocation(m_graphics.allocator,
        allocation, 0u, size)), "vertex-fetch host upload flush");
  }
  void Invalidate() {
    RequireVulkanSuccess(static_cast<vk::Result>(vmaInvalidateAllocation(m_graphics.allocator,
        allocation, 0u, size)), "vertex-fetch readback invalidate");
  }
  vk::Buffer buffer = nullptr;
  VmaAllocation allocation = nullptr;
  void* data = nullptr;
  uint64_t size = 0;
private:
  GraphicContext& m_graphics;
};

struct FetchGpuGuest {
  static constexpr uint64_t Base = 0x0000000200a00000ull;
  static constexpr uint64_t Bytes = 0x10000u;
  static constexpr uint64_t Input = Base + 0x1000u;
  static constexpr uint64_t Output = Base + 0x2000u;
  int64_t direct = -1;
  FetchGpuGuest() {
    Require("EmbeddedFetchGpu", "tracked synthetic guest allocation",
        LibKernel::Memory::KernelAllocateDirectMemory(0, LibKernel::Memory::KernelGetDirectMemorySize(),
            Bytes, Bytes, 0, &direct) == 0, "guest allocation failed; not a semantic RED");
    void* address = reinterpret_cast<void*>(Base);
    Require("EmbeddedFetchGpu", "tracked synthetic guest map",
        LibKernel::Memory::KernelMapDirectMemory(&address, Bytes, 0x3, 0x10, direct, Bytes) == 0 &&
            address == reinterpret_cast<void*>(Base), "guest map failed; not a semantic RED");
    std::memset(address, 0, Bytes);
  }
  ~FetchGpuGuest() {
    Require("EmbeddedFetchGpu", "guest cleanup",
        LibKernel::Memory::KernelMunmap(Base, Bytes) == 0 &&
            LibKernel::Memory::KernelReleaseDirectMemory(direct, Bytes) == 0,
        "task-owned synthetic guest mapping was not released");
  }
};

bool ReadFetchGpuGuest(void*, uint64_t address, std::span<uint32_t> words) {
  const uint64_t bytes = words.size_bytes();
  if (address < FetchGpuGuest::Base || bytes > FetchGpuGuest::Bytes ||
      address - FetchGpuGuest::Base > FetchGpuGuest::Bytes - bytes) return false;
  return LibKernel::Memory::TryReadBacking(address, words.data(), bytes);
}

std::array<uint32_t, 4> FetchGpuDescriptor(uint64_t address, uint32_t stride, uint32_t records) {
  return {static_cast<uint32_t>(address),
          static_cast<uint32_t>(address >> 32u) | (stride << 16u), records,
          DstSel(4, 5, 6, 7) |
              (static_cast<uint32_t>(Prospero::BufferFormat::k32UInt) << 12u) |
              (1u << 24u) | (stride == 0 ? 3u << 28u : 0u)};
}

std::vector<uint32_t> FetchGpuCode(bool masked) {
  const auto vgpr = [](uint32_t reg) { return 256u + reg; };
  const auto mov = [&](uint32_t dst, uint32_t src) {
    return (0x3fu << 25u) | (dst << 17u) | (1u << 9u) | src;
  };
  std::vector<uint32_t> code{
      0xbe800380u, // s_mov_b32 s0,0
      (0x3du << 26u) | (2u << 18u) | (20u << 6u) | 4u, 0u,
      mov(30, vgpr(5)),
      (0x16u << 25u) | (31u << 17u) | (5u << 9u) | 144u,
      (0x35u << 26u) | (0x101u << 16u), vgpr(8) | (vgpr(5) << 9u) | (193u << 18u)};
  if (masked) {
    code.push_back((0x1bu << 25u) | 255u); // v_and_b32 v0,ffff,v0
    code.push_back(0xffffu);
  }
  code.push_back((0x38u << 26u) | (1u << 13u)); // buffer_load_format_x
  code.push_back((128u << 24u) | (5u << 16u) | (9u << 8u));
  code.push_back(mov(10, vgpr(30)));
  code.push_back(mov(11, vgpr(31)));
  code.push_back(mov(12, 255u));
  code.push_back(0x13579bdfu);
  code.push_back(mov(40, 128u));
  code.push_back((0x38u << 26u) | (0x1eu << 18u) | (1u << 12u));
  code.push_back((128u << 24u) | (3u << 16u) | (9u << 8u) | 40u);
  // Real guest buffer_store_dwordx4 writes payload, full raw ID, high16, marker.
  for (uint32_t reg = 20; reg < 23; ++reg) code.push_back(mov(reg, 128u));
  code.push_back(mov(23, 242u)); // finite float1 position.w / point size
  code.push_back((0x3eu << 26u) | (0x0cu << 4u) | 0xfu | (1u << 11u));
  code.push_back(20u | (21u << 8u) | (22u << 16u) | (23u << 24u));
  code.push_back((0x3eu << 26u) | (0x0du << 4u) | 1u | (1u << 11u));
  code.push_back(23u);
  code.push_back(0xbf810000u);
  return code;
}

void RunFetchGpuCase(VulkanHarness& vulkan, FetchGpuBuffer& native_vertices,
                    FetchGpuBuffer& shader_input, FetchGpuBuffer& output,
                    FetchGpuBuffer& indices, bool masked) {
  const char* name = masked ? "EmbeddedFetchGpuMaskedRaw" : "EmbeddedFetchGpuCanonical";
  auto& graphics = vulkan.RuntimeContext();
  const auto device = vulkan.Device();
  const auto input_descriptor = FetchGpuDescriptor(FetchGpuGuest::Input, 4u, 6u);
  const auto output_descriptor = FetchGpuDescriptor(FetchGpuGuest::Output, 0u, 16u);
  std::memcpy(reinterpret_cast<void*>(FetchGpuGuest::Base), input_descriptor.data(), sizeof(input_descriptor));
  std::array<uint32_t, 11> userdata{};
  userdata[0] = static_cast<uint32_t>(FetchGpuGuest::Base);
  userdata[1] = static_cast<uint32_t>(FetchGpuGuest::Base >> 32u);
  std::copy(output_descriptor.begin(), output_descriptor.end(), userdata.begin() + 4u);
  ShaderVertexInputInfo vertex{};
  vertex.logical_stage = ShaderType::Vertex;
  vertex.fetch_embedded = true;
  vertex.fetch_buffer_reg = 0;
  vertex.fetch_attrib_reg = 2;
  vertex.pa_cl_vs_out_cntl = (1u << 21u) | (1u << 16u);
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
  const auto code = FetchGpuCode(masked);
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
    std::fprintf(stderr, "EmbeddedFetchGpu SPIR-V word%zu: %s\n", position.index, message);
  });
  Require(name, "SPIR-V validation before GPU", validator.Validate(compiled.spirv),
          "compiled vertex SPIR-V invalid; not a semantic RED");
  const auto& layout = compiled.program.bindings;
  Require(name, "bounded ordinary buffer profile", !layout.table,
          "this explicit native fixture requires ordinary buffers, not table/BDA mode");
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
                binding.kind == Kind::ShaderData || binding.kind == Kind::FlattenedSrt,
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
      const auto* buffer = binding.kind == Kind::ShaderData ? shader_data_buffer.get() : flat_buffer.get();
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
  const uint32_t raw = masked ? 0x001d0001u : 1u;
  std::memcpy(indices.data, &raw, sizeof(raw));
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
  // These fixtures have no guest offset prolog. Their actual shared resolver
  // must leave Vulkan baseVertex/firstInstance0; nonzero-offset ABI is separate debt.
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
  std::array<uint32_t, 4> actual{};
  std::memcpy(actual.data(), output.data, sizeof(actual));
  std::printf("[gpu] %s actual=%08x,%08x,%08x,%08x attrs=%u nativeRawSentinel=%08x\n",
      name, actual[0], actual[1], actual[2], actual[3],
      compiled.program.info.vertex_fetch_components[0], 0x55667788u);
  const std::array<uint32_t, 4> expected{0x11223344u, raw, raw >> 16u, 0x13579bdfu};
  Require(name, masked ? "actual masked payload/raw/high readback" : "actual canonical positive control",
          actual == expected, "real vertex execution must fetch guest index1 while preserving full raw/high ID bits");
  std::printf("[gpu] %s PASS\n", name);
  device.destroyPipeline(pipeline, nullptr);
  device.destroyShaderModule(module, nullptr);
  device.destroyPipelineLayout(pipeline_layout, nullptr);
  device.destroyDescriptorPool(pool, nullptr);
  device.destroyDescriptorSetLayout(descriptor_layout, nullptr);
}

void CheckEmbeddedFetchGpu(VulkanHarness& vulkan) {
  constexpr const char* name = "EmbeddedFetchGpu";
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
  FetchGpuBuffer native_vertices(graphics, (uint64_t{raw} + 1u) * 4u, vk::BufferUsageFlagBits::eVertexBuffer);
  FetchGpuBuffer shader_input(graphics, sizeof(words), vk::BufferUsageFlagBits::eStorageBuffer);
  FetchGpuBuffer output(graphics, 16u, vk::BufferUsageFlagBits::eStorageBuffer);
  FetchGpuBuffer indices(graphics, 4u, vk::BufferUsageFlagBits::eIndexBuffer);
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
  RunFetchGpuCase(vulkan, native_vertices, shader_input, output, indices, false);
  RunFetchGpuCase(vulkan, native_vertices, shader_input, output, indices, true);
}
} // namespace
} // namespace Libs::Graphics
