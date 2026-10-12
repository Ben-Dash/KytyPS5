// TEST-ONLY first-use mapped BDA pointer chain; include after the existing
// FetchGpuGuest/Buffer/Read helpers and CopyEopNativeLabel in the compute harness.
// No title bytes/addresses/hashes. CPU SMEM materialization observes BOTH roots;
// dynamic FLAT reads must see those mapped positive bytes on the FIRST dispatch.
#include "graphics/shader/shaderBindings.h"
namespace Libs::Graphics {
namespace {
constexpr uint64_t FirstBdaRoot = FetchGpuGuest::Base + 0x1000u;
constexpr uint64_t FirstBdaChild = FetchGpuGuest::Base + 0x5000u;
constexpr uint64_t FirstBdaControl = FetchGpuGuest::Base + 0x8000u;
constexpr uint64_t FirstBdaOutput = FetchGpuGuest::Base + 0xc000u;
constexpr uint32_t FirstBdaWord = 0x71335599u, FirstBdaMarker = 0x12345678u;
constexpr uint32_t FirstBdaCanary = 0xaabbccddu;
struct FirstUseBdaCompilation {
  ShaderComputeInputInfo compute{};
  std::array<uint32_t, 12> userdata{};
  std::vector<uint32_t> code;
  std::vector<std::pair<uint64_t,uint64_t>> cpu_reads;
  FetchGpuIR::ResourceSnapshot resources;
  ShaderRecompiler::CompileResult compiled;
  FetchGpuIR::CompiledShaderInfo info;
  static bool Read(void* opaque, uint64_t address, std::span<uint32_t> words) {
    if (!ReadFetchGpuGuest(nullptr, address, words)) return false;
    static_cast<FirstUseBdaCompilation*>(opaque)->cpu_reads.emplace_back(address, words.size_bytes());
    return true;
  }
  bool Covers(uint64_t address, uint64_t bytes) const {
    for (const auto& [start, size] : cpu_reads)
      if (start <= address && size >= bytes && address-start <= size-bytes) return true;
    return false;
  }
  FirstUseBdaCompilation() {
    userdata[0]=static_cast<uint32_t>(FirstBdaRoot);
    userdata[1]=static_cast<uint32_t>(FirstBdaRoot>>32u);
    const auto control=FetchGpuDescriptor(FirstBdaControl,0u,4u);
    const auto output=FetchGpuDescriptor(FirstBdaOutput,0u,32u);
    std::copy(control.begin(),control.end(),userdata.begin()+4u);
    std::copy(output.begin(),output.end(),userdata.begin()+8u);
    const auto mov=[](uint32_t dst,uint32_t src) {
      return (0x3fu<<25u)|(dst<<17u)|(1u<<9u)|src;
    };
    const auto smem=[](uint32_t op,uint32_t dst,uint32_t base) {
      return (0x3du<<26u)|(op<<18u)|(dst<<6u)|(base/2u);
    };
    const auto store=[](uint32_t offset) {
      return (0x38u<<26u)|(0x1cu<<18u)|offset;
    };
    code={
      smem(1u,12u,0u),(127u<<25u)|24u,   // SMEM root+24, null scalar offset
      smem(0u,14u,12u),(127u<<25u)|152u, // SMEM child+152, null scalar offset
      (0x3eu<<25u)|(0xd2u<<17u)|128u, // CMPX_EQ_U32 0,v0: one active invocation
      (0x1au<<25u), // replaced below with actual VOP2 shift encoding
      (0x25u<<25u)|(1u<<17u)|(1u<<9u), // V_ADD_NC_U32 v1,s0,v1
      mov(2u,1u),
      (0x37u<<26u)|(0x0du<<18u)|24u,(4u<<24u)|(127u<<16u)|1u,
      (0x37u<<26u)|(0x0cu<<18u)|152u,(7u<<24u)|(127u<<16u)|4u,
      (0x38u<<26u)|(0x0cu<<18u),(128u<<24u)|(1u<<16u)|(3u<<8u),
      mov(6u,129u),
      (0x38u<<26u)|(0x32u<<18u)|(1u<<14u)|20u,
      (128u<<24u)|(2u<<16u)|(6u<<8u),
      mov(10u,259u),mov(11u,14u),mov(12u,260u),mov(13u,261u),
      (0x38u<<26u)|(0x1eu<<18u),(128u<<24u)|(2u<<16u)|(10u<<8u),
      store(16u),(128u<<24u)|(2u<<16u)|(7u<<8u),
      store(24u),(128u<<24u)|(2u<<16u)|(6u<<8u),0xbf810000u};
    // V_LSHLREV_B32 v1,3,v0 preserves a genuinely varying root-address
    // expression in the resource planner; masking leaves only LocalX0 active.
    code[5]=(0x1au<<25u)|(1u<<17u)|131u;
    compute.threads_num[0]=32u; compute.threads_num[1]=compute.threads_num[2]=1u;
    compute.dispatch_threads_num[0]=32u;
    compute.dispatch_threads_num[1]=compute.dispatch_threads_num[2]=1u;
    compute.thread_ids_num=1u;
    compute.wave_size=compute.host_subgroup_size=32u;
    ShaderRecompiler::CompileOptions options;
    options.stage=ShaderType::Compute; options.wave_size=32u;
    options.user_data=userdata; options.input_info.compute=&compute; options.dump_ir=false;
    auto translated=ShaderRecompiler::TranslateProgram(code,options);
    Require("FirstUseBdaPointerChain","actual synthetic translation",!translated.skip_dispatch,
            "setup translation failed; not the intended residency RED");
    auto plan=FetchGpuIR::ExtractResourcePlan(translated.program);
    FetchGpuIR::ResourceSpecialization specialization;
    const FetchGpuIR::SrtRuntime runtime{.user_data=userdata,
      .shader_base=reinterpret_cast<uint64_t>(code.data()),.read_memory=Read,
      .userdata=this,.read_specialization_memory=Read};
    Require("FirstUseBdaPointerChain","actual successful CPU root/child reads",
      FetchGpuIR::MaterializeResources(plan,runtime,resources,specialization) &&
      Covers(FirstBdaRoot+24u,4u) && Covers(FirstBdaRoot+28u,4u) &&
      Covers(FirstBdaChild+152u,4u),
      "the real resource planner did not read both mapped ranges; not the intended residency RED");
    compiled=ShaderRecompiler::CompileProgram(std::move(translated),options,specialization);
    spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_3);
    Require("FirstUseBdaPointerChain","live dynamic BDA plus ordinary resources",
      validator.Validate(compiled.spirv) && compiled.program.info.uses_dma &&
      compiled.program.info.buffers.size()==2u && compiled.program.info.images.empty() &&
      !compiled.program.bindings.table,
      "fixture must retain actual FLAT operations; scalar-only lowering is not a residency RED");
    for(size_t w=5u;w<compiled.spirv.size();w+=compiled.spirv[w]>>16u)
      Require("FirstUseBdaPointerChain","no GPU loops",
        (compiled.spirv[w]&0xffffu)!=spv::OpLoopMerge,"fixture is not bounded");
    info.stage=ShaderType::Compute; info.wave_size=32u;
    info.user_data_base=compiled.program.user_data_base;
    info.user_data_count=compiled.program.user_data_count;
    info.info=compiled.program.info; info.bindings=compiled.program.bindings;
  }
};

class FirstUseBdaNativePipeline {
public:
  PipelineCache::Pipeline native{};
  template <typename Compilation>
  FirstUseBdaNativePipeline(VulkanHarness& vulkan, const Compilation& shader) : device(vulkan.Device()) {
    native.uses_push_descriptors = true;
    std::vector<vk::DescriptorSetLayoutBinding> bindings;
    uint32_t count = 0u;
    for (const auto& binding : shader.info.bindings.descriptors) {
      bindings.push_back({FetchGpuIR::NativeBinding(ShaderType::Compute, binding.kind),
          NativeDescriptorType(binding.kind), NativeDescriptorCount(binding),
          vk::ShaderStageFlagBits::eCompute, nullptr});
      count += NativeDescriptorCount(binding);
    }
    Require("FirstUseBdaPointerChain", "actual push descriptor budget",
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
  ~FirstUseBdaNativePipeline() {
    device.destroyPipeline(native.pipeline, nullptr);
    device.destroyShaderModule(module, nullptr);
    device.destroyPipelineLayout(native.pipeline_layout, nullptr);
    device.destroyDescriptorSetLayout(native.descriptor_set_layout, nullptr);
  }
private:
  vk::Device device;
  vk::ShaderModule module = nullptr;
};


void ConfigureFirstUseBdaPointerChainTests() {
  Require("FirstUseBdaPointerChain","pin observation profile",
    _putenv_s("KYTY_RECORD_THREAD","0")==0 && _putenv_s("KYTY_TABLE_MODE","0")==0 &&
    _putenv_s("KYTY_VTX_EXPLODE_CHECK","0")==0,"configuration failed; not a residency RED");
  ShaderRecompiler::SetTableMode(false);
  ShaderRecompiler::SetHardwareStorageBufferBounds(false);
}
void CheckFirstUseBdaPointerChainGpu(VulkanHarness& vulkan, bool prior_gpu_marker = false) {
  const char* name = prior_gpu_marker ? "FirstUseBdaPointerChainGpuDirtyNeighbor"
                                     : "FirstUseBdaPointerChain";
  constexpr uint64_t marker_address = FirstBdaChild + 0x1100u;
  constexpr uint32_t gpu_marker = 0x89abcdefu;
  uint32_t actual_marker = 0u;
  bool marker_owner_found = false;
  std::array<uint32_t,8> actual{};
  std::array<uint64_t,4> ptes{};
  bool initial_root_absent=false,initial_child_absent=false;
  bool metadata_root_absent=false,metadata_child_absent=false;
  bool prepared_root_absent=false,prepared_child_absent=false;
  {
    FetchGpuGuest guest;
    std::memcpy(reinterpret_cast<void*>(FirstBdaRoot+24u),&FirstBdaChild,8u);
    std::memcpy(reinterpret_cast<void*>(FirstBdaChild+152u),&FirstBdaWord,4u);
    std::memcpy(reinterpret_cast<void*>(FirstBdaControl),&FirstBdaMarker,4u);
    auto* output=reinterpret_cast<uint32_t*>(FirstBdaOutput);
    std::fill_n(output,8u,FirstBdaCanary); output[5]=0u;
    auto& context=vulkan.RuntimeRenderer();
    auto& scheduler=context.GetCommandScheduler();
    auto& cache=context.GetBufferCache();
    context.MapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    HW::Context registers{}; HW::UserConfig user_config{}; HW::Shader shaders{};
    scheduler.Begin(registers,user_config,shaders);
    initial_root_absent=cache.PeekPage(FirstBdaRoot).first==nullptr;
    initial_child_absent=cache.PeekPage(FirstBdaChild).first==nullptr;
    if (prior_gpu_marker) {
      // Same16KiB aligned owner as the CPU SMEM leaf, separate4KiB dirty page.
      // Its backing remainszero: only this real native GPU producer writes it.
      const auto [producer, producer_offset] = cache.ObtainBuffer(marker_address, 4u, true);
      scheduler.Current().Handle().fillBuffer(producer->Handle(), producer_offset, 4u, gpu_marker);
      cache.OnCommandRecorded();
    }
    FirstUseBdaCompilation shader; // real CPU SMEM materialization, no GPU acquisition
    metadata_root_absent=cache.PeekPage(FirstBdaRoot).first==nullptr;
    metadata_child_absent=cache.PeekPage(FirstBdaChild).first==nullptr;
    ShaderStageRuntime runtime{&shader.info,&shader.resources};
    PreparedBindings prepared;
    FirstUseBdaNativePipeline pipeline(vulkan,shader);
    FetchGpuBuffer download(vulkan.RuntimeContext(),prior_gpu_marker ? 68u : 64u,vk::BufferUsageFlagBits::eTransferDst);
    auto& executor=context.GetRenderExecutor();
    executor.PrepareBindings(runtime,prepared);
    executor.FindBuffers(prepared);
    context.PrepareBda();
    CopyEopNativeLabel(scheduler,*cache.GetBdaPageTableBuffer(),
      BufferCache::PageIndex(FirstBdaRoot)*8u,download,32u,8u);
    CopyEopNativeLabel(scheduler,*cache.GetBdaPageTableBuffer(),
      BufferCache::PageIndex(FirstBdaChild)*8u,download,40u,8u);
    std::array<PreparedBindings*,1> stages{&prepared};
    RenderExecutorTestAccess::PrepareGraphicsBindings(executor,stages);
    prepared_root_absent=cache.PeekPage(FirstBdaRoot).first==nullptr;
    prepared_child_absent=cache.PeekPage(FirstBdaChild).first==nullptr;
    CopyEopNativeLabel(scheduler,*cache.GetBdaPageTableBuffer(),
      BufferCache::PageIndex(FirstBdaRoot)*8u,download,48u,8u);
    CopyEopNativeLabel(scheduler,*cache.GetBdaPageTableBuffer(),
      BufferCache::PageIndex(FirstBdaChild)*8u,download,56u,8u);
    executor.CommitBindings(scheduler.Current(),vk::PipelineBindPoint::eCompute,pipeline.native,stages);
    scheduler.Current().Handle().bindPipeline(vk::PipelineBindPoint::eCompute,pipeline.native.pipeline);
    scheduler.Current().Handle().dispatch(1u,1u,1u);
    cache.OnCommandRecorded();
    bool output_found=false;
    for(uint32_t i=0u;i<shader.resources.buffers.size();++i) {
      const auto& d=shader.resources.buffers[i].dwords;
      const uint64_t address=d[0]|(uint64_t{d[1]&0xffffu}<<32u);
      if(address!=FirstBdaOutput)continue;
      const auto& view=prepared.buffers.at(i);
      auto command=scheduler.Current().Handle();
      vk::MemoryBarrier done{};
      done.srcAccessMask=vk::AccessFlagBits::eShaderWrite;
      done.dstAccessMask=vk::AccessFlagBits::eTransferRead;
      command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eTransfer,{},1u,&done,0u,nullptr,0u,nullptr);
      const vk::BufferCopy copy{view.offset,0u,sizeof(actual)};
      command.copyBuffer(view.buffer,download.buffer,1u,&copy);
      output_found=true;
    }
    Require(name,"actual output binding",output_found,
      "setup did not bind the ordinary output; not a residency RED");
    if (prior_gpu_marker) {
      const auto [marker_buffer, marker_page_offset] = cache.PeekPage(marker_address);
      marker_owner_found = marker_buffer != nullptr;
      if (marker_owner_found) {
        auto command = scheduler.Current().Handle();
        vk::MemoryBarrier marker_done{};
        marker_done.srcAccessMask = vk::AccessFlagBits::eMemoryWrite;
        marker_done.dstAccessMask = vk::AccessFlagBits::eTransferRead;
        command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
          vk::PipelineStageFlagBits::eTransfer, {}, 1u, &marker_done, 0u, nullptr, 0u, nullptr);
        const vk::BufferCopy marker_copy{
          marker_page_offset + (marker_address & (BufferCache::CACHING_PAGESIZE - 1u)), 64u, 4u};
        command.copyBuffer(marker_buffer, download.buffer, 1u, &marker_copy);
      }
    }
    vk::MemoryBarrier host{};
    host.srcAccessMask=vk::AccessFlagBits::eTransferWrite;
    host.dstAccessMask=vk::AccessFlagBits::eHostRead;
    scheduler.Current().Handle().pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
      vk::PipelineStageFlagBits::eHost,{},1u,&host,0u,nullptr,0u,nullptr);
    const auto tick=scheduler.CurrentTick();
    scheduler.Finish(); scheduler.WaitPriorityOperations(tick); download.Invalidate();
    std::memcpy(actual.data(),download.data,sizeof(actual));
    std::memcpy(ptes.data(),static_cast<const uint8_t*>(download.data)+32u,sizeof(ptes));
    if (prior_gpu_marker && marker_owner_found)
      std::memcpy(&actual_marker, static_cast<const uint8_t*>(download.data) + 64u, 4u);
    // Cleanup uses observed bytes only. No replay or golden publication.
    if (prior_gpu_marker)
      cache.WriteOrderedMemory(marker_address,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&actual_marker), sizeof(actual_marker)});
    cache.WriteOrderedMemory(FirstBdaOutput,
      std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(actual.data()),sizeof(actual)});
    const auto cleanup_tick=scheduler.CurrentTick();
    scheduler.Finish(); scheduler.WaitPriorityOperations(cleanup_tick);
    context.UnmapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
  }
  std::printf("[first-bda] initialAbsence=%u,%u metadataAbsence=%u,%u preparedAbsence=%u,%u PTE=%llx,%llx,%llx,%llx output=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
    initial_root_absent,initial_child_absent,metadata_root_absent,metadata_child_absent,
    prepared_root_absent,prepared_child_absent,
    static_cast<unsigned long long>(ptes[0]),static_cast<unsigned long long>(ptes[1]),
    static_cast<unsigned long long>(ptes[2]),static_cast<unsigned long long>(ptes[3]),
    actual[0],actual[1],actual[2],actual[3],actual[4],actual[5],actual[6],actual[7]);
  if (prior_gpu_marker)
    std::printf("[first-bda-neighbor] currentOwner=%u GPUmarker=%08x expected=%08x\n",
      marker_owner_found, actual_marker, gpu_marker);
  Require(name,"initial owner provenance",initial_root_absent && initial_child_absent,
    "test did not start with unowned mapped roots; not the intended first-use RED");
  Require(name,"ordinary/SMEM/once-only/canary controls",
    actual[0]==FirstBdaMarker && actual[1]==FirstBdaWord &&
    actual[5]==1u && actual[6]==0u && actual[7]==FirstBdaCanary,
    "ordinary, actual CPU-materialized SMEM, atomic-once or guard control failed");
  Require(name,"first-dispatch two-hop BDA values",
    actual[2]==static_cast<uint32_t>(FirstBdaChild) &&
    actual[3]==static_cast<uint32_t>(FirstBdaChild>>32u) && actual[4]==FirstBdaWord,
    "mapped roots read by SMEM materialization were not resident for their dynamic FLAT consumer");
  if (prior_gpu_marker)
    Require(name, "preserve actual GPU producer in acquired metadata owner",
      marker_owner_found && actual_marker == gpu_marker,
      "preacquiring the CPU-read root/child spans discarded or overwrote a disjoint GPU-dirty marker");
}
} // namespace
} // namespace Libs::Graphics
