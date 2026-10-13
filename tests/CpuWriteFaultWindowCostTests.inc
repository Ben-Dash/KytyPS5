// Include after GpuReadMemoryExactPageCostTests.inc and existing native readback helpers.
// Genuine foreign protected CPU writes: no manually injected dirty/readiness state.
namespace Libs::Graphics {
namespace {
void ConfigureCpuWriteFaultWindowCostTests() {
  ConfigureEagerReadbackAdjacentPagesCostTests();
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  _putenv_s("KYTY_FAULT_AHEAD_PAGES","16");
  _putenv_s("KYTY_CPU_WRITE_FAULT_WINDOW","1");
  _putenv_s("KYTY_GPU_READ_EXACT_PAGES","0");
  _putenv_s("KYTY_BDA_UPLOAD_BATCH","0");
#else
  setenv("KYTY_FAULT_AHEAD_PAGES","16",1);setenv("KYTY_CPU_WRITE_FAULT_WINDOW","1",1);
  setenv("KYTY_GPU_READ_EXACT_PAGES","0",1);setenv("KYTY_BDA_UPLOAD_BATCH","0",1);
#endif
}
struct CpuWriteFaultWitness {
  static inline thread_local CpuWriteFaultWitness* active=nullptr;
  uint64_t address=0;uint32_t expected_gpu=0,faults=0;bool before_retry=false;
  static bool Handle(const Common::HostException::ExceptionInfo& i) {
    namespace E=Common::HostException;
    if(i.type!=E::ExceptionType::AccessViolation||
       (i.access_violation_type!=E::AccessViolationType::Read&&i.access_violation_type!=E::AccessViolationType::Write))return false;
    auto* witness=active;
    const bool actual_write=witness!=nullptr&&i.access_violation_type==E::AccessViolationType::Write&&
      i.access_violation_vaddr==witness->address;
    if(actual_write)++witness->faults;
    const bool handled=LibKernel::Memory::HandleGpuFault(
      i.access_violation_type==E::AccessViolationType::Write?PageFaultAccess::Write:PageFaultAccess::Read,i.access_violation_vaddr);
    // Read-only witness AFTER the real handler publishes GPU bytes, BEFORE CPU retries its store.
    if(actual_write&&handled){uint32_t value=0;witness->before_retry=
      LibKernel::Memory::TryReadBacking(witness->address,&value,4u)&&value==witness->expected_gpu;}
    return handled;
  }
};
void CheckCpuWriteFaultWindowCost(VulkanHarness& vulkan) {
  constexpr const char* name="CpuWriteFaultWindowCost";
  constexpr uint64_t base=ExactGpuReadGuest::Base,owner_bytes=ExactGpuReadGuest::Bytes;
  constexpr uint64_t a=base+0x5000u,sibling=base+0x5040u,tail=base+0xf040u,far_address=base+0x20000u;
  constexpr uint32_t window=64u*1024u,far_bytes=64u*1024u,old=0x5a5a5a5au;
  constexpr uint32_t a_value=0x12345678u,new_a_value=0xabcdef09u,sibling_value=0x2468ace0u;
  constexpr uint32_t tail_value=0x0badf00du,far_value=0x76543210u,cpu_value=0x13579bdfu;
  constexpr std::array labels{"actual-completed-producer-write-fault","actual-new-current-producer-write-fault","explicit-invalidate-remains-wide"};
  struct Result {
    bool setup=false,owner=false,initial_native=false,protected_old=false,foreign=false;
    bool before_retry=false,handler_count=false,pending_new=true,real_wait=true,cpu_value_correct=false;
    bool local_gpu_siblings=false,cpu_marked=false,far_unarmed=false,far_old=false;
    bool native_final=false,backing_final=false,guards=false,clean=false;
    uint32_t faults=0;uint64_t before=0,after=0,tick_before=0,tick_after=0;EagerNativeBudget budget;
  };
  std::array<Result,3> results{};
  Require(name,"existing actual fault handler",Common::HostException::InstallHandler(CpuWriteFaultWitness::Handle),
    "fault route bootstrap failed; NOT intended RED");
  {
    ExactGpuReadGuest guest;auto& graphics=vulkan.RuntimeContext();auto& renderer=vulkan.RuntimeRenderer();
    renderer.InitializeGpu(nullptr);LibKernel::Memory::InstallGpuResources(&renderer);renderer.MapMemory(base,owner_bytes);
    auto& gpu=renderer.GetGpu();auto& cache=renderer.GetBufferCache();auto& scheduler=renderer.GetCommandScheduler();
    std::unique_ptr<CommandProcessor> cp;Buffer* owner=nullptr;
    FetchGpuBuffer a_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer new_a_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer sibling_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer tail_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer proof(graphics,window+far_bytes,vk::BufferUsageFlagBits::eTransferDst);
    a_source.Flush();new_a_source.Flush();sibling_source.Flush();tail_source.Flush();proof.Flush();
    EopComputeProducer a_compute(vulkan.Device(),a_source.buffer,a_value),new_a_compute(vulkan.Device(),new_a_source.buffer,new_a_value);
    EopComputeProducer sibling_compute(vulkan.Device(),sibling_source.buffer,sibling_value),tail_compute(vulkan.Device(),tail_source.buffer,tail_value);
    std::vector<uint32_t> expected(window/4u+far_bytes/4u,old),initial=expected;
    expected[(a-base)/4u]=cpu_value;initial[(a-base)/4u]=a_value;
    for(auto* words:{&expected,&initial}) {
      (*words)[(sibling-base)/4u]=sibling_value;(*words)[(tail-base)/4u]=tail_value;
      std::fill(words->begin()+window/4u,words->end(),far_value);
    }
    const auto snapshot=[&]{
      CopyEopNativeLabel(scheduler,*owner,owner->Offset(base),proof,0u,window);
      CopyEopNativeLabel(scheduler,*owner,owner->Offset(far_address),proof,window,far_bytes);
    };
    gpu.SendCommandSync([&]{cp=std::make_unique<CommandProcessor>(renderer,0x31);cp->Reset();cp->BufferInit();
      owner=cache.ObtainBuffer(base,owner_bytes,true,true).first;});
    for(uint32_t test=0;test<results.size();++test) {
      auto& r=results[test];CpuWriteFaultWitness witness{a,test==1u?new_a_value:a_value};
      gpu.SendCommandSync([&]{
        // Reset real native owner and backing outside the requested-write observer.
        (void)cache.ObtainBuffer(base,owner_bytes,true,true);owner->Fill(0u,owner_bytes,old);cache.OnCommandRecorded();
        scheduler.Finish();scheduler.DrainPriorityOperations();cache.ReadMemory(base,owner_bytes);
        scheduler.Finish();scheduler.DrainPriorityOperations();PerformanceMemoryTestAccess::ResetEagerHotPolicy(cache);
        r.setup=GuestGpu::IsGpuThread()&&!CommandRecordingDeferred()&&!DrainStats::Enabled()&&!ReadbackAgeEnabled()&&
          !PerformanceMemoryTestAccess::HasQueue(cache)&&graphics.readback_queue==nullptr;
        ReadbackBootstrapWrite(scheduler,cache,a_compute,a_source,a);
        ReadbackBootstrapWrite(scheduler,cache,sibling_compute,sibling_source,sibling);
        ReadbackBootstrapWrite(scheduler,cache,tail_compute,tail_source,tail);
        auto [far_owner,offset]=cache.ObtainBuffer(far_address,far_bytes,true,true);
        far_owner->Fill(offset,far_bytes,far_value);cache.OnCommandRecorded();
        r.owner=far_owner==owner&&owner->CpuAddress()==base&&owner->Size()==owner_bytes;
        snapshot();const auto producer=scheduler.CurrentTick();scheduler.Flush();scheduler.GetMasterSemaphore().Wait(producer);
        scheduler.WaitPriorityOperations(producer);proof.Invalidate();r.initial_native=std::memcmp(proof.data,initial.data(),proof.size)==0;
        r.protected_old=true;
        for(const auto address:{a,sibling,tail,far_address}) {
          uint32_t value=0;const auto state=PerformanceMemoryTestAccess::Readback(cache,address,4u);
          r.protected_old=r.protected_old&&state.gpu_dirty&&state.unarmed&&
            LibKernel::Memory::TryReadBacking(address,&value,4u)&&value==old;
        }
        if(test==1u) {
          // A genuinely later native GPU rewrite must supersede the prior completed A producer.
          ReadbackBootstrapWrite(scheduler,cache,new_a_compute,new_a_source,a);
          r.pending_new=!scheduler.GetMasterSemaphore().IsFree(scheduler.CurrentTick());
        }
        r.tick_before=scheduler.CurrentTick();r.before=StreamBufferTestAccess::Offset(cache.GetUtilityBuffer(MemoryUsage::Download));
        r.budget.sources={static_cast<VkBuffer>(owner->Handle()),static_cast<VkBuffer>(owner->Handle())};
        r.budget.download=static_cast<VkBuffer>(cache.GetUtilityBuffer(MemoryUsage::Download).Handle());
      });
      {
        EagerNativeBudget::Scope observe(r.budget);
        // Driver observer is TLS on the actual GPU callback thread; no tracker/readiness injection.
        gpu.SendCommandSync([&]{EagerNativeBudget::active=&r.budget;});
        std::jthread writer([&]{
          r.foreign=!GuestGpu::IsGpuThread();CpuWriteFaultWitness::active=&witness;
          if(test==2u)cache.InvalidateMemory(a,4u); // Explicit public API control, not a hardware-fault claim.
          *reinterpret_cast<volatile uint32_t*>(a)=cpu_value; // ACTUAL protected write faults in cases 0/1.
          CpuWriteFaultWitness::active=nullptr;
        });writer.join();
        gpu.SendCommandSync([&]{EagerNativeBudget::active=nullptr;
          r.after=StreamBufferTestAccess::Offset(cache.GetUtilityBuffer(MemoryUsage::Download));r.tick_after=scheduler.CurrentTick();});
      }
      r.faults=witness.faults;r.handler_count=test==2u?witness.faults==0u:witness.faults>0u;
      r.before_retry=test==2u||witness.before_retry;
      gpu.SendCommandSync([&]{
        uint32_t value=0;r.cpu_value_correct=LibKernel::Memory::TryReadBacking(a,&value,4u)&&value==cpu_value;
        r.local_gpu_siblings=LibKernel::Memory::TryReadBacking(sibling,&value,4u)&&value==sibling_value&&
          LibKernel::Memory::TryReadBacking(tail,&value,4u)&&value==tail_value;
        r.cpu_marked=cache.IsRegionCpuModified(a,4u);
        const auto far_state=PerformanceMemoryTestAccess::Readback(cache,far_address,far_bytes);
        r.far_unarmed=far_state.gpu_dirty&&far_state.unarmed;
        r.far_old=LibKernel::Memory::TryReadBacking(far_address,&value,4u)&&value==old;
        if(test==1u)r.real_wait=r.tick_after==r.tick_before+1u&&scheduler.GetMasterSemaphore().IsFree(r.tick_before);
        // Genuine BDA upload of the CPU-new A, then independent native reads of EVERY word/guard.
        renderer.AdvanceBdaEpoch();renderer.PrepareBda();snapshot();scheduler.Finish();scheduler.DrainPriorityOperations();proof.Invalidate();
        r.native_final=std::memcmp(proof.data,expected.data(),proof.size)==0;
        // Far publication/cleanup is demanded separately AFTER the cost observation in both implementations.
        cache.ReadMemory(base,owner_bytes);scheduler.Finish();scheduler.DrainPriorityOperations();
        std::vector<uint32_t> backing(expected.size());
        r.backing_final=LibKernel::Memory::TryReadBacking(base,backing.data(),window)&&
          LibKernel::Memory::TryReadBacking(far_address,backing.data()+window/4u,far_bytes)&&backing==expected;
        constexpr std::array<uint64_t,8> guards{a-4u,a+4u,sibling-4u,sibling+4u,tail-4u,tail+4u,far_address-4u,far_address+far_bytes};
        r.guards=true;for(const auto address:guards)r.guards=r.guards&&LibKernel::Memory::TryReadBacking(address,&value,4u)&&value==old;
        r.clean=!PerformanceMemoryTestAccess::Readback(cache,base,owner_bytes).gpu_dirty;
        scheduler.Finish();scheduler.DrainPriorityOperations();
      });
    }
    renderer.UnmapMemory(base,owner_bytes);renderer.ShutdownGpu();LibKernel::Memory::InstallGpuResources(nullptr);cp.reset();
  }
  // Every setup/generation/native byte/CPU RMW/fence/publication/lifetime control is checked BEFORE cost.
  for(uint32_t test=0;test<results.size();++test){const auto& r=results[test];
    std::printf("[cpu-write-fault-window] %s setup=%u owner=%u initialNative=%u protectedOld=%u foreign=%u faults=%u handlerGpuBeforeRetry=%u pendingNew=%u realWait=%u cpuNew=%u localGpuSiblings=%u cpuMarked=%u farUnarmed=%u farOld=%u nativeAllWords=%u backingAllWords=%u guards=%u clean=%u copies=%u bytes=%llu ring=%llu->%llu tick=%llu->%llu\n",
      labels[test],r.setup,r.owner,r.initial_native,r.protected_old,r.foreign,r.faults,r.before_retry,r.pending_new,r.real_wait,r.cpu_value_correct,r.local_gpu_siblings,
      r.cpu_marked,r.far_unarmed,r.far_old,r.native_final,r.backing_final,r.guards,r.clean,r.budget.copy_calls,
      static_cast<unsigned long long>(r.budget.copied_bytes),static_cast<unsigned long long>(r.before),static_cast<unsigned long long>(r.after),
      static_cast<unsigned long long>(r.tick_before),static_cast<unsigned long long>(r.tick_after));
    Require(labels[test],"actual foreign protected CPU WRITE, fresh GPU publication BEFORE retry, native BDA RMW bytes/all guards/cleanup BEFORE locality cost",
      r.setup&&r.owner&&r.initial_native&&r.protected_old&&r.foreign&&r.handler_count&&r.before_retry&&r.pending_new&&r.real_wait&&
      r.cpu_value_correct&&r.local_gpu_siblings&&r.cpu_marked&&r.native_final&&r.backing_final&&r.guards&&r.clean,
      "actual write fault/producer/native RMW/publication/guard/lifetime failure; NOT intended locality RED");
  }
  for(uint32_t test=0;test<results.size();++test){const auto& r=results[test];const bool wide=test==2u;
    Require(labels[test],"hardware WRITE preserves all chosen 64KiB page bytes without publishing unrelated far64KiB; explicit invalidation stays wide",
      r.budget.copy_calls==1u&&r.budget.source_barriers==1u&&r.budget.host_barriers==1u&&
      r.budget.copied_bytes==12u+(wide?far_bytes:0u)&&r.after-r.before==192u+(wide?far_bytes:0u)&&
      (wide?(!r.far_unarmed&&!r.far_old):(r.far_unarmed&&r.far_old)),
      "real hardware write RMW widens to unrelated GPU dirty data, or explicit-write contract was narrowed");
  }
}
} // namespace
} // namespace Libs::Graphics
