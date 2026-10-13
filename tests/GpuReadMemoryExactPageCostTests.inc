// Include after EagerReadbackAdjacentPagesCostTests.inc and actual readback/gate helpers.
namespace Libs::Graphics {
namespace {
void ConfigureGpuReadMemoryExactPageTests() { ConfigureEagerReadbackAdjacentPagesCostTests(); }
struct ExactGpuReadGuest {
  static constexpr uint64_t Base=0x0000000200b00000ull,Bytes=0x40000u;
  int64_t direct=-1;
  ExactGpuReadGuest() {
    Require("GpuReadMemoryExactPage","real guest allocation",
      LibKernel::Memory::KernelAllocateDirectMemory(0,LibKernel::Memory::KernelGetDirectMemorySize(),Bytes,Bytes,0,&direct)==0,"allocation bootstrap failed");
    void* address=reinterpret_cast<void*>(Base);
    Require("GpuReadMemoryExactPage","real guest mapping",
      LibKernel::Memory::KernelMapDirectMemory(&address,Bytes,0x3,0x10,direct,Bytes)==0&&address==reinterpret_cast<void*>(Base),"mapping bootstrap failed");
    std::memset(address,0,Bytes);
  }
  ~ExactGpuReadGuest() {
    Require("GpuReadMemoryExactPage","actual task-owned mapping cleanup",
      LibKernel::Memory::KernelMunmap(Base,Bytes)==0&&LibKernel::Memory::KernelReleaseDirectMemory(direct,Bytes)==0,"mapping cleanup failed");
  }
};
struct ExactGpuReadWitness {
  BufferCache* cache=nullptr;CommandScheduler* scheduler=nullptr;uint64_t a=0,b=0,size=0,tick=0;uint32_t old=0;
  bool seen=false,armed_old=false;
  static inline thread_local ExactGpuReadWitness* active=nullptr;
  static void Observe(DrainStats::Kind kind) {
    if(active==nullptr||kind!=DrainStats::Kind::FullDrain||active->seen)return;
    auto& w=*active;w.seen=true;const auto s=PerformanceMemoryTestAccess::Readback(*w.cache,w.a,w.size);
    uint32_t av=0,bv=0;
    w.armed_old=s.gpu_dirty&&!s.unarmed&&s.tick==w.tick&&w.scheduler->CurrentTick()==w.tick&&
      LibKernel::Memory::TryReadBacking(w.a,&av,4u)&&av==w.old&&LibKernel::Memory::TryReadBacking(w.b,&bv,4u)&&bv==w.old;
  }
};
void CheckGpuReadMemoryExactPageCost(VulkanHarness& vulkan) {
  constexpr const char* name="GpuReadMemoryExactPageCost";
  constexpr uint32_t old=0x5a5a5a5au,low=0x12345678u,high=0x89abcdefu,near_value=0x0badf00du;
  constexpr uint32_t far_value=0x76543210u,new_low=0xabcdef09u,far_bytes=64u*1024u;
  constexpr uint64_t far_address=ExactGpuReadGuest::Base+0x20000u;
  constexpr std::array labels{"small-explicit","same-page-sibling","crossing-two-pages","new-current-A-writer","actual-GPU-thread-read-fault","is-write-locality"};
  struct Result {
    bool setup=false,owner=false,initial_native=false,initial_old=false,initial_unarmed=false,prefence=false,gate=false;
    bool requested=false,near_correct=false,near_clean=false,real_wait=false,write_marker=true,fault_origin=true;
    bool far_unarmed=false,far_old=false,final_native=false,final_backing=false,guards=false,clean=false;
    uint64_t ring_before=0,ring_after=0,tick_before=0,tick_after=0,expected_copied=0,expected_reserved=0;
    EagerNativeBudget budget;
  };
  std::array<Result,6> results{};
  Require(name,"existing real fault handler",Common::HostException::InstallHandler([](const Common::HostException::ExceptionInfo& i) {
    if(i.type!=Common::HostException::ExceptionType::AccessViolation||
       (i.access_violation_type!=Common::HostException::AccessViolationType::Read&&i.access_violation_type!=Common::HostException::AccessViolationType::Write))return false;
    return LibKernel::Memory::HandleGpuFault(i.access_violation_type==Common::HostException::AccessViolationType::Write?PageFaultAccess::Write:PageFaultAccess::Read,i.access_violation_vaddr);
  }),"fault bootstrap failed; NOT RED");
  {
    ExactGpuReadGuest guest;auto& graphics=vulkan.RuntimeContext();auto& renderer=vulkan.RuntimeRenderer();
    renderer.InitializeGpu(nullptr);LibKernel::Memory::InstallGpuResources(&renderer);renderer.MapMemory(ExactGpuReadGuest::Base,ExactGpuReadGuest::Bytes);
    auto& gpu=renderer.GetGpu();auto& cache=renderer.GetBufferCache();auto& scheduler=renderer.GetCommandScheduler();
    FetchGpuBuffer low_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer high_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer near_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer new_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer proof(graphics,far_bytes+64u,vk::BufferUsageFlagBits::eTransferDst);
    low_source.Flush();high_source.Flush();near_source.Flush();new_source.Flush();proof.Flush();
    EopComputeProducer low_compute(vulkan.Device(),low_source.buffer,low),high_compute(vulkan.Device(),high_source.buffer,high);
    EopComputeProducer near_compute(vulkan.Device(),near_source.buffer,near_value),new_compute(vulkan.Device(),new_source.buffer,new_low);
    std::unique_ptr<CommandProcessor> cp;Buffer* owner=nullptr;
    gpu.SendCommandSync([&]{cp=std::make_unique<CommandProcessor>(renderer,0x31);cp->Reset();cp->BufferInit();
      owner=cache.ObtainBuffer(ExactGpuReadGuest::Base,ExactGpuReadGuest::Bytes,true,true).first;});
    for(uint32_t test=0;test<results.size();++test) {
      auto& r=results[test];const bool sibling=test==1u||test==2u||test==3u,cross=test==2u,wide=test>=4u;
      const uint64_t a=ExactGpuReadGuest::Base+(cross?0x5ffcu:0x5000u),size=cross?8u:4u;
      const uint64_t nearby=ExactGpuReadGuest::Base+(cross?0x6040u:0x5040u);
      const std::array<uint64_t,6> canaries{a-4u,a+size,nearby-4u,nearby+4u,far_address-4u,far_address+far_bytes};
      ArmedWindowGate gate;std::unique_ptr<std::jthread> releaser;
      gpu.SendCommandSync([&] {
        // Warm/reset whole real owner and publish it OUTSIDE the requested-read observer.
        (void)cache.ObtainBuffer(ExactGpuReadGuest::Base,ExactGpuReadGuest::Bytes,true,true);
        owner->Fill(0u,ExactGpuReadGuest::Bytes,old);cache.OnCommandRecorded();
        scheduler.Finish();scheduler.DrainPriorityOperations();cache.ReadMemory(ExactGpuReadGuest::Base,ExactGpuReadGuest::Bytes);
        scheduler.Finish();scheduler.DrainPriorityOperations();PerformanceMemoryTestAccess::ResetEagerHotPolicy(cache);
        r.setup=GuestGpu::IsGpuThread()&&!CommandRecordingDeferred()&&!DrainStats::Enabled()&&!ReadbackAgeEnabled()&&
          !PerformanceMemoryTestAccess::HasQueue(cache)&&graphics.readback_queue==nullptr;
        ReadbackBootstrapWrite(scheduler,cache,low_compute,low_source,a);
        if(cross)ReadbackBootstrapWrite(scheduler,cache,high_compute,high_source,a+4u);
        if(sibling)ReadbackBootstrapWrite(scheduler,cache,near_compute,near_source,nearby);
        auto [far_owner,far_offset]=cache.ObtainBuffer(far_address,far_bytes,true,true);
        far_owner->Fill(far_offset,far_bytes,far_value);cache.OnCommandRecorded();
        const auto actual_owner=cache.ObtainBuffer(a,size,false,true).first;
        r.owner=actual_owner==owner&&far_owner==owner&&owner->CpuAddress()==ExactGpuReadGuest::Base&&owner->Size()==ExactGpuReadGuest::Bytes;
        const auto snapshot=[&] {
          CopyEopNativeLabel(scheduler,*owner,owner->Offset(a),proof,0u,size);
          CopyEopNativeLabel(scheduler,*owner,owner->Offset(nearby),proof,8u,4u);
          CopyEopNativeLabel(scheduler,*owner,owner->Offset(far_address),proof,16u,far_bytes);
          for(uint32_t i=0;i<canaries.size();++i)CopyEopNativeLabel(scheduler,*owner,owner->Offset(canaries[i]),proof,16u+far_bytes+i*4u,4u);
        };
        snapshot();const auto producer=scheduler.CurrentTick();scheduler.Flush();scheduler.GetMasterSemaphore().Wait(producer);
        scheduler.WaitPriorityOperations(producer);proof.Invalidate();const auto* words=static_cast<const uint32_t*>(proof.data);
        r.initial_native=words[0]==low&&(!cross||words[1]==high)&&words[2]==(sibling?near_value:old)&&
          std::all_of(words+4u,words+4u+far_bytes/4u,[](uint32_t v){return v==far_value;});
        for(uint32_t i=0;i<canaries.size();++i)r.initial_native=r.initial_native&&words[4u+far_bytes/4u+i]==old;
        uint32_t av=0,bv=0;r.initial_old=LibKernel::Memory::TryReadBacking(a,&av,4u)&&av==old&&LibKernel::Memory::TryReadBacking(far_address,&bv,4u)&&bv==old;
        const auto as=PerformanceMemoryTestAccess::Readback(cache,a,size),bs=PerformanceMemoryTestAccess::Readback(cache,far_address,far_bytes);
        r.initial_unarmed=as.gpu_dirty&&as.unarmed&&bs.gpu_dirty&&bs.unarmed;
        // A genuine NEW current native writer must remain ahead of the readback, not old armed A.
        if(test==3u)ReadbackBootstrapWrite(scheduler,cache,new_compute,new_source,a);
        scheduler.DeferPriorityOperation([&]{gate.Hold();});r.tick_before=scheduler.CurrentTick();
      });
      releaser=std::make_unique<std::jthread>([&]{gate.ReleaseAfterReadStarts();});
      gpu.SendCommandSync([&] {
        ExactGpuReadWitness witness{&cache,&scheduler,a,far_address,size,scheduler.CurrentTick(),old};
        const auto previous=DrainStats::g_wait_timer_test_observer;
        DrainStats::g_wait_timer_test_observer=ExactGpuReadWitness::Observe;ExactGpuReadWitness::active=&witness;
        r.budget.sources={static_cast<VkBuffer>(owner->Handle()),static_cast<VkBuffer>(owner->Handle())};
        r.budget.download=static_cast<VkBuffer>(cache.GetUtilityBuffer(MemoryUsage::Download).Handle());
        r.ring_before=StreamBufferTestAccess::Offset(cache.GetUtilityBuffer(MemoryUsage::Download));gate.StartRead();
        {EagerNativeBudget::Scope observe(r.budget);
          if(test==4u) {
            // Genuine access violation through installed production handler; origin locality MUST remain wide.
            const volatile uint32_t* address=reinterpret_cast<const volatile uint32_t*>(a);
            const uint32_t raw=*address;r.fault_origin=raw==low;
          } else cache.ReadMemory(a,size,test==5u);
        }
        r.ring_after=StreamBufferTestAccess::Offset(cache.GetUtilityBuffer(MemoryUsage::Download));r.tick_after=scheduler.CurrentTick();
        ExactGpuReadWitness::active=nullptr;DrainStats::g_wait_timer_test_observer=previous;r.prefence=witness.seen&&witness.armed_old;
        scheduler.WaitPriorityOperations(scheduler.CurrentTick()-1u);
        r.real_wait=r.tick_after==r.tick_before+1u&&scheduler.GetMasterSemaphore().IsFree(r.tick_before);
        uint32_t av=0,hv=0,nv=0,bv=0;
        r.requested=LibKernel::Memory::TryReadBacking(a,&av,4u)&&av==(test==3u?new_low:low)&&
          (!cross||(LibKernel::Memory::TryReadBacking(a+4u,&hv,4u)&&hv==high));
        r.near_correct=LibKernel::Memory::TryReadBacking(nearby,&nv,4u)&&nv==(sibling?near_value:old);
        r.near_clean=!PerformanceMemoryTestAccess::Readback(cache,a,size).gpu_dirty&&
          (!sibling||!PerformanceMemoryTestAccess::Readback(cache,nearby,4u).gpu_dirty);
        if(test==5u)r.write_marker=cache.IsRegionCpuModified(a,size);
        const auto bs=PerformanceMemoryTestAccess::Readback(cache,far_address,far_bytes);
        r.far_unarmed=bs.gpu_dirty&&bs.unarmed;r.far_old=LibKernel::Memory::TryReadBacking(far_address,&bv,4u)&&bv==old;
        // Separate real far_address-region demand/cleanup is required in either implementation.
        cache.ReadMemory(far_address,far_bytes);
        CopyEopNativeLabel(scheduler,*owner,owner->Offset(a),proof,0u,size);
        CopyEopNativeLabel(scheduler,*owner,owner->Offset(nearby),proof,8u,4u);
        CopyEopNativeLabel(scheduler,*owner,owner->Offset(far_address),proof,16u,far_bytes);
        for(uint32_t i=0;i<canaries.size();++i)CopyEopNativeLabel(scheduler,*owner,owner->Offset(canaries[i]),proof,16u+far_bytes+i*4u,4u);
        scheduler.Finish();scheduler.DrainPriorityOperations();proof.Invalidate();const auto* words=static_cast<const uint32_t*>(proof.data);
        r.final_native=words[0]==(test==3u?new_low:low)&&(!cross||words[1]==high)&&words[2]==(sibling?near_value:old)&&
          std::all_of(words+4u,words+4u+far_bytes/4u,[](uint32_t v){return v==far_value;});
        r.guards=true;for(uint32_t i=0;i<canaries.size();++i){uint32_t v=0;r.guards=r.guards&&words[4u+far_bytes/4u+i]==old&&
          LibKernel::Memory::TryReadBacking(canaries[i],&v,4u)&&v==old;}
        std::vector<uint32_t> actual_far(far_bytes/4u);
        r.final_backing=LibKernel::Memory::TryReadBacking(far_address,actual_far.data(),far_bytes)&&
          std::all_of(actual_far.begin(),actual_far.end(),[](uint32_t v){return v==far_value;})&&r.requested&&r.near_correct;
        r.clean=!PerformanceMemoryTestAccess::Readback(cache,a,size).gpu_dirty&&!PerformanceMemoryTestAccess::Readback(cache,far_address,far_bytes).gpu_dirty;
        r.expected_copied=size+(sibling?4u:0u)+(wide?far_bytes:0u);
        r.expected_reserved=64u+(sibling?64u:0u)+(wide?far_bytes:0u);
        scheduler.Finish();scheduler.DrainPriorityOperations();
      });
      releaser->join();{std::lock_guard lock(gate.mutex);r.gate=gate.callback_entered&&!gate.timeout;}
    }
    renderer.UnmapMemory(ExactGpuReadGuest::Base,ExactGpuReadGuest::Bytes);renderer.ShutdownGpu();LibKernel::Memory::InstallGpuResources(nullptr);cp.reset();
  }
  for(uint32_t test=0;test<results.size();++test){const auto& r=results[test];
    std::printf("[gpu-read-exact] %s setup=%u owner=%u initial=%u/%u/%u prefence=%u gate=%u read=%u sibling=%u cleanNear=%u wait=%u writeMarker=%u faultRoute=%u bytes=%llu reserve=%llu->%llu expected=%llu/%llu BstillUnarmed=%u Bold=%u nativeFinal=%u backingFinal=%u guards=%u clean=%u\n",
      labels[test],r.setup,r.owner,r.initial_native,r.initial_old,r.initial_unarmed,r.prefence,r.gate,r.requested,r.near_correct,r.near_clean,r.real_wait,r.write_marker,r.fault_origin,
      static_cast<unsigned long long>(r.budget.copied_bytes),static_cast<unsigned long long>(r.ring_before),static_cast<unsigned long long>(r.ring_after),
      static_cast<unsigned long long>(r.expected_copied),static_cast<unsigned long long>(r.expected_reserved),r.far_unarmed,r.far_old,r.final_native,r.final_backing,r.guards,r.clean);
    Require(labels[test],"ALL actual native producers/protected old bytes/real fence+priority publication/generations/whole-page siblings/canaries/cleanup BEFORE footprint",
      r.setup&&r.owner&&r.initial_native&&r.initial_old&&r.initial_unarmed&&r.prefence&&r.gate&&r.requested&&r.near_correct&&r.near_clean&&r.real_wait&&
      r.write_marker&&r.fault_origin&&r.final_native&&r.final_backing&&r.guards&&r.clean,
      "bootstrap/true producer/publication/value/guard/lifetime control failed; NOT read-footprint RED");
  }
  for(uint32_t test=0;test<results.size();++test){const auto& r=results[test];const bool wide=test>=4u;
    Require(labels[test],"explicit GPU read publishes only requested pages; actual fault and writes preserve locality",
      r.budget.copied_bytes==r.expected_copied&&r.ring_after-r.ring_before==r.expected_reserved&&
      r.budget.copy_calls==1u&&r.budget.source_barriers==1u&&r.budget.host_barriers==1u&&
      (wide?(!r.far_unarmed&&!r.far_old):(r.far_unarmed&&r.far_old)),
      "unrelated far64KiB copied/published for explicit GPU demand, or true fault/write locality was narrowed");
  }
}
} // namespace
} // namespace Libs::Graphics
