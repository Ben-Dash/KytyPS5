// Include after EmptyPm4TailSubmitCostTests.inc. Required-work negatives use existing public boundary.
namespace Libs::Graphics {
namespace {
void CheckEmptyPm4TailRequiredWorkNeighbors(VulkanHarness& vulkan) {
  constexpr const char* name="EmptyPm4TailRequiredWorkNeighbors";
  constexpr uint32_t old=0x5a5a5a5au,copied=0x13579bdfu,filled=0x2468ace0u,literal=0x11223344u;
  constexpr std::array names{"transfer-only","fill-only","callbacks-only","eager-on-empty","ordered-literal"};
  struct Result {
    bool setup=false,old_before=false,eager_ready=true,callback_old=true,submitted=false;
    bool native_value=true,cpu_value=true,callback_value=true,guards=false,clean=true;
    uint64_t before=0,after=0;uint32_t calls=0,buffers=0,failed=0,actual_native=0,actual_cpu=0;
  };
  std::array<Result,5> results{};
  Require(name,"existing real memory route",Common::HostException::InstallHandler([](const Common::HostException::ExceptionInfo& i) {
    if(i.type!=Common::HostException::ExceptionType::AccessViolation||
       (i.access_violation_type!=Common::HostException::AccessViolationType::Read&&
        i.access_violation_type!=Common::HostException::AccessViolationType::Write))return false;
    return LibKernel::Memory::HandleGpuFault(i.access_violation_type==Common::HostException::AccessViolationType::Write?
      PageFaultAccess::Write:PageFaultAccess::Read,i.access_violation_vaddr);
  }),"fault bootstrap failed");
  {
    FetchGpuGuest guest;auto& graphics=vulkan.RuntimeContext();auto& renderer=vulkan.RuntimeRenderer();
    renderer.InitializeGpu(nullptr);LibKernel::Memory::InstallGpuResources(&renderer);renderer.MapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    auto& gpu=renderer.GetGpu();auto& cache=renderer.GetBufferCache();auto& scheduler=renderer.GetCommandScheduler();
    FetchGpuBuffer source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer proof(graphics,4u,vk::BufferUsageFlagBits::eTransferDst);source.Flush();proof.Flush();
    EopComputeProducer compute(vulkan.Device(),source.buffer,copied);
    std::unique_ptr<CommandProcessor> cp;
    gpu.SendCommandSync([&] {
      cp=std::make_unique<CommandProcessor>(renderer,0x31);cp->Reset();cp->BufferInit();
      // Source GPU compute is genuinely executed OUTSIDE the observed copy-only boundary.
      compute.Record(scheduler.Current().Handle());const auto tick=scheduler.CurrentTick();scheduler.Flush();
      scheduler.GetMasterSemaphore().Wait(tick);scheduler.WaitPriorityOperations(tick);
    });
    for(uint32_t test=0;test<results.size();++test) {
      auto& r=results[test];const uint64_t a=FetchGpuGuest::Base+0x4000u+test*0x1000u;
      for(const auto p:{a-4u,a,a+4u})std::memcpy(reinterpret_cast<void*>(p),&old,4u);
      std::atomic<uint32_t> callback_word{old},normal_word{0};std::atomic<bool> normal_saw_literal{false};
      gpu.SendCommandSync([&] {
        PerformanceMemoryTestAccess::ResetEagerHotPolicy(cache);
        r.setup=GuestGpu::IsGpuThread()&&!CommandRecordingDeferred()&&!DrainStats::Enabled()&&
          !PerformanceMemoryTestAccess::HasQueue(cache)&&graphics.readback_queue==nullptr;
        if(test==0u)LiteralCostCopyUnknown(scheduler,cache,source,a);
        else if(test==1u) {
          auto [owner,offset]=cache.ObtainBuffer(a,4u,true,true);owner->Fill(offset,4u,filled);cache.OnCommandRecorded();
        } else if(test==2u) {
          scheduler.DeferPriorityOperation([&]{callback_word.store(literal);});
          scheduler.DeferOperation([&]{normal_saw_literal.store(callback_word.load()==literal);normal_word.store(11u);});
          r.callback_old=callback_word.load()==old&&normal_word.load()==0;
        } else if(test==3u) {
          PerformanceMemoryTestAccess::MarkHot(cache,a);
          LiteralCostCopyUnknown(scheduler,cache,source,a);
          // Explicit Flush submits the source producer WITHOUT eager planning, leaving
          // the NEW current buffer empty but the real hot-page publication still required.
          const auto producer=scheduler.CurrentTick();scheduler.Flush();scheduler.GetMasterSemaphore().Wait(producer);
          r.eager_ready=PerformanceMemoryTestAccess::EagerReadyContains(cache,a);
        } else {
          const auto* bytes=reinterpret_cast<const uint8_t*>(&literal);cache.WriteOrderedMemory(a,std::span<const uint8_t>{bytes,4u});
        }
        uint32_t av=0;r.old_before=LibKernel::Memory::TryReadBacking(a,&av,4u)&&av==old;
        r.before=scheduler.CurrentTick();
      });
      EmptyTailSubmitBudget budget;budget.queue=static_cast<VkQueue>(graphics.queue);
      {
        EmptyTailSubmitBudget::Scope observe(budget);
        gpu.SendCommandSync([&] {
          cp->BufferFlush();r.after=scheduler.CurrentTick();r.submitted=r.after>r.before;
          if(!r.submitted) {
            // A broken optimization must fail a controlled assertion, not hang on
            // an unsubmitted tick. Mandatory explicit fallback is cleanup only.
            EmptyTailSubmitBudget::active.store(nullptr,std::memory_order_release);scheduler.Flush();
          }
          scheduler.GetMasterSemaphore().Wait(scheduler.CurrentTick()-1u);
          scheduler.WaitPriorityOperations(scheduler.CurrentTick()-1u);scheduler.PopPendingOperations();
        });
        r.calls=budget.calls.load();r.buffers=budget.buffers.load();r.failed=budget.failed.load();
      }
      gpu.SendCommandSync([&] {
        if(test!=2u) {
          auto [owner,offset]=cache.ObtainBuffer(a,4u,false,true);CopyEopNativeLabel(scheduler,*owner,offset,proof,0u,4u);
          scheduler.Finish();scheduler.WaitPriorityOperations(scheduler.CurrentTick()-1u);proof.Invalidate();
          std::memcpy(&r.actual_native,proof.data,4u);const uint32_t expected=test==1u?filled:test==4u?literal:copied;
          r.native_value=r.actual_native==expected;cache.ReadMemory(a,4u);
          r.cpu_value=LibKernel::Memory::TryReadBacking(a,&r.actual_cpu,4u)&&r.actual_cpu==expected;
          r.clean=!PerformanceMemoryTestAccess::Readback(cache,a,4u).gpu_dirty;
        } else {
          r.callback_value=callback_word.load()==literal&&normal_word.load()==11u&&normal_saw_literal.load();
        }
        uint32_t left=0,right=0;r.guards=LibKernel::Memory::TryReadBacking(a-4u,&left,4u)&&left==old&&
          LibKernel::Memory::TryReadBacking(a+4u,&right,4u)&&right==old;
        scheduler.Finish();scheduler.DrainPriorityOperations();
      });
    }
    renderer.UnmapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);renderer.ShutdownGpu();LibKernel::Memory::InstallGpuResources(nullptr);cp.reset();
  }
  for(uint32_t test=0;test<results.size();++test) {
    const auto& r=results[test];std::printf("[empty-tail-required] %s setup=%u old=%u eager=%u callbackOld=%u submitted=%u tick=%llu->%llu nativeSubmits=%u buffers=%u failed=%u native=%08x CPU=%08x callbacks=%u guards=%u clean=%u\n",
      names[test],r.setup,r.old_before,r.eager_ready,r.callback_old,r.submitted,static_cast<unsigned long long>(r.before),static_cast<unsigned long long>(r.after),
      r.calls,r.buffers,r.failed,r.actual_native,r.actual_cpu,r.callback_value,r.guards,r.clean);
    Require(names[test],"real GPU/callback/eager values and cleanup BEFORE required-submit invariant",
      r.setup&&r.old_before&&r.eager_ready&&r.callback_old&&r.failed==0&&r.native_value&&r.cpu_value&&r.callback_value&&r.guards&&r.clean,
      "native/callback/eager/value/guard/cleanup failed");
  }
  for(uint32_t test=0;test<results.size();++test) {
    const auto& r=results[test];Require(names[test],"real copy/fill/callback/eager/literal boundary remains submitted",
      r.submitted&&r.after==r.before+1u&&r.calls==1u&&r.buffers==1u,
      "empty classification skipped required commands/current-tick operations; cleanup fallback is not success");
  }
}
} // namespace
} // namespace Libs::Graphics
