// Include after ArmedReadbackWindowCostTests.inc. Existing public PollReadMemory,
// actual finite compute/copy producers, and normal CPU ReadMemory cleanup.
namespace Libs::Graphics {
namespace {
struct PollExactPagePublicationGate {
  std::mutex mutex;std::condition_variable cv;
  bool entered=false,released=false,timed_out=false;
  void Hold() {
    std::unique_lock lock(mutex);entered=true;cv.notify_all();
    if(!cv.wait_for(lock,std::chrono::seconds(2),[&]{return released;}))timed_out=true;
  }
  void Release() {{std::lock_guard lock(mutex);released=true;}cv.notify_all();}
  bool Completed() {std::lock_guard lock(mutex);return entered&&released&&!timed_out;}
};
struct PollExactPageResult {
  bool setup=false,same_owner=false,initial_native=false,initial_old=false,initial_unarmed=false;
  bool first_pending=false,near_armed=false,old_before_publication=false,gate_ok=false;
  bool all_near_published=false,near_clean=false,far_remains_unarmed=false,far_remains_old=false;
  bool final_native=false,final_backing=false,guards=false,clean=false;
  uint64_t ring_before=0,ring_after=0,expected_bytes=0;
};

void CheckPollExactPageReadbackCost(VulkanHarness& vulkan) {
  constexpr const char* name="PollExactPageReadbackCost";
  constexpr uint32_t old=0x5a5a5a5au,low=0x12345678u,high=0x89abcdefu;
  constexpr uint32_t nearby=0x0badf00du,far_value=0x76543210u;
  std::array<PollExactPageResult,2> results{};
  namespace Exception=Common::HostException;
  Require(name,"existing real fault route",Exception::InstallHandler([](const Exception::ExceptionInfo& i) {
    if(i.type!=Exception::ExceptionType::AccessViolation||
       (i.access_violation_type!=Exception::AccessViolationType::Read&&
        i.access_violation_type!=Exception::AccessViolationType::Write))return false;
    return LibKernel::Memory::HandleGpuFault(
      i.access_violation_type==Exception::AccessViolationType::Write?PageFaultAccess::Write:PageFaultAccess::Read,
      i.access_violation_vaddr);
  }),"fault route bootstrap failed; NOT valid RED");
  {
    FetchGpuGuest guest;auto& graphics=vulkan.RuntimeContext();auto& renderer=vulkan.RuntimeRenderer();
    renderer.InitializeGpu(nullptr);LibKernel::Memory::InstallGpuResources(&renderer);
    renderer.MapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    auto& gpu=renderer.GetGpu();auto& cache=renderer.GetBufferCache();auto& scheduler=renderer.GetCommandScheduler();
    FetchGpuBuffer low_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer high_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer near_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer far_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer proof(graphics,64u,vk::BufferUsageFlagBits::eTransferDst);
    low_source.Flush();high_source.Flush();near_source.Flush();far_source.Flush();proof.Flush();
    EopComputeProducer low_compute(vulkan.Device(),low_source.buffer,low);
    EopComputeProducer high_compute(vulkan.Device(),high_source.buffer,high);
    EopComputeProducer near_compute(vulkan.Device(),near_source.buffer,nearby);
    EopComputeProducer far_compute(vulkan.Device(),far_source.buffer,far_value);
    std::unique_ptr<CommandProcessor> cp;
    gpu.SendCommandSync([&]{cp=std::make_unique<CommandProcessor>(renderer,0x31);cp->Reset();cp->BufferInit();});
    for(uint32_t test=0;test<results.size();++test) {
      auto& r=results[test];PollExactPagePublicationGate gate;
      const uint64_t a=FetchGpuGuest::Base+(test==0u?0x5000u:0x5ffcu);
      const uint64_t near_address=FetchGpuGuest::Base+(test==0u?0x5040u:0x6040u);
      const uint64_t far_address=FetchGpuGuest::Base+(test==0u?0x6000u:0x7000u);
      const uint64_t size=test==0u?4u:8u;
      const std::array<uint64_t,6> canaries{a-4u,a+size,near_address-4u,near_address+4u,far_address-4u,far_address+4u};
      for(const auto p:canaries)std::memcpy(reinterpret_cast<void*>(p),&old,4u);
      for(const auto p:{a,near_address,far_address})std::memcpy(reinterpret_cast<void*>(p),&old,4u);
      if(test==1u)std::memcpy(reinterpret_cast<void*>(a+4u),&old,4u);
      gpu.SendCommandSync([&] {
        r.setup=GuestGpu::IsGpuThread()&&!CommandRecordingDeferred()&&
            !PerformanceMemoryTestAccess::HasQueue(cache)&&graphics.readback_queue==nullptr&&!ReadbackAgeEnabled();
        ReadbackBootstrapWrite(scheduler,cache,low_compute,low_source,a);
        if(test==1u)ReadbackBootstrapWrite(scheduler,cache,high_compute,high_source,a+4u);
        ReadbackBootstrapWrite(scheduler,cache,near_compute,near_source,near_address);
        ReadbackBootstrapWrite(scheduler,cache,far_compute,far_source,far_address);
        auto [owner,offset]=cache.ObtainBuffer(a,size,false,true);
        auto [near_owner,near_offset]=cache.ObtainBuffer(near_address,4u,false,true);
        auto [far_owner,far_offset]=cache.ObtainBuffer(far_address,4u,false,true);
        r.same_owner=owner->Handle()==near_owner->Handle()&&owner->Handle()==far_owner->Handle();
        CopyEopNativeLabel(scheduler,*owner,offset,proof,0u,size);
        CopyEopNativeLabel(scheduler,*near_owner,near_offset,proof,8u,4u);
        CopyEopNativeLabel(scheduler,*far_owner,far_offset,proof,12u,4u);
        for(size_t n=0;n<canaries.size();++n) {
          auto [guard_owner,guard_offset]=cache.ObtainBuffer(canaries[n],4u,false,true);
          CopyEopNativeLabel(scheduler,*guard_owner,guard_offset,proof,16u+n*4u,4u);
        }
        const auto initial_tick=scheduler.CurrentTick();scheduler.Flush();scheduler.GetMasterSemaphore().Wait(initial_tick);
        scheduler.WaitPriorityOperations(initial_tick);proof.Invalidate();
        std::array<uint32_t,10> words{};std::memcpy(words.data(),proof.data,words.size()*4u);
        r.initial_native=words[0]==low&&(test==0u||words[1]==high)&&words[2]==nearby&&words[3]==far_value;
        for(size_t n=0;n<canaries.size();++n)r.initial_native=r.initial_native&&words[4u+n]==old;
        r.initial_old=true;r.initial_unarmed=true;
        for(const auto p:{a,near_address,far_address}) {
          uint32_t actual=0;r.initial_old=r.initial_old&&LibKernel::Memory::TryReadBacking(p,&actual,4u)&&actual==old;
          const auto s=PerformanceMemoryTestAccess::Readback(cache,p,4u);r.initial_unarmed=r.initial_unarmed&&s.gpu_dirty&&s.unarmed;
        }
        // Hold priority publication, not native work. B native commands may submit.
        scheduler.DeferPriorityOperation([&]{gate.Hold();});
        r.ring_before=StreamBufferTestAccess::Offset(cache.GetUtilityBuffer(MemoryUsage::Download));
        r.first_pending=!cache.PollReadMemory(a,size);
        r.ring_after=StreamBufferTestAccess::Offset(cache.GetUtilityBuffer(MemoryUsage::Download));
        const auto armed=PerformanceMemoryTestAccess::Readback(cache,a,size);
        r.near_armed=armed.gpu_dirty&&!armed.unarmed&&armed.tick<scheduler.CurrentTick();
        uint32_t before=0;r.old_before_publication=LibKernel::Memory::TryReadBacking(a,&before,4u)&&before==old;
        gate.Release();scheduler.Finish();scheduler.WaitPriorityOperations(scheduler.CurrentTick()-1u);
        r.gate_ok=gate.Completed();
        uint32_t lo=0,hi=0,nv=0,fv=0;
        r.all_near_published=LibKernel::Memory::TryReadBacking(a,&lo,4u)&&lo==low&&
            (test==0u||(LibKernel::Memory::TryReadBacking(a+4u,&hi,4u)&&hi==high))&&
            LibKernel::Memory::TryReadBacking(near_address,&nv,4u)&&nv==nearby;
        r.near_clean=!PerformanceMemoryTestAccess::Readback(cache,a,size).gpu_dirty&&
            !PerformanceMemoryTestAccess::Readback(cache,near_address,4u).gpu_dirty;
        const auto fs=PerformanceMemoryTestAccess::Readback(cache,far_address,4u);
        r.far_remains_unarmed=fs.gpu_dirty&&fs.unarmed;
        r.far_remains_old=LibKernel::Memory::TryReadBacking(far_address,&fv,4u)&&fv==old;
        // The unchanged CPU route must subsequently publish the independent far_address page.
        cache.ReadMemory(far_address,4u);
        auto [final_owner,final_offset]=cache.ObtainBuffer(a,size,false,true);
        auto [final_near_owner,final_near_offset]=cache.ObtainBuffer(near_address,4u,false,true);
        auto [final_far_owner,final_far_offset]=cache.ObtainBuffer(far_address,4u,false,true);
        CopyEopNativeLabel(scheduler,*final_owner,final_offset,proof,0u,size);
        CopyEopNativeLabel(scheduler,*final_near_owner,final_near_offset,proof,8u,4u);
        CopyEopNativeLabel(scheduler,*final_far_owner,final_far_offset,proof,12u,4u);
        for(size_t n=0;n<canaries.size();++n) {
          auto [guard_owner,guard_offset]=cache.ObtainBuffer(canaries[n],4u,false,true);
          CopyEopNativeLabel(scheduler,*guard_owner,guard_offset,proof,16u+n*4u,4u);
        }
        scheduler.Finish();scheduler.WaitPriorityOperations(scheduler.CurrentTick()-1u);proof.Invalidate();
        std::memcpy(words.data(),proof.data,words.size()*4u);
        r.final_native=words[0]==low&&(test==0u||words[1]==high)&&words[2]==nearby&&words[3]==far_value;
        for(size_t n=0;n<canaries.size();++n)r.final_native=r.final_native&&words[4u+n]==old;
        r.final_backing=LibKernel::Memory::TryReadBacking(far_address,&fv,4u)&&fv==far_value&&r.all_near_published;
        r.guards=true;for(const auto p:canaries) {
          uint32_t actual=0;r.guards=r.guards&&LibKernel::Memory::TryReadBacking(p,&actual,4u)&&actual==old;
        }
        r.clean=!PerformanceMemoryTestAccess::Readback(cache,a,size).gpu_dirty&&
            !PerformanceMemoryTestAccess::Readback(cache,near_address,4u).gpu_dirty&&
            !PerformanceMemoryTestAccess::Readback(cache,far_address,4u).gpu_dirty;
        // WAIT word/block is one contiguous dirty span, plus a separated near_address word.
        r.expected_bytes=2u*64u;
        scheduler.Finish();scheduler.DrainPriorityOperations();
      });
    }
    renderer.UnmapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    renderer.ShutdownGpu();LibKernel::Memory::InstallGpuResources(nullptr);cp.reset();
  }
  for(uint32_t test=0;test<results.size();++test) {
    const auto& r=results[test];
    std::printf("[poll-exact-page] case=%u initial=%u/%u/%u sameOwner=%u pollPending=%u nearArmed=%u oldPending=%u gate=%u nearPublished=%u nearClean=%u farUnarmed=%u farOld=%u ring=%llu->%llu budget=%llu finalNative=%u finalBacking=%u guards=%u clean=%u\n",
      test,r.initial_native,r.initial_old,r.initial_unarmed,r.same_owner,r.first_pending,r.near_armed,
      r.old_before_publication,r.gate_ok,r.all_near_published,r.near_clean,r.far_remains_unarmed,r.far_remains_old,
      static_cast<unsigned long long>(r.ring_before),static_cast<unsigned long long>(r.ring_after),
      static_cast<unsigned long long>(r.expected_bytes),r.final_native,r.final_backing,r.guards,r.clean);
    Require(name,"real producers/whole touched pages/native canaries/publication/CPU far_address cleanup BEFORE cost",
      r.setup&&r.same_owner&&r.initial_native&&r.initial_old&&r.initial_unarmed&&r.first_pending&&
      r.near_armed&&r.old_before_publication&&r.gate_ok&&r.all_near_published&&r.near_clean&&
      r.final_native&&r.final_backing&&r.guards&&r.clean,
      "setup/native/touched-page/fence/canary/cleanup failure; NOT intended copy-footprint RED");
  }
  for(const auto& r:results)
    Require(name,"PM4 label polling copies only every dirty span on touched tracker pages",
      r.ring_after-r.ring_before==r.expected_bytes&&r.far_remains_unarmed&&r.far_remains_old,
      "PM4 wait inherited CPU512KiB locality window and armed/copied unrelated far_address dirty page");
}
} // namespace
} // namespace Libs::Graphics
