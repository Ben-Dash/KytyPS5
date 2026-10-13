// Include INSIDE VulkanHarness after OrdinaryBufferNativeFillCostTests.inc.
// Actual old native reader -> consumed clean CPU fill -> normal BDA upload -> new native reader.
void CheckOrdinaryBufferFillCpuCleanBdaNeighbor() {
  constexpr const char* name="OrdinaryBufferFillCpuCleanBdaNeighbor";
  constexpr uint64_t base=0x0000000208e00000ull,bytes=0x10000u;
  constexpr u32 prefix=16u,words=64u,sentinel=0xdeadbeefu,fill=0x11223344u;
  EnsureRuntimeContext();
  int64_t direct=-1;
  Require(name,"owned direct allocation",LibKernel::Memory::KernelAllocateDirectMemory(
      0,LibKernel::Memory::KernelGetDirectMemorySize(),bytes,bytes,0,&direct)==0,
      "allocation failed, not a native behavior result");
  void* mapped=reinterpret_cast<void*>(base);
  Require(name,"owned mapping",LibKernel::Memory::KernelMapDirectMemory(&mapped,bytes,0x3,0x10,direct,bytes)==0&&
      mapped==reinterpret_cast<void*>(base),"mapping setup failure");
  std::fill_n(static_cast<u32*>(mapped),bytes/4u,sentinel);
  std::vector<u32> code;
  AppendVMovU32(&code,1,fill);
  code.push_back(EncodeVop1(0x01u,2,4));
  code.push_back(EncodeVop2(0x1au,2,InlineU32(6),2));
  code.push_back(EncodeVop2(0x25u,2,Vgpr(0),2));
  code.push_back(EncodeMubuf0(0x04u,0,true,false));
  code.push_back(EncodeMubuf1(1,0,2));AppendEnd(&code);
  ShaderMapUserData(reinterpret_cast<uint64_t>(code.data()),
    {.type=Prospero::ShaderBinaryType::kCs,.code_size_bytes=static_cast<u32>(code.size()*4u)});
  struct Counts {
    u32 dispatch=0;
    static void Mark(void* opaque,vk::CommandBuffer,DrainStats::Zone zone,uint64_t,uint64_t) {
      if(zone==DrainStats::Zone::GameDispatch)++static_cast<Counts*>(opaque)->dispatch;
    }
  } counts;
  bool setup=false,warm_native=false,clean_before=false,epoch_advanced=false,cpu_fill=false;
  bool old_reader=false,new_reader=false,canaries=false,owner_consistent=false,clean_after=false;
  uint64_t epoch_before=0,epoch_after=0;
  RenderContext context(m_runtime_context);context.InitializeGpu(nullptr);
  LibKernel::Memory::InstallGpuResources(&context);context.MapMemory(base,bytes);
  context.GetGpu().SendCommandSync([&] {
    auto& scheduler=context.GetCommandScheduler();auto& cache=context.GetBufferCache();
    HW::Context registers{};HW::UserConfig user_config{};HW::Shader shaders{};
    scheduler.Begin(registers,user_config,shaders);
    shaders.SetCsShader({.data_addr=reinterpret_cast<uint64_t>(code.data()),.num_thread_x=words,
      .num_thread_y=1,.num_thread_z=1,.wave_size=64,.user_sgpr=4,.tgid_x_en=true});
    ShaderBufferResource descriptor{};descriptor.UpdateAddress48(base+prefix);
    descriptor.fields[1]|=4u<<16u;descriptor.fields[2]=words;
    descriptor.fields[3]=DstSel(4,5,6,7)|(static_cast<u32>(Prospero::BufferFormat::k32UInt)<<12u);
    for(u32 i=0;i<4;++i)shaders.SetCsUserSgpr(i,descriptor.fields[i],HW::UserSgprType::Unknown);
    ShaderComputeInputInfo info{};
    const auto program=context.GetPipelineCache().GetComputeProgram(shaders.GetCs(),registers.GetShaderRegisters(),info);
    setup=bool(program)&&info.stage.resources!=nullptr&&
      info.stage.resources->uniform_fill.kind==ShaderRecompiler::IR::UniformFillKind::Buffer&&
      info.stage.resources->uniform_fill.uniform&&info.stage.resources->uniform_fill.value==fill&&
      context.GetPipelineCache().GetComputePipeline(info,program).pipeline!=nullptr;
    // Existing normal mapping/BDA preparation creates the real resident owner and uploads CPU data.
    const auto owner_id=cache.FindBuffer(base,bytes);
    context.PrepareBda();
    auto& owner=cache.GetBuffer(owner_id);
    auto old_probe=CreateHostBuffer(name,128u*4u,vk::BufferUsageFlagBits::eTransferDst,{});
    auto new_probe=CreateHostBuffer(name,128u*4u,vk::BufferUsageFlagBits::eTransferDst,{});
    const auto copy_native=[&](const auto& probe) {
      scheduler.EndRendering();const auto command=scheduler.Current().Handle();
      vk::MemoryBarrier before{};before.srcAccessMask=vk::AccessFlagBits::eMemoryWrite|vk::AccessFlagBits::eHostWrite;
      before.dstAccessMask=vk::AccessFlagBits::eTransferRead;
      command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands|vk::PipelineStageFlagBits::eHost,
        vk::PipelineStageFlagBits::eTransfer,{},1u,&before,0u,nullptr,0u,nullptr);
      const vk::BufferCopy copy{owner.Offset(base),0u,128u*4u};
      command.copyBuffer(owner.Handle(),probe.buffer,1u,&copy);
      vk::MemoryBarrier after{};after.srcAccessMask=vk::AccessFlagBits::eTransferWrite;
      after.dstAccessMask=vk::AccessFlagBits::eHostRead;
      command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eHost,
        {},1u,&after,0u,nullptr,0u,nullptr);
    };
    copy_native(old_probe);scheduler.Finish();
    const auto warm=ReadBuffer(name,old_probe,128u);
    warm_native=std::ranges::all_of(warm,[](u32 v){return v==sentinel;});
    clean_before=!cache.IsRegionGpuModified(base,128u*4u)&&
      !context.GetTextureCache().IsRegionRegistered(base,128u*4u)&&context.CurrentBdaEpoch()!=0u;
    // Queue an actual old reader. It executes before the later BDA upload, even though host
    // backing becomes new immediately through the legal clean CPU-fill policy.
    copy_native(old_probe);epoch_before=context.CurrentBdaEpoch();
    Require(name,"isolated marker",GpuZones::g_marker==nullptr,"another test marker is installed");
    {
      struct MarkerScope {~MarkerScope(){GpuZones::g_marker=nullptr;GpuZones::g_context=nullptr;}} marker;
      GpuZones::g_marker=Counts::Mark;GpuZones::g_context=&counts;
      context.GetRenderExecutor().DispatchDirect(0,scheduler.Current(),1,1,1,0x41u);
    }
    epoch_after=context.CurrentBdaEpoch();epoch_advanced=epoch_after>epoch_before;
    std::array<u32,words> cpu{};
    cpu_fill=LibKernel::Memory::TryReadBacking(base+prefix,cpu.data(),sizeof(cpu))&&
      std::ranges::all_of(cpu,[](u32 v){return v==fill;});
    // Native consumer uses the real BDA resident backing after the normal epoch/upload.
    context.PrepareBda();const auto peek=cache.PeekPage(base);
    owner_consistent=peek.first==owner.Handle()&&peek.second==owner.Offset(base);
    copy_native(new_probe);scheduler.Finish();
    const auto old_words=ReadBuffer(name,old_probe,128u);
    const auto new_words=ReadBuffer(name,new_probe,128u);
    old_reader=std::ranges::all_of(old_words,[](u32 v){return v==sentinel;});
    new_reader=true;canaries=true;
    for(u32 i=0;i<128u;++i) {
      const bool replaced=i>=prefix/4u&&i<prefix/4u+words;
      const auto expected=replaced?fill:sentinel;
      new_reader=new_reader&&new_words[i]==expected;
      if(!replaced)canaries=canaries&&new_words[i]==sentinel;
    }
    clean_after=!cache.IsRegionGpuModified(base,128u*4u);
    scheduler.Finish();scheduler.DrainPriorityOperations();
    DestroyBuffer(&old_probe);DestroyBuffer(&new_probe);
    context.UnmapMemory(base,bytes);
  });
  context.ShutdownGpu();LibKernel::Memory::InstallGpuResources(nullptr);
  Require(name,"owned unmap",LibKernel::Memory::KernelMunmap(base,bytes)==0,"unmap failed");
  Require(name,"owned release",LibKernel::Memory::KernelReleaseDirectMemory(direct,bytes)==0,"release failed");
  std::printf("[ordinary-fill-cpu-bda] setup=%u warm=%u cleanBefore=%u epoch=%llu->%llu advanced=%u cpuFill=%u oldReader=%u newReader=%u canaries=%u BDAowner=%u cleanAfter=%u GameDispatch=%u\n",
    setup,warm_native,clean_before,static_cast<unsigned long long>(epoch_before),
    static_cast<unsigned long long>(epoch_after),epoch_advanced,cpu_fill,old_reader,new_reader,canaries,
    owner_consistent,clean_after,counts.dispatch);
  Require(name,"real cached old/new consumers, CPU ownership, BDA epoch/upload and cleanup",
    setup&&warm_native&&clean_before&&epoch_advanced&&cpu_fill&&old_reader&&new_reader&&canaries&&
    owner_consistent&&clean_after,"clean CPU fill lost previous native readers or future BDA uploads");
  Require(name,"legal CPU clean ordinary fill replaces only proved kernel",counts.dispatch==0u,
    "CPU clean ordinary fill still executed redundant compute");
}
