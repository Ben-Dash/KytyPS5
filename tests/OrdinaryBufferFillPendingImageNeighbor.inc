// Include INSIDE VulkanHarness after OrdinaryBufferNativeFillCostTests.inc.
// Actual pending image OUTSIDE fill bytes on SAME tracker page must reject ordinary CPU consume.
void CheckOrdinaryBufferFillPendingImageNeighbor() {
  constexpr const char* name="OrdinaryBufferFillPendingImageNeighbor";
  constexpr uint64_t base=0x0000000208e00000ull,bytes=0x10000u;
  constexpr u32 prefix=16u,words=64u,sentinel=0xdeadbeefu,fill=0x11223344u;
  constexpr uint64_t image_address=base+0x800u;
  constexpr u32 image_value=0x2468ace0u;
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
  struct Gate {
    std::mutex mutex;std::condition_variable cv;
    bool entered=false,released=false,timed_out=false;
    void Hold() {
      std::unique_lock lock(mutex);entered=true;cv.notify_all();
      if(!cv.wait_for(lock,std::chrono::seconds(2),[&]{return released;}))timed_out=true;
    }
    void Release() {{std::lock_guard lock(mutex);released=true;}cv.notify_all();}
  } gate;
  bool image_registered=false,image_download=false,image_native=false,pending_page=false;
  bool pending_not_exact=false,image_backing_old=false,image_final=false,gate_ok=false;
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
    auto& textures=context.GetTextureCache();
    TextureCache::ImageDesc desc{};desc.type=TextureCache::BindingType::Texture;
    desc.info.data={image_address,4u};desc.info.pixel_format=vk::Format::eR32Uint;
    desc.info.guest_format=Prospero::BufferFormat::k32UInt;desc.info.type=Prospero::ImageType::kColor2D;
    desc.info.extent={1u,1u,1u};desc.info.resources={1u,1u};desc.info.pitch=1u;
    desc.info.bytes_per_block=4u;desc.info.samples=1u;desc.info.tile_mode=Prospero::TileMode::kLinear;
    desc.info.mip_layout[0]={0u,4u,1u,1u};desc.view_info.format=desc.info.pixel_format;
    desc.view_info.type=vk::ImageViewType::e2D;desc.view_info.aspect=vk::ImageAspectFlagBits::eColor;
    desc.view_info.usage=vk::ImageUsageFlagBits::eSampled;desc.view_info.level_count=desc.view_info.layer_count=1u;
    const auto image=textures.FindImage(desc);const auto view=textures.FindTexture(image,desc);
    image_registered=view!=nullptr&&textures.PeekImage(image)!=nullptr&&textures.PeekImage(image)->registered;
    auto image_probe=CreateHostBuffer(name,4u,vk::BufferUsageFlagBits::eTransferDst,{});
    scheduler.EndRendering();const auto command=scheduler.Current().Handle();auto& native=textures.GetImage(image);
    native.Transit(vk::ImageLayout::eTransferDstOptimal,vk::AccessFlagBits2::eTransferWrite,{},command);
    vk::ClearColorValue clear{};std::fill_n(clear.uint32.data(),4u,image_value);
    const vk::ImageSubresourceRange image_range{vk::ImageAspectFlagBits::eColor,0u,1u,0u,1u};
    command.clearColorImage(native.backing.image,vk::ImageLayout::eTransferDstOptimal,&clear,1u,&image_range);
    textures.MarkGpuWritten(image);
    vk::BufferImageCopy image_copy{};image_copy.imageSubresource={vk::ImageAspectFlagBits::eColor,0u,0u,1u};
    image_copy.imageExtent={1u,1u,1u};
    native.Download(std::span<const vk::BufferImageCopy>(&image_copy,1u),image_probe.buffer,0u,4u,4u);
    vk::MemoryBarrier host_image{};host_image.srcAccessMask=vk::AccessFlagBits::eMemoryWrite;
    host_image.dstAccessMask=vk::AccessFlagBits::eHostRead;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,vk::PipelineStageFlagBits::eHost,
      {},1u,&host_image,0u,nullptr,0u,nullptr);
    scheduler.DeferPriorityOperation([&]{gate.Hold();});
    // Actual image staging/publication, not a fabricated pending range or tracker flag.
    {auto image_lock=TextureCacheTestAccess::Lock(textures);image_download=TextureCacheTestAccess::TryDownload(textures,image);}
    const auto image_tick=scheduler.CurrentTick();scheduler.Flush();scheduler.GetMasterSemaphore().Wait(image_tick);
    {std::unique_lock lock(gate.mutex);gate_ok=gate.cv.wait_for(lock,std::chrono::seconds(2),[&]{return gate.entered;});}
    image_native=ReadBuffer(name,image_probe,1u)[0]==image_value;
    pending_page=textures.HasPendingDownload(base,TRACKER_PAGE_SIZE);
    pending_not_exact=!textures.HasPendingDownload(base+prefix,words*4u)&&
      !textures.IsRegionRegistered(base+prefix,words*4u);
    uint32_t pending_image=0;
    image_backing_old=LibKernel::Memory::TryReadBacking(image_address,&pending_image,4u)&&pending_image==sentinel;
    clean_before=clean_before&&!cache.IsRegionGpuModified(base+prefix,words*4u);
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
      std::ranges::all_of(cpu,[](u32 v){return v==sentinel;}); // native fallback remains GPU-owned
    // Native consumer uses the real BDA resident backing after the normal epoch/upload.
    context.PrepareBda();const auto peek=cache.PeekPage(base);
    owner_consistent=peek.first==owner.Handle()&&peek.second==owner.Offset(base);
    copy_native(new_probe);gate.Release();scheduler.Finish();
    scheduler.WaitPriorityOperations(scheduler.CurrentTick()-1u);
    cache.ReadMemory(base,128u*4u);scheduler.Finish();scheduler.DrainPriorityOperations();
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
    uint32_t published_image=0;
    image_final=LibKernel::Memory::TryReadBacking(image_address,&published_image,4u)&&published_image==image_value&&
      !textures.HasPendingDownload(base,TRACKER_PAGE_SIZE);
    {std::lock_guard lock(gate.mutex);gate_ok=gate_ok&&gate.entered&&gate.released&&!gate.timed_out;}
    scheduler.Finish();scheduler.DrainPriorityOperations();
    DestroyBuffer(&old_probe);DestroyBuffer(&new_probe);DestroyBuffer(&image_probe);
    context.UnmapMemory(base,bytes);
    scheduler.Finish();scheduler.DrainPriorityOperations();
    image_final=image_final&&!textures.IsRegionRegistered(image_address,4u);
  });
  context.ShutdownGpu();LibKernel::Memory::InstallGpuResources(nullptr);
  Require(name,"owned unmap",LibKernel::Memory::KernelMunmap(base,bytes)==0,"unmap failed");
  Require(name,"owned release",LibKernel::Memory::KernelReleaseDirectMemory(direct,bytes)==0,"release failed");
  std::printf("[ordinary-fill-pending-image] image=%u native=%u actualDownload=%u pendingPage=%u pendingNotExact=%u oldImage=%u finalImage=%u gate=%u\n",
    image_registered,image_native,image_download,pending_page,pending_not_exact,image_backing_old,image_final,gate_ok);
  std::printf("[ordinary-fill-cpu-bda] setup=%u warm=%u cleanBefore=%u epoch=%llu->%llu advanced=%u cpuFill=%u oldReader=%u newReader=%u canaries=%u BDAowner=%u cleanAfter=%u GameDispatch=%u\n",
    setup,warm_native,clean_before,static_cast<unsigned long long>(epoch_before),
    static_cast<unsigned long long>(epoch_after),epoch_advanced,cpu_fill,old_reader,new_reader,canaries,
    owner_consistent,clean_after,counts.dispatch);
  Require(name,"real cached old/new consumers, CPU ownership, BDA epoch/upload and cleanup",
    setup&&warm_native&&clean_before&&cpu_fill&&old_reader&&new_reader&&canaries&&
    owner_consistent&&clean_after&&image_registered&&image_native&&image_download&&pending_page&&
    pending_not_exact&&image_backing_old&&image_final&&gate_ok,
    "real pending image/producer/old backing/fallback/output/canary/cleanup control failed");
  Require(name,"pending page alias preserves normal native compute fallback",counts.dispatch==1u,
    "ordinary fill consumed despite a real pending image alias outside its bytes on the same page");
}
