// Include after FirstUseBdaPointerChainTests.inc. This synthetic decoded shader
// never reads SMEM/SRT data: only the address operands identify its DMA base.
// FirstUseBdaNativePipeline's constructor must be templated over Compilation.
namespace Libs::Graphics {
namespace {
constexpr uint64_t FirstDmaBase = FetchGpuGuest::Base + 0x1ffcu;
constexpr uint64_t FirstDmaLast = FirstDmaBase + 0x3ffcu;
constexpr uint64_t FirstDmaControl = FetchGpuGuest::Base + 0x8000u;
constexpr uint64_t FirstDmaOutput = FetchGpuGuest::Base + 0x9000u;
constexpr uint64_t FirstDmaDecoy = FetchGpuGuest::Base + 0xd000u;
constexpr uint64_t FirstDmaDirty = FetchGpuGuest::Base + 0x4100u;
constexpr uint32_t FirstDmaStartWord = 0x521749abu;
constexpr uint32_t FirstDmaLastWord = 0x789acdefu;
constexpr uint32_t FirstDmaMarker = 0x12345678u;
constexpr uint32_t FirstDmaCanary = 0xaabbccddu;
constexpr uint32_t FirstDmaGpuMarker = 0x89abcdefu;

struct FirstUseDmaBaseCompilation {
  ShaderComputeInputInfo compute{};
  std::array<uint32_t,12> userdata{};
  std::vector<uint32_t> code;
  std::vector<std::pair<uint64_t,uint64_t>> cpu_reads;
  FetchGpuIR::ResourceSnapshot resources;
  ShaderRecompiler::CompileResult compiled;
  FetchGpuIR::CompiledShaderInfo info;
  static bool Read(void* opaque, uint64_t address, std::span<uint32_t> words) {
    static_cast<FirstUseDmaBaseCompilation*>(opaque)->cpu_reads.emplace_back(address,words.size_bytes());
    return ReadFetchGpuGuest(nullptr,address,words);
  }
  FirstUseDmaBaseCompilation() {
    userdata[0]=static_cast<uint32_t>(FirstDmaBase);
    userdata[1]=static_cast<uint32_t>(FirstDmaBase>>32u);
    // This valid mapped pointer is not an address operand. Arbitrary userdata
    // scanning would unnecessarily give its fourth16KiB owner residency.
    userdata[2]=static_cast<uint32_t>(FirstDmaDecoy);
    userdata[3]=static_cast<uint32_t>(FirstDmaDecoy>>32u);
    const auto control=FetchGpuDescriptor(FirstDmaControl,0u,4u);
    const auto output=FetchGpuDescriptor(FirstDmaOutput,0u,32u);
    std::copy(control.begin(),control.end(),userdata.begin()+4u);
    std::copy(output.begin(),output.end(),userdata.begin()+8u);
    const auto mov=[](uint32_t dst,uint32_t src) {
      return (0x3fu<<25u)|(dst<<17u)|(1u<<9u)|src;
    };
    const auto store=[](uint32_t offset,uint32_t data) {
      return std::array<uint32_t,2>{(0x38u<<26u)|(0x1cu<<18u)|offset,
        (128u<<24u)|(2u<<16u)|(data<<8u)};
    };
    code={
      (0x3eu<<25u)|(0xd2u<<17u)|128u, // CMPX_EQ_U32 0,v0: one active invocation
      (0x1au<<25u)|(1u<<17u)|130u, // V_LSHLREV_B32 v1,2,v0
      (0x25u<<25u)|(1u<<17u)|(1u<<9u), // V_ADD_NC_U32 v1,s0,v1
      mov(2u,1u), // V_MOV_B32 v2,s1
      (0x37u<<26u)|(0x0cu<<18u),(3u<<24u)|(127u<<16u)|1u, // FLAT_LOAD_DWORD v3,v[1:2]
      mov(4u,255u),0x3ffcu, // V_MOV_B32 v4,last DWORD offset
      (0x25u<<25u)|(4u<<17u)|(4u<<9u)|257u, // V_ADD_NC_U32 v4,v1,v4
      mov(5u,258u), // V_MOV_B32 v5,v2
      (0x37u<<26u)|(0x0cu<<18u),(6u<<24u)|(127u<<16u)|4u, // FLAT_LOAD_DWORD v6,v[4:5]
      (0x38u<<26u)|(0x0cu<<18u),(128u<<24u)|(1u<<16u)|(7u<<8u), // ordinary control
      mov(8u,129u), // atomic increment1
      (0x38u<<26u)|(0x32u<<18u)|(1u<<14u)|20u,
      (128u<<24u)|(2u<<16u)|(8u<<8u)};
    for(const auto [offset,data]:std::array<std::array<uint32_t,2>,4>{{{0u,7u},{4u,3u},{8u,6u},{16u,8u}}}) {
      const auto encoded=store(offset,data);
      code.insert(code.end(),encoded.begin(),encoded.end());
    }
    code.push_back(0xbf810000u);
    compute.threads_num[0]=32u; compute.threads_num[1]=compute.threads_num[2]=1u;
    compute.dispatch_threads_num[0]=32u;
    compute.dispatch_threads_num[1]=compute.dispatch_threads_num[2]=1u;
    compute.thread_ids_num=1u;
    compute.wave_size=compute.host_subgroup_size=32u;
    ShaderRecompiler::CompileOptions options;
    options.stage=ShaderType::Compute; options.wave_size=32u;
    options.user_data=userdata; options.input_info.compute=&compute; options.dump_ir=false;
    auto translated=ShaderRecompiler::TranslateProgram(code,options);
    Require("FirstUseDmaBase","actual synthetic translation",!translated.skip_dispatch,
      "setup translation failed; not the intended DMA-base RED");
    auto plan=FetchGpuIR::ExtractResourcePlan(translated.program);
    FetchGpuIR::ResourceSpecialization specialization;
    const FetchGpuIR::SrtRuntime runtime{.user_data=userdata,
      .shader_base=reinterpret_cast<uint64_t>(code.data()),.read_memory=Read,
      .userdata=this,.read_specialization_memory=Read};
    Require("FirstUseDmaBase","noCPUmetadata read",
      FetchGpuIR::MaterializeResources(plan,runtime,resources,specialization) &&
      cpu_reads.empty() && plan.srt_reads.empty() && resources.srt_memory_reads.empty(),
      "fixture has CPU/SRT reads; it would not isolate direct DMA-base residency");
    compiled=ShaderRecompiler::CompileProgram(std::move(translated),options,specialization);
    spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_3);
    Require("FirstUseDmaBase","live dynamic DMA plus ordinary resources",
      validator.Validate(compiled.spirv) && compiled.program.info.uses_dma &&
      compiled.program.info.buffers.size()==2u && compiled.program.info.images.empty() &&
      !compiled.program.bindings.table,
      "fixture lost actual FLAT operations; not the intended DMA-base RED");
    uint32_t reads=0u;
    for(const auto* block:compiled.program.blocks)
      for(const auto& inst:*block)
        if(inst.GetOpcode()==FetchGpuIR::ValueOpcode::LoadAddressU32 &&
           compiled.program.memory_info[inst.Flags<FetchGpuIR::MemoryFlags>().index].kind==FetchGpuIR::ResourceKind::Flat)
          ++reads;
    Require("FirstUseDmaBase","two live directFLATreads",reads==2u,
      "both window endpoints must remain decoded FLAT accesses");
    for(size_t w=5u;w<compiled.spirv.size();w+=compiled.spirv[w]>>16u)
      Require("FirstUseDmaBase","noGPUloops",(compiled.spirv[w]&0xffffu)!=spv::OpLoopMerge,
        "fixture is not bounded");
    info.stage=ShaderType::Compute; info.wave_size=32u;
    info.user_data_base=compiled.program.user_data_base;
    info.user_data_count=compiled.program.user_data_count;
    info.info=compiled.program.info; info.bindings=compiled.program.bindings;
  }
};

void ConfigureFirstUseDmaBaseTests() { ConfigureFirstUseBdaPointerChainTests(); }

void CheckFirstUseDmaBaseGpu(VulkanHarness& vulkan,bool prior_gpu_marker=false) {
  const char* name=prior_gpu_marker ? "FirstUseDmaBaseGpuDirtyNeighbor" : "FirstUseDmaBase";
  std::array<uint32_t,8> actual{};
  std::array<uint64_t,4> ptes{};
  uint32_t actual_marker=0u;
  bool initial_base_absent=false,initial_last_absent=false;
  bool metadata_base_absent=false,metadata_last_absent=false;
  bool prepared_base_present=false,prepared_last_present=false,decoy_absent=false,marker_found=false;
  {
    FetchGpuGuest guest;
    std::memcpy(reinterpret_cast<void*>(FirstDmaBase),&FirstDmaStartWord,4u);
    std::memcpy(reinterpret_cast<void*>(FirstDmaLast),&FirstDmaLastWord,4u);
    std::memcpy(reinterpret_cast<void*>(FirstDmaControl),&FirstDmaMarker,4u);
    auto* output=reinterpret_cast<uint32_t*>(FirstDmaOutput);
    std::fill_n(output,8u,FirstDmaCanary); output[5]=0u; output[6]=0u;
    auto& context=vulkan.RuntimeRenderer();
    auto& scheduler=context.GetCommandScheduler();
    auto& cache=context.GetBufferCache();
    context.MapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    HW::Context registers{}; HW::UserConfig user_config{}; HW::Shader shaders{};
    scheduler.Begin(registers,user_config,shaders);
    initial_base_absent=cache.PeekPage(FirstDmaBase).first==nullptr;
    initial_last_absent=cache.PeekPage(FirstDmaLast).first==nullptr;
    if(prior_gpu_marker) {
      // The second window owner contains a disjoint alreadyGPU-written page.
      // Its CPU backing stayszero; prep must preserve the actual GPU producer.
      const auto [producer,offset]=cache.ObtainBuffer(FirstDmaDirty,4u,true);
      scheduler.Current().Handle().fillBuffer(producer->Handle(),offset,4u,FirstDmaGpuMarker);
      cache.OnCommandRecorded();
    }
    FirstUseDmaBaseCompilation shader;
    metadata_base_absent=cache.PeekPage(FirstDmaBase).first==nullptr;
    metadata_last_absent=cache.PeekPage(FirstDmaLast).first==nullptr;
    ShaderStageRuntime runtime{&shader.info,&shader.resources};
    PreparedBindings prepared;
    FirstUseBdaNativePipeline pipeline(vulkan,shader);
    FetchGpuBuffer download(vulkan.RuntimeContext(),prior_gpu_marker ? 68u : 64u,vk::BufferUsageFlagBits::eTransferDst);
    auto& executor=context.GetRenderExecutor();
    executor.PrepareBindings(runtime,prepared);
    executor.FindBuffers(prepared);
    context.PrepareBda();
    CopyEopNativeLabel(scheduler,*cache.GetBdaPageTableBuffer(),BufferCache::PageIndex(FirstDmaBase)*8u,download,32u,8u);
    CopyEopNativeLabel(scheduler,*cache.GetBdaPageTableBuffer(),BufferCache::PageIndex(FirstDmaLast)*8u,download,40u,8u);
    std::array<PreparedBindings*,1> stages{&prepared};
    RenderExecutorTestAccess::PrepareGraphicsBindings(executor,stages);
    prepared_base_present=cache.PeekPage(FirstDmaBase).first!=nullptr;
    prepared_last_present=cache.PeekPage(FirstDmaLast).first!=nullptr;
    decoy_absent=cache.PeekPage(FirstDmaDecoy).first==nullptr;
    CopyEopNativeLabel(scheduler,*cache.GetBdaPageTableBuffer(),BufferCache::PageIndex(FirstDmaBase)*8u,download,48u,8u);
    CopyEopNativeLabel(scheduler,*cache.GetBdaPageTableBuffer(),BufferCache::PageIndex(FirstDmaLast)*8u,download,56u,8u);
    executor.CommitBindings(scheduler.Current(),vk::PipelineBindPoint::eCompute,pipeline.native,stages);
    scheduler.Current().Handle().bindPipeline(vk::PipelineBindPoint::eCompute,pipeline.native.pipeline);
    scheduler.Current().Handle().dispatch(1u,1u,1u);
    cache.OnCommandRecorded();
    bool output_found=false;
    for(uint32_t i=0u;i<shader.resources.buffers.size();++i) {
      const auto& d=shader.resources.buffers[i].dwords;
      if((d[0]|(uint64_t{d[1]&0xffffu}<<32u))!=FirstDmaOutput)continue;
      const auto& view=prepared.buffers.at(i);
      auto command=scheduler.Current().Handle();
      vk::MemoryBarrier done{};
      done.srcAccessMask=vk::AccessFlagBits::eShaderWrite; done.dstAccessMask=vk::AccessFlagBits::eTransferRead;
      command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,vk::PipelineStageFlagBits::eTransfer,{},1u,&done,0u,nullptr,0u,nullptr);
      const vk::BufferCopy copy{view.offset,0u,sizeof(actual)};
      command.copyBuffer(view.buffer,download.buffer,1u,&copy); output_found=true;
    }
    Require(name,"actual ordinaryoutput binding",output_found,"setup failed; not a DMA-base RED");
    if(prior_gpu_marker) {
      const auto [buffer,page_offset]=cache.PeekPage(FirstDmaDirty);
      marker_found=buffer!=nullptr;
      if(marker_found) {
        auto command=scheduler.Current().Handle();
        vk::MemoryBarrier done{};
        done.srcAccessMask=vk::AccessFlagBits::eMemoryWrite; done.dstAccessMask=vk::AccessFlagBits::eTransferRead;
        command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,vk::PipelineStageFlagBits::eTransfer,{},1u,&done,0u,nullptr,0u,nullptr);
        const vk::BufferCopy copy{page_offset+(FirstDmaDirty&(BufferCache::CACHING_PAGESIZE-1u)),64u,4u};
        command.copyBuffer(buffer,download.buffer,1u,&copy);
      }
    }
    vk::MemoryBarrier host{};
    host.srcAccessMask=vk::AccessFlagBits::eTransferWrite; host.dstAccessMask=vk::AccessFlagBits::eHostRead;
    scheduler.Current().Handle().pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eHost,{},1u,&host,0u,nullptr,0u,nullptr);
    const auto tick=scheduler.CurrentTick();
    scheduler.Finish(); scheduler.WaitPriorityOperations(tick); download.Invalidate();
    std::memcpy(actual.data(),download.data,sizeof(actual));
    std::memcpy(ptes.data(),static_cast<const uint8_t*>(download.data)+32u,sizeof(ptes));
    if(prior_gpu_marker && marker_found)
      std::memcpy(&actual_marker,static_cast<const uint8_t*>(download.data)+64u,4u);
    // Cleanup publishes observedreadback only, never expectedbytes or replay.
    if(prior_gpu_marker)
      cache.WriteOrderedMemory(FirstDmaDirty,std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&actual_marker),4u});
    cache.WriteOrderedMemory(FirstDmaOutput,std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(actual.data()),sizeof(actual)});
    const auto cleanup_tick=scheduler.CurrentTick();
    scheduler.Finish(); scheduler.WaitPriorityOperations(cleanup_tick);
    context.UnmapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
  }
  std::printf("[first-dma] initialAbsence=%u,%u metadataAbsence=%u,%u preparedPresence=%u,%u decoyAbsent=%u PTE=%llx,%llx,%llx,%llx output=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x marker=%08x\n",
    initial_base_absent,initial_last_absent,metadata_base_absent,metadata_last_absent,
    prepared_base_present,prepared_last_present,decoy_absent,
    static_cast<unsigned long long>(ptes[0]),static_cast<unsigned long long>(ptes[1]),
    static_cast<unsigned long long>(ptes[2]),static_cast<unsigned long long>(ptes[3]),
    actual[0],actual[1],actual[2],actual[3],actual[4],actual[5],actual[6],actual[7],actual_marker);
  Require(name,"initialowner provenance",initial_base_absent && initial_last_absent && metadata_base_absent &&
    (prior_gpu_marker || metadata_last_absent),"fixture did not isolate initiallyuncached decodedbase; not intendedRED");
  Require(name,"ordinary/once/canary controls",actual[0]==FirstDmaMarker && actual[3]==FirstDmaCanary &&
    actual[4]==0u && actual[5]==1u && actual[6]==0u && actual[7]==FirstDmaCanary,
    "ordinarybuffer, exactlyonce atomic or guardcontrol failed");
  Require(name,"unusedmapped userdata pair staysuncached",decoy_absent,
    "basepreparation scanned unrelateduserdata instead of actualaddress operands");
  Require(name,"firstdispatch basewindow endpoints",actual[1]==FirstDmaStartWord && actual[2]==FirstDmaLastWord,
    "mapped DMA basewithout CPUmetadata reads missed its FIRSTdispatch window");
  Require(name,"basewindow preparedbefore dispatch",prepared_base_present && prepared_last_present && ptes[2]!=0u && ptes[3]!=0u,
    "firstdispatch unexpectedlypassed without intendedbasewindow residency");
  if(prior_gpu_marker)
    Require(name,"preserve GPUwrittenneighbor",marker_found && actual_marker==FirstDmaGpuMarker,
      "basewindow acquisitionlost the actualprior GPUproducer");
}
} // namespace
} // namespace Libs::Graphics
