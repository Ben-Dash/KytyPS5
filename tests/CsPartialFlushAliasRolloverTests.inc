// INSIDE ShaderRecompilerComputeTests.cpp VulkanHarness. Globally include the
// SEPARATE CsPartialFlushAliasBarrierAudit.inc (array4); do not alter the frozen cost observer.
// Add TextureCacheAliasedClearTestAccess.inc inside the existing friend test-access struct.
// Fresh OFF/ON processes; direct recorder, age0 before static policy getters. Root owns native run.
void CheckCsPartialFlushAliasRollover() {
  constexpr const char* name="CsPartialFlushAliasRollover";
  constexpr uint64_t base=0x0000000208e00000ull,allocation=0x20000u,image_address=base+0x10000u;
  constexpr u32 side=4u,layers=2u,texels=side*side*layers,guards=4u;
  // Render-target discovery pads the guest linear pitch. Own the complete padded
  // span while native logical readback still checks exactly4x4x2 texels.
  constexpr uint64_t guest_span=0x800u;
  constexpr u32 sentinel=0xdeadbeefu,initial=0x11223344u,final_fill=0x55667788u;
  const bool scoped=CsPartialFlushStagesEnabled();
  EnsureRuntimeContext();
  int64_t direct=-1;
  Require(name,"owned direct allocation",LibKernel::Memory::KernelAllocateDirectMemory(0,
    LibKernel::Memory::KernelGetDirectMemorySize(),allocation,0x10000u,0,&direct)==0,
    "owned allocation failed; not a producer-stage failure");
  void* mapped=reinterpret_cast<void*>(base);
  Require(name,"owned direct mapping",LibKernel::Memory::KernelMapDirectMemory(&mapped,
    allocation,0x3,0x10,direct,0x10000u)==0&&mapped==reinterpret_cast<void*>(base),
    "owned mapping failed; not a producer-stage failure");
  std::fill_n(static_cast<u32*>(mapped),allocation/4u,sentinel);
  std::array<Buffer,4> probes{};
  const std::vector<u32> probe_initial(texels+guards*2u,sentinel);
  for(auto& probe:probes)probe=CreateHostBuffer(name,probe_initial.size()*4u,
    vk::BufferUsageFlagBits::eTransferDst,probe_initial);
  std::array<CsPartialFlushAliasBarrierAudit::Observed,4> observed{};
  u32 event_count=0;
  bool valid_image=false,real_alias_view=false,wrapper_stable=false,submitted=false;
  uint64_t cache_full=0;
  RenderContext context(m_runtime_context);context.InitializeGpu(nullptr);
  LibKernel::Memory::InstallGpuResources(&context);context.MapMemory(base,allocation);
  context.GetGpu().SendCommandSync([&] {
    auto& scheduler=context.GetCommandScheduler();auto& cache=context.GetTextureCache();
    HW::Context registers{};HW::UserConfig config{};HW::Shader shaders{};
    scheduler.Begin(registers,config,shaders);
    CommandProcessor boundary(context,0x31u);
    auto desc=MakeLinearDesc(image_address,guest_span,vk::Format::eR32Uint,
      Prospero::BufferFormat::k32UInt,Prospero::ImageType::kColor2D,{side,side,1u},layers,4u,1u);
    desc.type=TextureCache::BindingType::RenderTarget;
    desc.view_info.usage=vk::ImageUsageFlagBits::eColorAttachment;
    const auto id=cache.FindImage(desc);
    Require(name,"actual mutable-format image and render-target setup",bool(id)&&
      cache.FindRenderTarget(id,desc)!=nullptr,"real image/render target did not materialize");
    auto& image=cache.GetImage(id);
    valid_image=image.backing.format==vk::Format::eR32Uint&&image.backing.layers==layers&&
      image.info.data.address==image_address&&image.info.data.size==guest_span;
    Require(name,"actual intended native backing",valid_image,"image geometry/format changed");
    ImageViewInfo alias{};alias.format=vk::Format::eR32Sfloat;alias.type=vk::ImageViewType::e2D;
    alias.aspect=vk::ImageAspectFlagBits::eColor;alias.level_count=1u;alias.layer_count=1u;
    alias.usage=vk::ImageUsageFlagBits::eColorAttachment;
    real_alias_view=image.FindView(alias)!=nullptr;
    Require(name,"real different-format compatible attachment view",real_alias_view,
      "alias view was unsupported; not a semantic RED");
    const vk::ImageSubresourceRange all{vk::ImageAspectFlagBits::eColor,0u,1u,0u,layers};
    const auto ReadNative=[&](u32 index) {
      vk::BufferImageCopy copy{};copy.bufferOffset=guards*4u;
      copy.imageSubresource={vk::ImageAspectFlagBits::eColor,0u,0u,layers};
      copy.imageExtent={side,side,1u};
      image.Download(std::span<const vk::BufferImageCopy>{&copy,1u},probes[index].buffer,
        0u,probes[index].size,probes[index].size);
      vk::MemoryBarrier host{};host.srcAccessMask=vk::AccessFlagBits::eTransferWrite;
      host.dstAccessMask=vk::AccessFlagBits::eHostRead;
      scheduler.Current().Handle().pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eHost,{},1u,&host,0u,nullptr,0u,nullptr);
    };
    CsPartialFlushAliasBarrierAudit audit;
    const auto Cs=[&] {
      audit.capture_partial=true;boundary.TriggerEvent(7u,4u,0u);audit.capture_partial=false;
      Require(name,"CS flush leaves no unconsumed compute chain",!scheduler.Current().ComputeChainPending(),
        "partial barrier did not cover pending chain state");
    };
    vk::ClearValue clear{};clear.color.uint32[0]=initial;
    TextureCacheTestAccess::ClearImage(cache,scheduler.Current(),id,all,clear);
    Cs();ReadNative(0u); // Actual TRANSFER producer, no color attachment substitution.
    clear={};clear.color.float32[0]=0.5f;
    TextureCacheTestAccess::CsAliasedClear(cache,scheduler.Current(),id,vk::Format::eR32Sfloat,
      {vk::ImageAspectFlagBits::eColor,0u,1u,0u,1u},clear);
    Cs();ReadNative(1u); // Actual ClearImage rendering loadOp clear on layer0.
    clear={};clear.color.float32[0]=0.25f;
    TextureCacheTestAccess::CsAliasedClear(cache,scheduler.Current(),id,vk::Format::eR32Sfloat,
      {vk::ImageAspectFlagBits::eColor,0u,1u,1u,1u},clear);
    auto* old_wrapper=&scheduler.Current();const auto previous_tick=scheduler.CurrentTick();
    scheduler.Flush(); // Real submit + BeginNext before the CS event.
    wrapper_stable=&scheduler.Current()==old_wrapper;
    submitted=scheduler.CurrentTick()>previous_tick;
    // Do not require different Vk handle values: lawful pool reuse after completion is possible.
    Cs();ReadNative(2u); // Color history must survive the real native-buffer rollover.
    clear={};clear.color.uint32[0]=final_fill;
    TextureCacheTestAccess::ClearImage(cache,scheduler.Current(),id,all,clear);
    Cs();ReadNative(3u); // Completed color epoch must not leak into this transfer-only CS event.
    const auto before_cache=audit.full_write_visibility;
    boundary.TriggerEvent(0x16u,0u,0u);cache_full=audit.full_write_visibility-before_cache;
    observed=audit.partial;event_count=audit.partial_count;
    scheduler.Finish();scheduler.WaitPriorityOperations(scheduler.CurrentTick()-1u);
    scheduler.DrainPriorityOperations();
    context.UnmapMemory(base,allocation);
  });
  context.ShutdownGpu();LibKernel::Memory::InstallGpuResources(nullptr);
  bool values=true,probe_guards=true;
  for(u32 pass=0;pass<probes.size();++pass) {
    const auto actual=ReadBuffer(name,probes[pass],texels+guards*2u);
    for(u32 i=0;i<actual.size();++i) {
      if(i<guards||i>=guards+texels)probe_guards&=actual[i]==sentinel;
      else {
        const auto layer=(i-guards)/(side*side);
        const auto expected=pass==0u?initial:pass==1u?(layer==0u?0x3f000000u:initial):
          pass==2u?(layer==0u?0x3f000000u:0x3e800000u):final_fill;
        values&=actual[i]==expected;
      }
    }
    DestroyBuffer(&probes[pass]);
  }
  Require(name,"guest mapping cleanup",LibKernel::Memory::KernelMunmap(base,allocation)==0,
    "owned guest mapping cleanup failed");
  Require(name,"direct allocation cleanup",LibKernel::Memory::KernelReleaseDirectMemory(direct,allocation)==0,
    "owned direct allocation cleanup failed");
  Require(name,"real alias clear/rollover/numerics/guards/full cache after cleanup",
    valid_image&&real_alias_view&&wrapper_stable&&submitted&&values&&probe_guards&&cache_full==1u,
    "native color/transfer values, lifetime/rollover or retained full-cache behavior failed");
  const auto C=VkPipelineStageFlags2(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
  const auto T=VkPipelineStageFlags2(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT);
  const auto Color=VkPipelineStageFlags2(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
  const auto All=VkPipelineStageFlags2(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
  bool stages=event_count==observed.size();
  for(u32 i=0;i<observed.size();++i) {
    const auto& e=observed[i];const auto expected=scoped?(C|T|((i==1u||i==2u)?Color:0u)):All;
    stages=stages&&e.source==expected&&(e.destination&All)!=0&&
      (e.source_access&VK_ACCESS_2_MEMORY_WRITE_BIT)!=0&&
      (e.destination_access&(VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT))==
        (VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT);
    std::printf("[cs-alias-rollover] scoped=%u event=%u src=%llx expected=%llx\n",scoped?1u:0u,i,
      static_cast<unsigned long long>(e.source),static_cast<unsigned long long>(expected));
  }
  Require(name,"actual transfer/color/rollover/consumed native source-stage epochs",stages,
    "actual native clear stage omitted, lost on rollover or leaked after covering flush");
}
