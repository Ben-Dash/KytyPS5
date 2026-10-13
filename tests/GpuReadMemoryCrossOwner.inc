// Include AFTER GpuReadMemoryExactPageCostTests.inc. Existing-API incomplete owner/discovery neighbor.
namespace Libs::Graphics {
namespace {
void CheckGpuReadMemoryCrossOwner(VulkanHarness& vulkan) {
  constexpr const char* name="GpuReadMemoryCrossOwner";
  constexpr uint64_t base=ExactGpuReadGuest::Base,half=ExactGpuReadGuest::Bytes/2u,a=base+half-4u;
  constexpr uint64_t high_address=a+4u,near_address=high_address+64u,far_address=base+0x2e000u;
  constexpr uint32_t old=0x5a5a5a5au,low=0x12345678u,high=0x89abcdefu,near_value=0x0badf00du,far_value=0x76543210u,far_bytes=65536u;
  bool setup=false,initial_native=false,old_before=false,both_dirty=false,merged=false,requested=false;
  bool whole_siblings=false,final_native=false,backing=false,guards=false,clean=false;
  Require(name,"actual fault route",Common::HostException::InstallHandler([](const Common::HostException::ExceptionInfo& i){
    if(i.type!=Common::HostException::ExceptionType::AccessViolation||
       (i.access_violation_type!=Common::HostException::AccessViolationType::Read&&i.access_violation_type!=Common::HostException::AccessViolationType::Write))return false;
    return LibKernel::Memory::HandleGpuFault(i.access_violation_type==Common::HostException::AccessViolationType::Write?PageFaultAccess::Write:PageFaultAccess::Read,i.access_violation_vaddr);
  }),"fault bootstrap failed");
  {
    ExactGpuReadGuest guest;auto& graphics=vulkan.RuntimeContext();auto& renderer=vulkan.RuntimeRenderer();
    renderer.InitializeGpu(nullptr);LibKernel::Memory::InstallGpuResources(&renderer);renderer.MapMemory(base,ExactGpuReadGuest::Bytes);
    auto& gpu=renderer.GetGpu();auto& cache=renderer.GetBufferCache();auto& scheduler=renderer.GetCommandScheduler();
    std::unique_ptr<CommandProcessor> cp;Buffer* first=nullptr;Buffer* second=nullptr;
    FetchGpuBuffer low_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer high_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer near_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer proof(graphics,far_bytes+64u,vk::BufferUsageFlagBits::eTransferDst);
    low_source.Flush();high_source.Flush();near_source.Flush();proof.Flush();
    EopComputeProducer lp(vulkan.Device(),low_source.buffer,low),hp(vulkan.Device(),high_source.buffer,high),np(vulkan.Device(),near_source.buffer,near_value);
    constexpr std::array<uint64_t,6> guard_addresses{a-4u,a+8u,near_address-4u,near_address+4u,far_address-4u,far_address+far_bytes};
    gpu.SendCommandSync([&]{
      cp=std::make_unique<CommandProcessor>(renderer,0x31);cp->Reset();cp->BufferInit();
      first=cache.ObtainBuffer(base,half,true,true).first;second=cache.ObtainBuffer(base+half,half,true,true).first;
      first->Fill(0u,half,old);second->Fill(0u,half,old);cache.OnCommandRecorded();scheduler.Finish();scheduler.DrainPriorityOperations();
      cache.ReadMemory(base,half);cache.ReadMemory(base+half,half);scheduler.Finish();scheduler.DrainPriorityOperations();PerformanceMemoryTestAccess::ResetEagerHotPolicy(cache);
      setup=GuestGpu::IsGpuThread()&&!CommandRecordingDeferred()&&!DrainStats::Enabled()&&!ReadbackAgeEnabled()&&
        !PerformanceMemoryTestAccess::HasQueue(cache)&&graphics.readback_queue==nullptr&&first!=second&&
        first->CpuAddress()==base&&second->CpuAddress()==base+half&&first->Size()==half&&second->Size()==half&&
        !first->IsInBounds(a,8u)&&!second->IsInBounds(a,8u);
      ReadbackBootstrapWrite(scheduler,cache,lp,low_source,a);ReadbackBootstrapWrite(scheduler,cache,hp,high_source,high_address);
      ReadbackBootstrapWrite(scheduler,cache,np,near_source,near_address);
      auto [far_owner,offset]=cache.ObtainBuffer(far_address,far_bytes,true,true);far_owner->Fill(offset,far_bytes,far_value);cache.OnCommandRecorded();
      CopyEopNativeLabel(scheduler,*first,first->Offset(a),proof,0u,4u);CopyEopNativeLabel(scheduler,*second,second->Offset(high_address),proof,4u,4u);
      CopyEopNativeLabel(scheduler,*second,second->Offset(near_address),proof,8u,4u);CopyEopNativeLabel(scheduler,*second,second->Offset(far_address),proof,16u,far_bytes);
      for(uint32_t i=0;i<guard_addresses.size();++i){auto* owner=guard_addresses[i]<base+half?first:second;
        CopyEopNativeLabel(scheduler,*owner,owner->Offset(guard_addresses[i]),proof,16u+far_bytes+i*4u,4u);}
      const auto tick=scheduler.CurrentTick();scheduler.Flush();scheduler.GetMasterSemaphore().Wait(tick);scheduler.WaitPriorityOperations(tick);proof.Invalidate();
      const auto* words=static_cast<const uint32_t*>(proof.data);initial_native=words[0]==low&&words[1]==high&&words[2]==near_value&&
        std::all_of(words+4u,words+4u+far_bytes/4u,[](uint32_t v){return v==far_value;});
      for(uint32_t i=0;i<guard_addresses.size();++i)initial_native=initial_native&&words[4u+far_bytes/4u+i]==old;
      uint32_t av=0,hv=0,fv=0;old_before=LibKernel::Memory::TryReadBacking(a,&av,4u)&&av==old&&LibKernel::Memory::TryReadBacking(high_address,&hv,4u)&&hv==old&&
        LibKernel::Memory::TryReadBacking(far_address,&fv,4u)&&fv==old;
      const auto as=PerformanceMemoryTestAccess::Readback(cache,a,8u),fs=PerformanceMemoryTestAccess::Readback(cache,far_address,far_bytes);
      both_dirty=as.gpu_dirty&&as.unarmed&&fs.gpu_dirty&&fs.unarmed;
      const auto old_first=first->Handle(),old_second=second->Handle();
      cache.ReadMemory(a,8u); // Must retain discovery/merge before any narrow-owner transfer.
      const auto [new_handle,new_offset]=cache.PeekPage(a);const auto [high_handle,high_offset]=cache.PeekPage(high_address);
      (void)new_offset;(void)high_offset;merged=new_handle!=nullptr&&new_handle==high_handle&&new_handle!=old_first&&new_handle!=old_second;
      requested=LibKernel::Memory::TryReadBacking(a,&av,4u)&&av==low&&LibKernel::Memory::TryReadBacking(high_address,&hv,4u)&&hv==high;
      uint32_t nv=0;std::vector<uint32_t> far_words(far_bytes/4u);
      whole_siblings=LibKernel::Memory::TryReadBacking(near_address,&nv,4u)&&nv==near_value&&LibKernel::Memory::TryReadBacking(far_address,far_words.data(),far_bytes)&&
        std::all_of(far_words.begin(),far_words.end(),[](uint32_t v){return v==far_value;});
      auto [owner,offset2]=cache.ObtainBuffer(a,8u,false,true);(void)offset2;
      // Reacquire the actual merged owner; never reuse deleted first/second pointers after discovery.
      CopyEopNativeLabel(scheduler,*owner,owner->Offset(a),proof,0u,8u);CopyEopNativeLabel(scheduler,*owner,owner->Offset(near_address),proof,8u,4u);
      CopyEopNativeLabel(scheduler,*owner,owner->Offset(far_address),proof,16u,far_bytes);
      for(uint32_t i=0;i<guard_addresses.size();++i)CopyEopNativeLabel(scheduler,*owner,owner->Offset(guard_addresses[i]),proof,16u+far_bytes+i*4u,4u);
      scheduler.Finish();scheduler.DrainPriorityOperations();proof.Invalidate();words=static_cast<const uint32_t*>(proof.data);
      final_native=words[0]==low&&words[1]==high&&words[2]==near_value&&std::all_of(words+4u,words+4u+far_bytes/4u,[](uint32_t v){return v==far_value;});
      guards=true;for(uint32_t i=0;i<guard_addresses.size();++i){uint32_t v=0;guards=guards&&words[4u+far_bytes/4u+i]==old&&LibKernel::Memory::TryReadBacking(guard_addresses[i],&v,4u)&&v==old;}
      backing=requested&&whole_siblings;clean=!PerformanceMemoryTestAccess::Readback(cache,a,8u).gpu_dirty&&!PerformanceMemoryTestAccess::Readback(cache,far_address,far_bytes).gpu_dirty;
      cache.ReadMemory(a,8u);cache.ReadMemory(far_address,far_bytes);scheduler.Finish();scheduler.DrainPriorityOperations();
    });
    renderer.UnmapMemory(base,ExactGpuReadGuest::Bytes);renderer.ShutdownGpu();LibKernel::Memory::InstallGpuResources(nullptr);cp.reset();
  }
  std::printf("[GPU-read-cross-owner] setup=%u nativeInitial=%u oldCPU=%u bothDirty=%u actualNewMergedOwner=%u requestedLowHigh=%u wideSiblingFar=%u nativeFinal=%u backing=%u guards=%u clean=%u cleanup=PASS\n",
    setup,initial_native,old_before,both_dirty,merged,requested,whole_siblings,final_native,backing,guards,clean);
  Require(name,"actual two-owner request discovers/joins and preserves every requested/native/backing/canary generation",
    setup&&initial_native&&old_before&&both_dirty&&merged&&requested&&whole_siblings&&final_native&&backing&&guards&&clean,
    "first owner was treated as whole-request coverage, or discovery/migration dropped data/publication");
}
} // namespace
} // namespace Libs::Graphics
