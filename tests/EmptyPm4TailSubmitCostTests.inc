// Include after EagerReadbackAdjacentPagesCostTests.inc and queued/native WAIT helpers.
// Actual GuestGpu graphics queue and native queue submission; no production behavior hooks.
namespace Libs::Graphics {
namespace {
void ConfigureEmptyPm4TailSubmitCostTests() { ConfigureEagerReadbackAdjacentPagesCostTests(); }
struct EmptyTailSubmitBudget {
  VkQueue queue=VK_NULL_HANDLE;std::atomic<uint32_t> calls{0},buffers{0},failed{0};
  static inline std::atomic<EmptyTailSubmitBudget*> active{nullptr};
  static inline PFN_vkQueueSubmit original=nullptr;
  static VkResult VKAPI_PTR Submit(VkQueue queue,uint32_t count,const VkSubmitInfo* info,VkFence fence) {
    auto* observed=active.load(std::memory_order_acquire);
    if(observed!=nullptr&&queue==observed->queue) {
      observed->calls.fetch_add(1,std::memory_order_relaxed);
      uint32_t buffers=0;for(uint32_t i=0;i<count;++i)buffers+=info[i].commandBufferCount;
      observed->buffers.fetch_add(buffers,std::memory_order_relaxed);
    }
    const auto result=original(queue,count,info,fence);
    if(observed!=nullptr&&queue==observed->queue&&result!=VK_SUCCESS)observed->failed.fetch_add(1,std::memory_order_relaxed);
    return result;
  }
  struct Scope {
    explicit Scope(EmptyTailSubmitBudget& budget) {
      EXIT_IF(active.load()!=nullptr||original!=nullptr);
      original=VULKAN_HPP_DEFAULT_DISPATCHER.vkQueueSubmit;EXIT_IF(original==nullptr);
      VULKAN_HPP_DEFAULT_DISPATCHER.vkQueueSubmit=Submit;active.store(&budget,std::memory_order_release);
    }
    ~Scope() {
      active.store(nullptr,std::memory_order_release);VULKAN_HPP_DEFAULT_DISPATCHER.vkQueueSubmit=original;original=nullptr;
    }
  };
};
void CheckEmptyPm4TailSubmitCost(VulkanHarness& vulkan) {
  constexpr const char* name="EmptyPm4TailSubmitCost";
  constexpr uint64_t a=FetchGpuGuest::Base+0x5000u,done=FetchGpuGuest::Base+0xe000u;
  constexpr uint32_t old=0x5a5a5a5au,value=0x12345678u;
  bool setup=false,initial_native=false,initial_old=false,initial_unarmed=false;
  bool blocked=false,gate_entered=false,before_publication=false,real_producer=false;
  bool final_native=false,final_backing=false,final_marker=false,guards=false,clean=false,gate_ok=false;
  uint64_t tick_before=0,tick_after=0;uint32_t observed_calls=0,observed_buffers=0,actual_native=0,actual_cpu=0,actual_done=0;
  EmptyTailSubmitBudget budget;PollExactPagePublicationGate gate;
  namespace Exception=Common::HostException;
  Require(name,"existing fault bootstrap",Exception::InstallHandler([](const Exception::ExceptionInfo& i) {
    if(i.type!=Exception::ExceptionType::AccessViolation||
       (i.access_violation_type!=Exception::AccessViolationType::Read&&i.access_violation_type!=Exception::AccessViolationType::Write))return false;
    return LibKernel::Memory::HandleGpuFault(i.access_violation_type==Exception::AccessViolationType::Write?
      PageFaultAccess::Write:PageFaultAccess::Read,i.access_violation_vaddr);
  }),"existing memory route failed; NOT RED");
  {
    FetchGpuGuest guest;auto& graphics=vulkan.RuntimeContext();auto& renderer=vulkan.RuntimeRenderer();
    renderer.InitializeGpu(nullptr);LibKernel::Memory::InstallGpuResources(&renderer);
    renderer.MapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    auto& gpu=renderer.GetGpu();auto& cache=renderer.GetBufferCache();auto& scheduler=renderer.GetCommandScheduler();
    for(const auto p:{a-4u,a,a+4u})std::memcpy(reinterpret_cast<void*>(p),&old,4u);
    const uint32_t zero=0;std::memcpy(reinterpret_cast<void*>(done),&zero,4u);
    FetchGpuBuffer source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer proof(graphics,8u,vk::BufferUsageFlagBits::eTransferDst);source.Flush();proof.Flush();
    EopComputeProducer compute(vulkan.Device(),source.buffer,value);
    std::unique_ptr<CommandProcessor> cp;
    gpu.SendCommandSync([&] {
      cp=std::make_unique<CommandProcessor>(renderer,0x31);cp->Reset();cp->BufferInit();
      PerformanceMemoryTestAccess::ResetEagerHotPolicy(cache);
      setup=GuestGpu::IsGpuThread()&&!CommandRecordingDeferred()&&!DrainStats::Enabled()&&
        !PerformanceMemoryTestAccess::HasQueue(cache)&&graphics.readback_queue==nullptr;
      ReadbackBootstrapWrite(scheduler,cache,compute,source,a);
      auto [owner,offset]=cache.ObtainBuffer(a,4u,false,true);CopyEopNativeLabel(scheduler,*owner,offset,proof,0u,4u);
      const auto tick=scheduler.CurrentTick();scheduler.Flush();scheduler.GetMasterSemaphore().Wait(tick);
      scheduler.WaitPriorityOperations(tick);proof.Invalidate();std::memcpy(&actual_native,proof.data,4u);initial_native=actual_native==value;
      uint32_t av=0;initial_old=LibKernel::Memory::TryReadBacking(a,&av,4u)&&av==old;
      const auto as=PerformanceMemoryTestAccess::Readback(cache,a,4u);initial_unarmed=as.gpu_dirty&&as.unarmed;
      // The actual download callback stays pending while the real parser's first pass is inspected.
      scheduler.DeferPriorityOperation([&]{gate.Hold();});tick_before=scheduler.CurrentTick();
    });
    auto commands=LiteralCostWaitThenDone(a,4u,value,UINT32_MAX,done,11u);
    commands.insert(commands.begin(),{KYTY_PM4(2,Pm4::IT_NOP,0),0u}); // Genuine progressed prefix before WAIT.
    budget.queue=static_cast<VkQueue>(graphics.queue);
    {
      EmptyTailSubmitBudget::Scope observe(budget);
      gpu.Submit(commands,{});
      const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
      while(std::chrono::steady_clock::now()<deadline) {
        if(WaitRegMemNativeQueueTestAccess::GraphicsBlocked(gpu)){blocked=true;break;}
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      gpu.SendCommandSync([&] {
        // Wait only the already SUBMITTED actual native tick; priority gate remains closed.
        tick_after=scheduler.CurrentTick();scheduler.GetMasterSemaphore().Wait(tick_after-1u);
        const auto as=PerformanceMemoryTestAccess::Readback(cache,a,4u);uint32_t av=0,dv=0;
        before_publication=as.gpu_dirty&&!as.unarmed&&as.tick==tick_before&&
          LibKernel::Memory::TryReadBacking(a,&av,4u)&&av==old&&LibKernel::Memory::TryReadBacking(done,&dv,4u)&&dv==0;
        real_producer=tick_after>tick_before&&scheduler.GetMasterSemaphore().IsFree(tick_before);
      });
      {std::lock_guard lock(gate.mutex);gate_entered=gate.entered;}
      observed_calls=budget.calls.load();observed_buffers=budget.buffers.load();
    }
    gate.Release();gpu.WaitForIdle();
    gpu.SendCommandSync([&] {
      scheduler.Finish();scheduler.WaitPriorityOperations(scheduler.CurrentTick()-1u);
      cache.ReadMemory(a,4u);cache.ReadMemory(done,4u);
      auto [owner,offset]=cache.ObtainBuffer(a,4u,false,true);CopyEopNativeLabel(scheduler,*owner,offset,proof,4u,4u);
      scheduler.Finish();scheduler.DrainPriorityOperations();proof.Invalidate();
      std::memcpy(&actual_native,static_cast<uint8_t*>(proof.data)+4u,4u);final_native=actual_native==value;
      final_backing=LibKernel::Memory::TryReadBacking(a,&actual_cpu,4u)&&actual_cpu==value;
      final_marker=LibKernel::Memory::TryReadBacking(done,&actual_done,4u)&&actual_done==11u;
      uint32_t left=0,right=0;guards=LibKernel::Memory::TryReadBacking(a-4u,&left,4u)&&left==old&&
        LibKernel::Memory::TryReadBacking(a+4u,&right,4u)&&right==old;
      clean=!PerformanceMemoryTestAccess::Readback(cache,a,4u).gpu_dirty;
    });
    gate_ok=gate.Completed();renderer.UnmapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    renderer.ShutdownGpu();LibKernel::Memory::InstallGpuResources(nullptr);cp.reset();
  }
  std::printf("[empty-tail] setup=%u nativeInitial=%u old=%u unarmed=%u blocked=%u gate=%u/%u beforePublication=%u producer=%u tick=%llu->%llu realQueueSubmit=%u commandBuffers=%u failures=%u finalNative=%08x CPU=%08x done=%u guards=%u clean=%u\n",
    setup,initial_native,initial_old,initial_unarmed,blocked,gate_entered,gate_ok,before_publication,real_producer,
    static_cast<unsigned long long>(tick_before),static_cast<unsigned long long>(tick_after),observed_calls,observed_buffers,
    budget.failed.load(),actual_native,actual_cpu,actual_done,guards,clean);
  Require(name,"actual parser/native producer/old protected bytes/final publication/marker/guard/cleanup controls BEFORE cost",
    setup&&initial_native&&initial_old&&initial_unarmed&&blocked&&gate_entered&&gate_ok&&before_publication&&real_producer&&
    budget.failed.load()==0u&&final_native&&final_backing&&final_marker&&guards&&clean,
    "setup/producer/fence/publication/value/guard/cleanup failure; NOT redundant-tail RED");
  Require(name,"no empty submit after WAIT already submitted required producer",
    observed_calls==1u&&observed_buffers==1u&&tick_after==tick_before+1u,
    "progressed WAIT slice submitted its actual download then an unnecessary empty tail");
}
} // namespace
} // namespace Libs::Graphics
