// Include AFTER GpuReadMemoryExactPageCostTests.inc. Actual foreign CPU protected read fault locality.
namespace Libs::Graphics {
namespace {
void CheckGpuReadMemoryForeignFaultLocality(VulkanHarness& vulkan) {
  constexpr const char* name="GpuReadMemoryForeignFaultLocality";
  constexpr uint64_t a=ExactGpuReadGuest::Base+0x5000u,far_address=ExactGpuReadGuest::Base+0x20000u;
  constexpr uint32_t old=0x5a5a5a5au,value=0x12345678u,far_value=0x76543210u,far_bytes=65536u;
  bool setup=false,initial_native=false,protected_old=false,foreign=false,requested=false,far_published=false,guards=false,clean=false;
  uint32_t read_value=0;uint64_t before=0,after=0;EagerNativeBudget budget;
  Require(name,"actual fault route",Common::HostException::InstallHandler([](const Common::HostException::ExceptionInfo& i){
    if(i.type!=Common::HostException::ExceptionType::AccessViolation||
       (i.access_violation_type!=Common::HostException::AccessViolationType::Read&&i.access_violation_type!=Common::HostException::AccessViolationType::Write))return false;
    return LibKernel::Memory::HandleGpuFault(i.access_violation_type==Common::HostException::AccessViolationType::Write?PageFaultAccess::Write:PageFaultAccess::Read,i.access_violation_vaddr);
  }),"fault setup failed");
  {
    ExactGpuReadGuest guest;auto& graphics=vulkan.RuntimeContext();auto& renderer=vulkan.RuntimeRenderer();
    renderer.InitializeGpu(nullptr);LibKernel::Memory::InstallGpuResources(&renderer);renderer.MapMemory(ExactGpuReadGuest::Base,ExactGpuReadGuest::Bytes);
    auto& gpu=renderer.GetGpu();auto& cache=renderer.GetBufferCache();auto& scheduler=renderer.GetCommandScheduler();
    std::unique_ptr<CommandProcessor> cp;Buffer* owner=nullptr;
    FetchGpuBuffer source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer proof(graphics,far_bytes+32u,vk::BufferUsageFlagBits::eTransferDst);source.Flush();proof.Flush();
    EopComputeProducer producer(vulkan.Device(),source.buffer,value);
    constexpr std::array<uint64_t,4> guard_addresses{a-4u,a+4u,far_address-4u,far_address+far_bytes};
    gpu.SendCommandSync([&]{cp=std::make_unique<CommandProcessor>(renderer,0x31);cp->Reset();cp->BufferInit();
      owner=cache.ObtainBuffer(ExactGpuReadGuest::Base,ExactGpuReadGuest::Bytes,true,true).first;
      owner->Fill(0u,ExactGpuReadGuest::Bytes,old);cache.OnCommandRecorded();scheduler.Finish();scheduler.DrainPriorityOperations();
      cache.ReadMemory(ExactGpuReadGuest::Base,ExactGpuReadGuest::Bytes);scheduler.Finish();scheduler.DrainPriorityOperations();PerformanceMemoryTestAccess::ResetEagerHotPolicy(cache);
      ReadbackBootstrapWrite(scheduler,cache,producer,source,a);auto [far_owner,offset]=cache.ObtainBuffer(far_address,far_bytes,true,true);
      far_owner->Fill(offset,far_bytes,far_value);cache.OnCommandRecorded();
      setup=GuestGpu::IsGpuThread()&&!CommandRecordingDeferred()&&!DrainStats::Enabled()&&!ReadbackAgeEnabled()&&
        !PerformanceMemoryTestAccess::HasQueue(cache)&&graphics.readback_queue==nullptr&&far_owner==owner&&owner->CpuAddress()==ExactGpuReadGuest::Base&&owner->Size()==ExactGpuReadGuest::Bytes;
      CopyEopNativeLabel(scheduler,*owner,owner->Offset(a),proof,0u,4u);
      CopyEopNativeLabel(scheduler,*owner,owner->Offset(far_address),proof,8u,far_bytes);
      for(uint32_t i=0;i<guard_addresses.size();++i)CopyEopNativeLabel(scheduler,*owner,owner->Offset(guard_addresses[i]),proof,8u+far_bytes+i*4u,4u);
      const auto tick=scheduler.CurrentTick();scheduler.Flush();scheduler.GetMasterSemaphore().Wait(tick);scheduler.WaitPriorityOperations(tick);proof.Invalidate();
      const auto* words=static_cast<const uint32_t*>(proof.data);initial_native=words[0]==value&&
        std::all_of(words+2u,words+2u+far_bytes/4u,[](uint32_t v){return v==far_value;});
      for(uint32_t i=0;i<guard_addresses.size();++i)initial_native=initial_native&&words[2u+far_bytes/4u+i]==old;
      uint32_t av=0,bv=0;const auto as=PerformanceMemoryTestAccess::Readback(cache,a,4u),bs=PerformanceMemoryTestAccess::Readback(cache,far_address,far_bytes);
      protected_old=as.gpu_dirty&&as.unarmed&&bs.gpu_dirty&&bs.unarmed&&LibKernel::Memory::TryReadBacking(a,&av,4u)&&av==old&&LibKernel::Memory::TryReadBacking(far_address,&bv,4u)&&bv==old;
      budget.sources={static_cast<VkBuffer>(owner->Handle()),static_cast<VkBuffer>(owner->Handle())};
      budget.download=static_cast<VkBuffer>(cache.GetUtilityBuffer(MemoryUsage::Download).Handle());before=StreamBufferTestAccess::Offset(cache.GetUtilityBuffer(MemoryUsage::Download));
    });
    {
      EagerNativeBudget::Scope observe(budget);
      // Install only an observation pointer on the GPU callback thread; no readiness/state changed.
      gpu.SendCommandSync([&]{EagerNativeBudget::active=&budget;});
      std::jthread reader([&]{foreign=!GuestGpu::IsGpuThread();const volatile uint32_t* p=reinterpret_cast<const volatile uint32_t*>(a);read_value=*p;});reader.join();
      gpu.SendCommandSync([&]{EagerNativeBudget::active=nullptr;after=StreamBufferTestAccess::Offset(cache.GetUtilityBuffer(MemoryUsage::Download));});
    }
    gpu.SendCommandSync([&]{
      requested=read_value==value;std::vector<uint32_t> actual_far(far_bytes/4u);
      far_published=LibKernel::Memory::TryReadBacking(far_address,actual_far.data(),far_bytes)&&
        std::all_of(actual_far.begin(),actual_far.end(),[](uint32_t v){return v==far_value;});
      guards=true;for(const auto address:guard_addresses){uint32_t v=0;guards=guards&&LibKernel::Memory::TryReadBacking(address,&v,4u)&&v==old;}
      clean=!PerformanceMemoryTestAccess::Readback(cache,a,4u).gpu_dirty&&!PerformanceMemoryTestAccess::Readback(cache,far_address,far_bytes).gpu_dirty;
      cache.ReadMemory(a,4u);cache.ReadMemory(far_address,far_bytes);scheduler.Finish();scheduler.DrainPriorityOperations();
    });
    renderer.UnmapMemory(ExactGpuReadGuest::Base,ExactGpuReadGuest::Bytes);renderer.ShutdownGpu();LibKernel::Memory::InstallGpuResources(nullptr);cp.reset();
  }
  std::printf("[foreign-fault-locality] setup=%u initialNativeAllFar=%u protectedOld=%u foreign=%u requested=%u farPublished=%u guards=%u clean=%u bytes=%llu ring=%llu->%llu\n",
    setup,initial_native,protected_old,foreign,requested,far_published,guards,clean,static_cast<unsigned long long>(budget.copied_bytes),
    static_cast<unsigned long long>(before),static_cast<unsigned long long>(after));
  Require(name,"actual foreign protected pointer read/native values/far_address publication/canaries/cleanup BEFORE locality budget",
    setup&&initial_native&&protected_old&&foreign&&requested&&far_published&&guards&&clean,"actual foreign-fault values/protection/lifetime failed");
  Require(name,"foreign CPU fault retains normal broad locality and exact actual data",
    budget.copy_calls==1u&&budget.copied_bytes==far_bytes+4u&&after-before==far_bytes+64u&&budget.source_barriers==1u&&budget.host_barriers==1u,
    "explicit GPU-reader footprint leaked into genuine foreign CPU fault");
}
} // namespace
} // namespace Libs::Graphics
