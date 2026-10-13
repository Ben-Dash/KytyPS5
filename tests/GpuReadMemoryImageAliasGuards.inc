// Include after GpuReadMemoryExactPageCostTests.inc. Standalone/direct recording only.
// Functional neighboring controls: no production behavior or synthetic dirty flags.
namespace Libs::Graphics {
namespace {
TextureCache::ImageDesc GpuReadAliasLinearImage(uint64_t address) {
  TextureCache::ImageDesc desc{};
  desc.type=TextureCache::BindingType::Texture;
  desc.info.data={address,4u};
  desc.info.pixel_format=vk::Format::eR32Uint;
  desc.info.guest_format=Prospero::BufferFormat::k32UInt;
  desc.info.type=Prospero::ImageType::kColor2D;
  desc.info.extent={1u,1u,1u};desc.info.resources={1u,1u};
  desc.info.pitch=1u;desc.info.bytes_per_block=4u;desc.info.samples=1u;
  desc.info.tile_mode=Prospero::TileMode::kLinear;
  desc.info.mip_layout[0]={0u,4u,1u,1u};
  desc.view_info.format=desc.info.pixel_format;
  desc.view_info.type=vk::ImageViewType::e2D;
  desc.view_info.aspect=vk::ImageAspectFlagBits::eColor;
  desc.view_info.usage=vk::ImageUsageFlagBits::eSampled;
  desc.view_info.level_count=desc.view_info.layer_count=1u;
  return desc;
}
void CheckGpuReadMemoryImageAliasGuards(VulkanHarness& vulkan) {
  constexpr const char* name="GpuReadMemoryImageAliasGuards";
  constexpr uint32_t old=0x5a5a5a5au,a_value=0x12345678u;
  constexpr uint32_t image_value=0x2468ace0u,new_image_value=0xabcdef09u,far_value=0x76543210u;
  constexpr uint64_t a=ExactGpuReadGuest::Base+0x5000u;
  constexpr uint64_t image_address=a+0x800u,far_address=ExactGpuReadGuest::Base+0x20000u;
  constexpr uint32_t far_bytes=64u*1024u;
  constexpr std::array labels{"dirty-native-image-same-page","pending-only-image-same-page"};
  struct Result {
    bool setup=false,owner=false,native_initial=false,cpu_old=false,unarmed_initial=false;
    bool image_registered=false,dirty_before=false,pending_before=false,image_guard=false;
    bool prefence=false,gate=false,requested=false,far_whole=false,image_order=false;
    bool native_final=false,guards=false,clean=false,image_cleanup=false;
    uint64_t ring_before=0,ring_after=0,tick_before=0,tick_after=0;
    EagerNativeBudget budget;
  };
  std::array<Result,2> results{};
  {
    ExactGpuReadGuest guest;
    auto& graphics=vulkan.RuntimeContext();auto& renderer=vulkan.RuntimeRenderer();
    renderer.InitializeGpu(nullptr);LibKernel::Memory::InstallGpuResources(&renderer);
    renderer.MapMemory(ExactGpuReadGuest::Base,ExactGpuReadGuest::Bytes);
    auto& gpu=renderer.GetGpu();auto& cache=renderer.GetBufferCache();
    auto& textures=renderer.GetTextureCache();auto& scheduler=renderer.GetCommandScheduler();
    FetchGpuBuffer source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer proof(graphics,far_bytes+64u,vk::BufferUsageFlagBits::eTransferDst);
    FetchGpuBuffer image_proof(graphics,4u,vk::BufferUsageFlagBits::eTransferDst);
    source.Flush();proof.Flush();image_proof.Flush();
    EopComputeProducer compute(vulkan.Device(),source.buffer,a_value);
    std::unique_ptr<CommandProcessor> cp;Buffer* owner=nullptr;
    gpu.SendCommandSync([&]{
      cp=std::make_unique<CommandProcessor>(renderer,0x31);cp->Reset();cp->BufferInit();
      owner=cache.ObtainBuffer(ExactGpuReadGuest::Base,ExactGpuReadGuest::Bytes,true,true).first;
    });
    for(uint32_t test=0;test<results.size();++test) {
      auto& r=results[test];const bool pending=test==1u;
      ImageId image{};ArmedWindowGate gate;
      std::unique_ptr<std::jthread> releaser;
      const std::array<uint64_t,6> canaries{a-4u,a+4u,image_address-4u,image_address+4u,
                                          far_address-4u,far_address+far_bytes};
      gpu.SendCommandSync([&] {
        (void)cache.ObtainBuffer(ExactGpuReadGuest::Base,ExactGpuReadGuest::Bytes,true,true);
        owner->Fill(0u,ExactGpuReadGuest::Bytes,old);cache.OnCommandRecorded();
        scheduler.Finish();scheduler.DrainPriorityOperations();
        cache.ReadMemory(ExactGpuReadGuest::Base,ExactGpuReadGuest::Bytes);
        scheduler.Finish();scheduler.DrainPriorityOperations();PerformanceMemoryTestAccess::ResetEagerHotPolicy(cache);
        r.setup=GuestGpu::IsGpuThread()&&!CommandRecordingDeferred()&&!DrainStats::Enabled()&&
          !ReadbackAgeEnabled()&&!PerformanceMemoryTestAccess::HasQueue(cache)&&graphics.readback_queue==nullptr;
        ReadbackBootstrapWrite(scheduler,cache,compute,source,a);
        auto [far_owner,far_offset]=cache.ObtainBuffer(far_address,far_bytes,true,true);
        far_owner->Fill(far_offset,far_bytes,far_value);cache.OnCommandRecorded();
        r.owner=far_owner==owner&&owner->CpuAddress()==ExactGpuReadGuest::Base&&owner->Size()==ExactGpuReadGuest::Bytes;
        auto desc=GpuReadAliasLinearImage(image_address);image=textures.FindImage(desc);
        const auto view=textures.FindTexture(image,desc);
        r.image_registered=view!=nullptr&&textures.PeekImage(image)!=nullptr&&textures.PeekImage(image)->registered&&
          textures.GetImage(image).backing.format==vk::Format::eR32Uint;
        // Actual finite native clear; independent image readback proves real producer output.
        scheduler.EndRendering();auto command=scheduler.Current().Handle();auto& native=textures.GetImage(image);
        native.Transit(vk::ImageLayout::eTransferDstOptimal,vk::AccessFlagBits2::eTransferWrite,{},command);
        vk::ClearColorValue clear{};std::fill_n(clear.uint32.data(),4u,image_value);
        const vk::ImageSubresourceRange image_range{vk::ImageAspectFlagBits::eColor,0u,1u,0u,1u};
        command.clearColorImage(native.backing.image,vk::ImageLayout::eTransferDstOptimal,&clear,1u,&image_range);
        textures.MarkGpuWritten(image);
        vk::BufferImageCopy image_copy{};image_copy.imageSubresource={vk::ImageAspectFlagBits::eColor,0u,0u,1u};
        image_copy.imageExtent={1u,1u,1u};
        native.Download(std::span<const vk::BufferImageCopy>(&image_copy,1u),image_proof.buffer,0u,4u,4u);
        BcAliasHostReadBarrier(scheduler,image_proof.buffer,4u);
        CopyEopNativeLabel(scheduler,*owner,owner->Offset(a),proof,0u,4u);
        CopyEopNativeLabel(scheduler,*owner,owner->Offset(far_address),proof,4u,far_bytes);
        for(uint32_t i=0;i<canaries.size();++i)
          CopyEopNativeLabel(scheduler,*owner,owner->Offset(canaries[i]),proof,far_bytes+4u+i*4u,4u);
        scheduler.Finish();scheduler.DrainPriorityOperations();proof.Invalidate();image_proof.Invalidate();
        const auto* words=static_cast<const uint32_t*>(proof.data);
        r.native_initial=words[0]==a_value&&*static_cast<const uint32_t*>(image_proof.data)==image_value&&
          std::all_of(words+1u,words+1u+far_bytes/4u,[](uint32_t v){return v==far_value;});
        for(uint32_t i=0;i<canaries.size();++i)r.native_initial=r.native_initial&&words[1u+far_bytes/4u+i]==old;
        uint32_t av=0,bv=0,iv=0;
        r.cpu_old=LibKernel::Memory::TryReadBacking(a,&av,4u)&&av==old&&
          LibKernel::Memory::TryReadBacking(far_address,&bv,4u)&&bv==old&&
          LibKernel::Memory::TryReadBacking(image_address,&iv,4u)&&iv==old;
        const auto as=PerformanceMemoryTestAccess::Readback(cache,a,4u),bs=PerformanceMemoryTestAccess::Readback(cache,far_address,far_bytes);
        r.unarmed_initial=as.gpu_dirty&&as.unarmed&&bs.gpu_dirty&&bs.unarmed;
        scheduler.DeferPriorityOperation([&]{gate.Hold();});
        if(pending) {
          // Same actual DownloadImageMemory helper as production: owns a finite staging copy,
          // pending range and priority publication. No fabricated pending entries or flags.
          Require(name,"real image download bootstrap",PerformanceMemoryTestAccess::DownloadImageActual(textures,image),
            "actual image download was not recorded; setup failure");
          // A genuine later native buffer write relinquishes current image ownership while
          // the older image publication is still pending. This isolates the pending guard.
          textures.InvalidateMemoryFromGPU(image_address,4u,TextureCache::GpuWriteSource::Fill);
          auto [image_owner,image_offset]=cache.ObtainBuffer(image_address,4u,true,false);
          r.owner=r.owner&&image_owner==owner;image_owner->Fill(image_offset,4u,new_image_value);cache.OnCommandRecorded();
        }
        r.dirty_before=textures.IsRegionGpuModified(a,TRACKER_PAGE_SIZE);
        r.pending_before=textures.HasPendingDownload(a,TRACKER_PAGE_SIZE);
        r.image_guard=pending?(!r.dirty_before&&r.pending_before):(r.dirty_before&&!r.pending_before);
        r.tick_before=scheduler.CurrentTick();
      });
      releaser=std::make_unique<std::jthread>([&]{gate.ReleaseAfterReadStarts();});
      gpu.SendCommandSync([&] {
        ExactGpuReadWitness witness{&cache,&scheduler,a,far_address,4u,scheduler.CurrentTick(),old};
        const auto previous=DrainStats::g_wait_timer_test_observer;
        DrainStats::g_wait_timer_test_observer=ExactGpuReadWitness::Observe;ExactGpuReadWitness::active=&witness;
        r.budget.sources={static_cast<VkBuffer>(owner->Handle()),static_cast<VkBuffer>(owner->Handle())};
        r.budget.download=static_cast<VkBuffer>(cache.GetUtilityBuffer(MemoryUsage::Download).Handle());
        r.ring_before=StreamBufferTestAccess::Offset(cache.GetUtilityBuffer(MemoryUsage::Download));gate.StartRead();
        {EagerNativeBudget::Scope observe(r.budget);cache.ReadMemory(a,4u);}
        r.ring_after=StreamBufferTestAccess::Offset(cache.GetUtilityBuffer(MemoryUsage::Download));r.tick_after=scheduler.CurrentTick();
        ExactGpuReadWitness::active=nullptr;DrainStats::g_wait_timer_test_observer=previous;
        r.prefence=witness.seen&&witness.armed_old;
        uint32_t av=0,iv=0;std::vector<uint32_t> far_words(far_bytes/4u);
        r.requested=LibKernel::Memory::TryReadBacking(a,&av,4u)&&av==a_value;
        r.far_whole=LibKernel::Memory::TryReadBacking(far_address,far_words.data(),far_bytes)&&
          std::all_of(far_words.begin(),far_words.end(),[](uint32_t v){return v==far_value;});
        r.image_order=LibKernel::Memory::TryReadBacking(image_address,&iv,4u)&&
          iv==(pending?new_image_value:old)&&!textures.HasPendingDownload(a,TRACKER_PAGE_SIZE);
        CopyEopNativeLabel(scheduler,*owner,owner->Offset(a),proof,0u,4u);
        CopyEopNativeLabel(scheduler,*owner,owner->Offset(far_address),proof,4u,far_bytes);
        CopyEopNativeLabel(scheduler,*owner,owner->Offset(image_address),proof,far_bytes+32u,4u);
        for(uint32_t i=0;i<canaries.size();++i)
          CopyEopNativeLabel(scheduler,*owner,owner->Offset(canaries[i]),proof,far_bytes+4u+i*4u,4u);
        scheduler.Finish();scheduler.DrainPriorityOperations();proof.Invalidate();
        const auto* words=static_cast<const uint32_t*>(proof.data);
        r.native_final=words[0]==a_value&&words[far_bytes/4u+8u]==(pending?new_image_value:old)&&
          std::all_of(words+1u,words+1u+far_bytes/4u,[](uint32_t v){return v==far_value;});
        r.guards=true;for(uint32_t i=0;i<canaries.size();++i){uint32_t v=0;
          r.guards=r.guards&&words[1u+far_bytes/4u+i]==old&&LibKernel::Memory::TryReadBacking(canaries[i],&v,4u)&&v==old;}
        r.clean=!PerformanceMemoryTestAccess::Readback(cache,a,4u).gpu_dirty&&
          !PerformanceMemoryTestAccess::Readback(cache,far_address,far_bytes).gpu_dirty;
        if(!pending) {
          Require(name,"dirty image cleanup records real publication",PerformanceMemoryTestAccess::DownloadImageActual(textures,image),
            "actual image cleanup download failed");
          scheduler.Finish();scheduler.DrainPriorityOperations();
        }
        r.image_cleanup=LibKernel::Memory::TryReadBacking(image_address,&iv,4u)&&
          iv==(pending?new_image_value:image_value)&&!textures.HasPendingDownload(a,TRACKER_PAGE_SIZE);
        TextureCacheTestAccess::FreeImage(textures,image);scheduler.Finish();scheduler.DrainPriorityOperations();
        r.image_cleanup=r.image_cleanup&&!textures.IsRegionRegistered(image_address,4u);
      });
      releaser->join();{std::lock_guard lock(gate.mutex);r.gate=gate.callback_entered&&!gate.timeout;}
    }
    renderer.UnmapMemory(ExactGpuReadGuest::Base,ExactGpuReadGuest::Bytes);renderer.ShutdownGpu();
    LibKernel::Memory::InstallGpuResources(nullptr);cp.reset();
  }
  for(uint32_t test=0;test<results.size();++test){const auto& r=results[test];
    std::printf("[gpu-read-image-guard] %s setup=%u owner=%u nativeBefore=%u CPUold=%u unarmed=%u image=%u dirty=%u pending=%u guard=%u prefence=%u gate=%u A=%u farWhole=%u imageOrder=%u nativeFinal=%u guards=%u clean=%u imageCleanup=%u copied=%llu reserve=%llu->%llu tick=%llu->%llu\n",
      labels[test],r.setup,r.owner,r.native_initial,r.cpu_old,r.unarmed_initial,r.image_registered,r.dirty_before,r.pending_before,r.image_guard,r.prefence,r.gate,r.requested,r.far_whole,r.image_order,r.native_final,r.guards,r.clean,r.image_cleanup,
      static_cast<unsigned long long>(r.budget.copied_bytes),static_cast<unsigned long long>(r.ring_before),static_cast<unsigned long long>(r.ring_after),
      static_cast<unsigned long long>(r.tick_before),static_cast<unsigned long long>(r.tick_after));
    Require(labels[test],"all actual native values/old protected backing/real image ownership/priority/far_words data/guards/cleanup",
      r.setup&&r.owner&&r.native_initial&&r.cpu_old&&r.unarmed_initial&&r.image_registered&&r.image_guard&&r.prefence&&r.gate&&
      r.requested&&r.far_whole&&r.image_order&&r.native_final&&r.guards&&r.clean&&r.image_cleanup,
      "real image guard control failed");
  }
  for(uint32_t test=0;test<results.size();++test){const auto& r=results[test];
    // The earlier real image download leaves a four-byte ring offset. The
    // requested buffer download has a64-byte alignment contract, including
    // leading padding; this is not additional copied data.
    const uint64_t leading_padding=(64u-(r.ring_before%64u))%64u;
    Require(labels[test],"actual dirty or pending image on touched page preserves original wide buffer read",
      r.budget.copied_bytes==far_bytes+4u+(test==1u?4u:0u)&&r.budget.copy_calls==1u&&
      r.ring_after-r.ring_before==leading_padding+far_bytes+64u+(test==1u?64u:0u)&&r.tick_after==r.tick_before+1u,
      "image alias guard narrowed the existing wide readback or changed actual completion");
  }
}
} // namespace
} // namespace Libs::Graphics
