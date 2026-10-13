// Include AFTER ArmedReadbackTypedNeighbors.inc and WaitRegMemNativeNonblockingCostTests.inc.
// New Poll API neighbors only AFTER the original existing-PM4 cost RED/GREEN.
namespace Libs::Graphics {
namespace {
struct WaitPollBudgetScope {
  WaitRegMemWaitBudget budget;
  void (*previous)(DrainStats::Kind)=DrainStats::g_wait_timer_test_observer;
  WaitPollBudgetScope(){WaitRegMemWaitBudget::active=&budget;DrainStats::g_wait_timer_test_observer=WaitRegMemWaitBudget::Observe;}
  ~WaitPollBudgetScope(){DrainStats::g_wait_timer_test_observer=previous;WaitRegMemWaitBudget::active=nullptr;}
};
bool FinishActualPoll(BufferCache& cache,uint64_t address,uint64_t bytes) {
  const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  do {if(cache.PollReadMemory(address,bytes))return true;std::this_thread::yield();}
  while(std::chrono::steady_clock::now()<end);
  return false;
}
void CheckWaitRegMemNativePollNeighbors(VulkanHarness& vulkan) {
  constexpr const char* name="WaitRegMemNativePollNeighbors";
  constexpr uint32_t old=0x5a5a5a5au,literal=0x12345678u,gpu_new=0xabcdef09u,cpu_new=0x13579bdfu;
  constexpr uint64_t a=FetchGpuGuest::Base+0x5000u,b=FetchGpuGuest::Base+0x6000u;
  namespace Exception=Common::HostException;
  Require(name,"existing real fault route",Exception::InstallHandler([](const Exception::ExceptionInfo& i) {
    if(i.type!=Exception::ExceptionType::AccessViolation ||
       (i.access_violation_type!=Exception::AccessViolationType::Read &&
        i.access_violation_type!=Exception::AccessViolationType::Write))return false;
    return LibKernel::Memory::HandleGpuFault(i.access_violation_type==Exception::AccessViolationType::Write ?
        PageFaultAccess::Write:PageFaultAccess::Read,i.access_violation_vaddr);
  }),"native fault route setup failed");
  auto& renderer=vulkan.RuntimeRenderer();renderer.InitializeGpu(nullptr);
  auto& gpu=renderer.GetGpu();LibKernel::Memory::InstallGpuResources(&renderer);
  {
    FetchGpuGuest guest;renderer.MapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    auto& graphics=vulkan.RuntimeContext();
    FetchGpuBuffer source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer proof(graphics,16u,vk::BufferUsageFlagBits::eTransferDst);
    source.Flush();proof.Flush();EopComputeProducer compute(vulkan.Device(),source.buffer,gpu_new);
    std::unique_ptr<CommandProcessor> cp;
    gpu.SendCommandSync([&]{cp=std::make_unique<CommandProcessor>(renderer,0x31);cp->Reset();cp->BufferInit();});
    for(uint32_t test=0;test<3;++test) {
      const uint64_t address=test==2?FetchGpuGuest::Base+0x5ffcu:a;
      const uint64_t neighbor=test==2?address+4u:b;
      for(const auto p:{address-4u,address,address+4u,neighbor,neighbor+4u})
        std::memcpy(reinterpret_cast<void*>(p),&old,4u);
      ArmedWindowGate gate;std::unique_ptr<std::jthread> release;
      if(test==0)release=std::make_unique<std::jthread>([&]{gate.ReleaseAfterReadStarts();});
      bool before_ok=false,first_unready=true,ready=false,retoken_kept=true,guards=false,values=false;
      uint32_t got[2]{},native[2]{};uint64_t tick_before=0,tick_after=0;uint32_t waits=0;
      gpu.SendCommandSync([&] {
        auto& scheduler=renderer.GetCommandScheduler();auto& cache=renderer.GetBufferCache();
        if(test==0)scheduler.DeferPriorityOperation([&]{gate.Hold();});
        cp->WriteAtEndOfPipe32(0u,0u,0x2fu,0u,6u,2u,reinterpret_cast<void*>(address),literal,0u);
        if(test==0) {
          const auto submitted=scheduler.CurrentTick();scheduler.Flush();scheduler.GetMasterSemaphore().Wait(submitted);
          {std::unique_lock lock(gate.mutex);
           before_ok=gate.cv.wait_for(lock,std::chrono::seconds(2),[&]{return gate.callback_entered;});}
          const auto armed=PerformanceMemoryTestAccess::Readback(cache,address,4u);
          before_ok=before_ok && armed.gpu_dirty && !armed.unarmed;
          scheduler.DeferPriorityOperation([&]{retoken_kept=cache.IsRegionGpuModified(address,4u);});
          // A real unknown native writer invalidates the earlier known-store token.
          ReadbackBootstrapWrite(scheduler,cache,compute,source,address);
          before_ok=before_ok && PerformanceMemoryTestAccess::Readback(cache,address,4u).unarmed;
        } else if(test==1) {
          // A genuine CPU overwrite faults/publishes the earlier store as before;
          // the later WAIT must observe CPU bytes, never a cached literal.
          *reinterpret_cast<volatile uint32_t*>(address)=cpu_new;
          before_ok=!PerformanceMemoryTestAccess::Readback(cache,address,4u).gpu_dirty;
        } else {
          ReadbackBootstrapWrite(scheduler,cache,compute,source,neighbor);
          const auto mixed=PerformanceMemoryTestAccess::Readback(cache,address,8u);
          before_ok=mixed.gpu_dirty && mixed.unarmed;
        }
        auto [owner,offset]=cache.ObtainBuffer(address,test==2?8u:4u,false,true);
        CopyEopNativeLabel(scheduler,*owner,offset,proof,0u,test==2?8u:4u);
        tick_before=scheduler.CurrentTick();
        {
          WaitPollBudgetScope scope;
          first_unready=!cache.PollReadMemory(address,test==2?8u:4u);
          if(test==0)gate.StartRead();
          ready=FinishActualPoll(cache,address,test==2?8u:4u);
          tick_after=scheduler.CurrentTick();
          waits=scope.budget.full+scope.budget.tick+scope.budget.priority;
        }
        scheduler.Finish();scheduler.DrainPriorityOperations();proof.Invalidate();
        std::memcpy(native,proof.data,test==2?8u:4u);
        values=LibKernel::Memory::TryReadBacking(address,got,test==2?8u:4u) &&
            got[0]==(test==0?gpu_new:test==1?cpu_new:literal) && native[0]==got[0] &&
            (test!=2 || (got[1]==gpu_new && native[1]==gpu_new));
        uint32_t prefix=0,suffix=0;
        guards=LibKernel::Memory::TryReadBacking(address-4u,&prefix,4u) && prefix==old &&
               LibKernel::Memory::TryReadBacking((test==2?neighbor:address)+4u,&suffix,4u) && suffix==old &&
               !PerformanceMemoryTestAccess::Readback(cache,address,test==2?8u:4u).gpu_dirty;
      });
      if(release)release->join();
      {std::lock_guard lock(gate.mutex);before_ok=before_ok && !gate.timeout;}
      std::printf("[poll-neighbor] case=%u initialState=%u firstUnready=%u ready=%u retoken=%u native=%08x/%08x backing=%08x/%08x guards=%u waits=%u tick=%llu->%llu\n",
          test,before_ok,first_unready,ready,retoken_kept,native[0],native[1],got[0],got[1],guards,waits,
          static_cast<unsigned long long>(tick_before),static_cast<unsigned long long>(tick_after));
      Require(name,"actual native/rearm/CPU/cross-page bytes and protection controls",
          before_ok && ready && retoken_kept && values && guards,"native typed/rearmed/CPU controls failed");
      Require(name,"no stale readiness/no completion wait",waits==0 &&
          (test!=0 || first_unready) && (test!=1 || (!first_unready && tick_before==tick_after)),
          "pending protected bytes were called ready or clean CPU overwrite caused a new submit/wait");
    }
    gpu.SendCommandSync([&] {
      uint32_t host_word=1,host_done=0;
      Require(name,"unmapped host pointer control",!renderer.IsMapped(reinterpret_cast<uint64_t>(&host_word),4u),
          "native host stack unexpectedly belongs to GPU mappings");
      auto commands=LiteralCostWaitThenDone(reinterpret_cast<uint64_t>(&host_word),4u,2u,UINT32_MAX,
          reinterpret_cast<uint64_t>(&host_done),44u);
      Pm4Execution execution;const auto first=cp->Process(execution,commands);
      Require(name,"host fallback false comparator",first==Pm4ProcessResult::Blocked && host_done==0,
          "unmapped host false WAIT incorrectly advanced");
      host_word=2;const auto second=cp->Process(execution,commands);
      Require(name,"host fallback actual reread",second==Pm4ProcessResult::Complete && host_done==44,
          "unmapped host WAIT failed to reread CPU-updated value");
      renderer.GetCommandScheduler().Finish();
    });
    renderer.UnmapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);cp.reset();
  }
  renderer.ShutdownGpu();LibKernel::Memory::InstallGpuResources(nullptr);
  std::printf("[poll-neighbor] cleanup=PASS realNativeCases=3 hostFallback=PASS imageOnlyFallback=PENDING\n");
}
} // namespace
} // namespace Libs::Graphics
