// Test-only real binding-acquisition sequence used by RenderCompute. A prior
// GPU write makes a small sampled image acquire/merge its buffer after the
// initial BDA pass. The new aligned owner's fringe is outside the image/SRD.
#include "graphics/shader/shaderBindings.h"

namespace Libs::Graphics {
namespace {
constexpr uint64_t LateBdaInput = FetchGpuGuest::Base + 0x1000u;
constexpr uint64_t LateBdaImage = FetchGpuGuest::Base + 0x3000u;
constexpr uint64_t LateBdaFringe = FetchGpuGuest::Base + 0x6000u;
constexpr uint64_t LateBdaOutput = FetchGpuGuest::Base + 0xc000u;
constexpr uint32_t LateBdaInputWord = 0x12345678u, LateBdaImageWord = 0x89abcdefu;
constexpr uint32_t LateBdaFringeWord = 0x71335599u, LateBdaCanary = 0xaabbccddu;

struct LateBdaCompilation {
  ShaderComputeInputInfo compute{};
  std::array<uint32_t, 16> userdata{};
  std::vector<uint32_t> code;
  FetchGpuIR::ResourceSnapshot resources;
  ShaderRecompiler::CompileResult compiled;
  FetchGpuIR::CompiledShaderInfo info;
  LateBdaCompilation() {
    const auto input = FetchGpuDescriptor(LateBdaInput, 0u, 12u);
    const auto output = FetchGpuDescriptor(LateBdaOutput, 0u, 24u);
    std::copy(input.begin(), input.end(), userdata.begin());
    std::copy(output.begin(), output.end(), userdata.begin() + 4u);
    const uint64_t base = LateBdaImage >> 8u;
    userdata[8] = static_cast<uint32_t>(base);
    userdata[9] = static_cast<uint32_t>(base >> 32u) |
        ((static_cast<uint32_t>(Prospero::BufferFormat::k32UInt) & 0x1ffu) << 20u) | (3u << 30u);
    userdata[10] = (63u >> 2u) | (31u << 14u); // Actual linear64x32 R32UInt,8192bytes.
    userdata[11] = DstSel(4, 5, 6, 7) | (static_cast<uint32_t>(Prospero::TileMode::kLinear) << 20u) |
        (static_cast<uint32_t>(Prospero::ImageType::kColor2D) << 28u);
    const auto mov = [](uint32_t dst, uint32_t src) {
      return (0x3fu << 25u) | (dst << 17u) | (1u << 9u) | src;
    };
    code = {
        (0x38u << 26u) | (0x0fu << 18u), (128u << 24u), // BUFFER_LOAD_DWORDX3 v[0:2],s[0:3]
        (0x37u << 26u) | (0x0cu << 18u), (3u << 24u) | (127u << 16u) | 1u,
        mov(20u, 128u), mov(21u, 128u),
        (0x3cu << 26u) | (1u << 8u) | (1u << 3u), (2u << 16u) | (4u << 8u) | 20u,
        mov(6u, 129u), (0x38u << 26u) | (0x32u << 18u) | (1u << 14u) | 16u,
        (128u << 24u) | (1u << 16u) | (6u << 8u),
        mov(10u, 256u), mov(11u, 259u), mov(12u, 260u), mov(13u, 262u),
        (0x38u << 26u) | (0x1eu << 18u), (128u << 24u) | (1u << 16u) | (10u << 8u),
        0xbf810000u};
    compute.threads_num[0] = compute.threads_num[1] = compute.threads_num[2] = 1u;
    compute.dispatch_threads_num[0] = compute.dispatch_threads_num[1] = compute.dispatch_threads_num[2] = 1u;
    compute.wave_size = compute.host_subgroup_size = 32u;
    ShaderRecompiler::CompileOptions options;
    options.stage = ShaderType::Compute;
    options.wave_size = 32u;
    options.user_data = userdata;
    options.input_info.compute = &compute;
    options.dump_ir = false;
    auto translated = ShaderRecompiler::TranslateProgram(code, options);
    Require("LateBdaImageFringe", "actual synthetic translation", !translated.skip_dispatch,
            "synthetic guest code failed translation; not a residency RED");
    auto plan = FetchGpuIR::ExtractResourcePlan(translated.program);
    FetchGpuIR::ResourceSpecialization specialization;
    const FetchGpuIR::SrtRuntime runtime{.user_data = userdata,
        .shader_base = reinterpret_cast<uint64_t>(code.data()),
        .read_memory = ReadFetchGpuGuest, .read_specialization_memory = ReadFetchGpuGuest};
    Require("LateBdaImageFringe", "real resource materialization",
            FetchGpuIR::MaterializeResources(plan, runtime, resources, specialization),
            "actual SRD/T# failed materialization; not a residency RED");
    compiled = ShaderRecompiler::CompileProgram(std::move(translated), options, specialization);
    spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_3);
    Require("LateBdaImageFringe", "real DMA/image module", validator.Validate(compiled.spirv) &&
                compiled.program.info.uses_dma && compiled.program.info.buffers.size() == 2u &&
                compiled.program.info.images.size() == 1u && !compiled.program.bindings.table,
            "fixture must exercise actual dynamic FLAT plus sampled image and two ordinary SRDs");
    for (size_t word = 5u; word < compiled.spirv.size(); word += compiled.spirv[word] >> 16u)
      Require("LateBdaImageFringe", "no GPU loops", (compiled.spirv[word] & 0xffffu) != spv::OpLoopMerge,
              "fixture is not bounded");
    info.stage = ShaderType::Compute;
    info.wave_size = 32u;
    info.user_data_base = compiled.program.user_data_base;
    info.user_data_count = compiled.program.user_data_count;
    info.info = compiled.program.info;
    info.bindings = compiled.program.bindings;
  }
};

class LateBdaNativePipeline {
public:
  PipelineCache::Pipeline native{};
  LateBdaNativePipeline(VulkanHarness& vulkan, const LateBdaCompilation& shader) : device(vulkan.Device()) {
    native.uses_push_descriptors = true;
    std::vector<vk::DescriptorSetLayoutBinding> bindings;
    uint32_t count = 0u;
    for (const auto& binding : shader.info.bindings.descriptors) {
      bindings.push_back({FetchGpuIR::NativeBinding(ShaderType::Compute, binding.kind),
          NativeDescriptorType(binding.kind), NativeDescriptorCount(binding),
          vk::ShaderStageFlagBits::eCompute, nullptr});
      count += NativeDescriptorCount(binding);
    }
    Require("LateBdaImageFringe", "actual push descriptor budget",
            vulkan.RuntimeContext().max_push_descriptors >= count,
            "bootstrap must enable actual KHR push descriptors; not a residency RED");
    vk::DescriptorSetLayoutCreateInfo desc{};
    desc.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
    desc.bindingCount = static_cast<uint32_t>(bindings.size());
    desc.pBindings = bindings.data();
    RequireVulkanSuccess(device.createDescriptorSetLayout(&desc, nullptr, &native.descriptor_set_layout), "late-BDA real layout");
    vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0u, FetchGpuIR::NativePushConstantSize};
    vk::PipelineLayoutCreateInfo layout{};
    layout.setLayoutCount = 1u;
    layout.pSetLayouts = &native.descriptor_set_layout;
    layout.pushConstantRangeCount = shader.info.bindings.UsesPushData() ? 1u : 0u;
    layout.pPushConstantRanges = shader.info.bindings.UsesPushData() ? &push : nullptr;
    RequireVulkanSuccess(device.createPipelineLayout(&layout, nullptr, &native.pipeline_layout), "late-BDA pipeline layout");
    vk::ShaderModuleCreateInfo module_info{};
    module_info.codeSize = shader.compiled.spirv.size() * 4u;
    module_info.pCode = shader.compiled.spirv.data();
    RequireVulkanSuccess(device.createShaderModule(&module_info, nullptr, &module), "late-BDA actual guest module");
    vk::ComputePipelineCreateInfo pipeline{};
    pipeline.stage.stage = vk::ShaderStageFlagBits::eCompute;
    pipeline.stage.module = module;
    pipeline.stage.pName = "main";
    pipeline.layout = native.pipeline_layout;
    RequireVulkanSuccess(device.createComputePipelines(nullptr, 1u, &pipeline, nullptr, &native.pipeline), "late-BDA bounded pipeline");
  }
  ~LateBdaNativePipeline() {
    device.destroyPipeline(native.pipeline, nullptr);
    device.destroyShaderModule(module, nullptr);
    device.destroyPipelineLayout(native.pipeline_layout, nullptr);
    device.destroyDescriptorSetLayout(native.descriptor_set_layout, nullptr);
  }
private:
  vk::Device device;
  vk::ShaderModule module = nullptr;
};

void ConfigureLateBdaImageFringeTests() {
  Require("LateBdaImageFringe", "pin observation profile",
          _putenv_s("KYTY_RECORD_THREAD", "0") == 0 && _putenv_s("KYTY_TABLE_MODE", "0") == 0 &&
          _putenv_s("KYTY_VTX_EXPLODE_CHECK", "0") == 0,
          "configuration failed before first getter; not a residency RED");
  ShaderRecompiler::SetTableMode(false);
  ShaderRecompiler::SetHardwareStorageBufferBounds(false);
}

void CheckLateBdaImageFringeGpu(VulkanHarness& vulkan) {
  constexpr const char* name = "LateBdaImageFringe";
  std::array<uint32_t, 6> actual{};
  uint64_t before_pte = 0u, after_pte = 0u, expected_pte = 0u;
  uint32_t actual_source_marker = 0u;
  bool absent_after_prepare = false, late_owner = false;
  {
    FetchGpuGuest guest;
    const std::array<uint32_t, 3> input{LateBdaInputWord, static_cast<uint32_t>(LateBdaFringe),
                                      static_cast<uint32_t>(LateBdaFringe >> 32u)};
    std::memcpy(reinterpret_cast<void*>(LateBdaInput), input.data(), sizeof(input));
    std::memcpy(reinterpret_cast<void*>(LateBdaFringe), &LateBdaFringeWord, sizeof(uint32_t));
    auto* output = reinterpret_cast<uint32_t*>(LateBdaOutput);
    std::fill_n(output, 6u, LateBdaCanary);
    output[4] = 0u;
    auto& context = vulkan.RuntimeRenderer();
    auto& scheduler = context.GetCommandScheduler();
    auto& cache = context.GetBufferCache();
    context.MapMemory(FetchGpuGuest::Base, FetchGpuGuest::Bytes);
    HW::Context registers{};
    HW::UserConfig user_config{};
    HW::Shader shaders{};
    scheduler.Begin(registers, user_config, shaders);
    // A genuine prior GPU write, not a manual late Register/FindBuffer. Only
    // pageA exists: the new32KiB owner/pageB must arise from RebindImages.
    const auto [producer, producer_offset] = cache.ObtainBuffer(LateBdaImage, 4u, true);
    scheduler.Current().Handle().fillBuffer(producer->Handle(), producer_offset, 4u, LateBdaImageWord);
    cache.OnCommandRecorded();
    LateBdaCompilation shader;
    ShaderStageRuntime runtime{&shader.info, &shader.resources};
    PreparedBindings prepared;
    LateBdaNativePipeline pipeline(vulkan, shader);
    FetchGpuBuffer download(vulkan.RuntimeContext(), 44u, vk::BufferUsageFlagBits::eTransferDst);
    auto& executor = context.GetRenderExecutor();
    executor.PrepareBindings(runtime, prepared);
    Require(name, "actual bounded linear image span", prepared.images.size() == 1u &&
                prepared.images[0].desc.info.data.address == LateBdaImage &&
                prepared.images[0].desc.info.data.size == 8192u,
            "texture descriptor does not have the intended real8KiB span; not a residency RED");
    executor.FindBuffers(prepared);
    context.PrepareBda();
    absent_after_prepare = cache.PeekPage(LateBdaFringe).first == nullptr;
    const auto pte_offset = BufferCache::PageIndex(LateBdaFringe) * 8u;
    CopyEopNativeLabel(scheduler, *cache.GetBdaPageTableBuffer(), pte_offset, download, 24u, 8u);
    std::array<PreparedBindings*, 1> stages{&prepared};
    // Invoke the production preparation phase, including its real image/cache
    // acquisition order, rather than duplicate that phase in the test.
    RenderExecutorTestAccess::PrepareGraphicsBindings(executor, stages);
    const auto [fringe_buffer, fringe_offset] = cache.PeekPage(LateBdaFringe);
    late_owner = fringe_buffer != nullptr;
    if (late_owner) {
      vk::BufferDeviceAddressInfo address{};
      address.buffer = fringe_buffer;
      expected_pte = vulkan.Device().getBufferAddress(address) + fringe_offset;
    }
    CopyEopNativeLabel(scheduler, *cache.GetBdaPageTableBuffer(), pte_offset, download, 32u, 8u);
    executor.CommitBindings(scheduler.Current(), vk::PipelineBindPoint::eCompute, pipeline.native, stages);
    scheduler.Current().Handle().bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.native.pipeline);
    scheduler.Current().Handle().dispatch(1u, 1u, 1u);
    cache.OnCommandRecorded();
    for (uint32_t resource = 0u; resource < shader.resources.buffers.size(); ++resource) {
      const auto& descriptor = shader.resources.buffers[resource].dwords;
      const uint64_t address = descriptor[0] | (uint64_t{descriptor[1] & 0xffffu} << 32u);
      if (address != LateBdaOutput) continue;
      const auto& view = prepared.buffers.at(resource);
      vk::MemoryBarrier done{};
      done.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
      done.dstAccessMask = vk::AccessFlagBits::eTransferRead;
      auto command = scheduler.Current().Handle();
      command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eTransfer,
          {}, 1u, &done, 0u, nullptr, 0u, nullptr);
      const vk::BufferCopy copy{view.offset, 0u, sizeof(actual)};
      command.copyBuffer(view.buffer, download.buffer, 1u, &copy);
    }
    const auto [marker_buffer, marker_offset] = cache.PeekPage(LateBdaImage);
    Require(name, "actual prior producer owner", marker_buffer != nullptr, "actual GPU source owner is absent");
    auto command = scheduler.Current().Handle();
    const vk::BufferCopy marker_copy{marker_offset + (LateBdaImage & (BufferCache::CACHING_PAGESIZE - 1u)), 40u, 4u};
    command.copyBuffer(marker_buffer, download.buffer, 1u, &marker_copy);
    vk::MemoryBarrier host{};
    host.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    host.dstAccessMask = vk::AccessFlagBits::eHostRead;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
        {}, 1u, &host, 0u, nullptr, 0u, nullptr);
    const auto tick = scheduler.CurrentTick();
    scheduler.Finish();
    scheduler.WaitPriorityOperations(tick);
    download.Invalidate();
    std::memcpy(actual.data(), download.data, sizeof(actual));
    std::memcpy(&before_pte, static_cast<const uint8_t*>(download.data) + 24u, 8u);
    std::memcpy(&after_pte, static_cast<const uint8_t*>(download.data) + 32u, 8u);
    std::memcpy(&actual_source_marker, static_cast<const uint8_t*>(download.data) + 40u, 4u);
    // Retire actual GPU-owned synthetic bytes through the existing ordered
    // publication path. Publish OBSERVED data, never replace it with goldens.
    cache.WriteOrderedMemory(LateBdaImage,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&actual_source_marker), sizeof(actual_source_marker)});
    cache.WriteOrderedMemory(LateBdaOutput,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(actual.data()), sizeof(actual)});
    const auto cleanup_tick = scheduler.CurrentTick();
    scheduler.Finish();
    scheduler.WaitPriorityOperations(cleanup_tick);
    context.UnmapMemory(FetchGpuGuest::Base, FetchGpuGuest::Bytes);
  }
  std::printf("[late-bda] absentAfterPrepare=%u lateOwner=%u PTEbefore=%llx PTEafter=%llx expectedPTE=%llx output=%08x,%08x,%08x,%08x,%08x,%08x source=%08x\n",
      absent_after_prepare, late_owner, static_cast<unsigned long long>(before_pte),
      static_cast<unsigned long long>(after_pte), static_cast<unsigned long long>(expected_pte),
      actual[0], actual[1], actual[2], actual[3], actual[4], actual[5], actual_source_marker);
  Require(name, "actual late ownership/PTE positive control", absent_after_prepare && late_owner &&
              before_pte == 0u && after_pte == expected_pte && after_pte != 0u,
          "real acquisition path did not create the intended late owner; not a residency RED");
  Require(name, "ordinary SRD/image/counter/sentinel controls",
          actual[0] == LateBdaInputWord && actual[2] == LateBdaImageWord && actual[3] == 0u &&
              actual[4] == 1u && actual[5] == LateBdaCanary && actual_source_marker == LateBdaImageWord,
          "ordinary data, real image or once-only dispatch witness failed; not the intended fringe RED");
  Require(name, "first real FLAT fringe read", actual[1] == LateBdaFringeWord,
          "new aligned BDA owner exposed a mapped nonzero guest fringe before its bytes were synchronized");
}
} // namespace
} // namespace Libs::Graphics
