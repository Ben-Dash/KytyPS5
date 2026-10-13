// Include AFTER CpuWriteFaultWindowCostTests.inc. The primary RED oracle is unchanged.
namespace Libs::Graphics {
namespace {
struct CpuWriteNeighborWitness {
  static inline thread_local CpuWriteNeighborWitness* active=nullptr;
  uint64_t a=0;uint32_t bytes=4,expected_low=0,expected_high=0,writes=0,reads=0;
  bool before_retry=true;
  static bool Handle(const Common::HostException::ExceptionInfo& i) {
    namespace E=Common::HostException;
    if(i.type!=E::ExceptionType::AccessViolation||
       (i.access_violation_type!=E::AccessViolationType::Read&&i.access_violation_type!=E::AccessViolationType::Write))return false;
    auto* w=active;const bool owned=w!=nullptr&&i.access_violation_vaddr>=w->a&&i.access_violation_vaddr<w->a+w->bytes;
    const bool write=i.access_violation_type==E::AccessViolationType::Write;
    if(owned){if(write)++w->writes;else ++w->reads;}
    const bool handled=LibKernel::Memory::HandleGpuFault(write?PageFaultAccess::Write:PageFaultAccess::Read,i.access_violation_vaddr);
    if(owned&&write&&handled) {
      const bool high=i.access_violation_vaddr>=w->a+4u;uint32_t value=0;
      w->before_retry=w->before_retry&&LibKernel::Memory::TryReadBacking(w->a+(high?4u:0u),&value,4u)&&
        value==(high?w->expected_high:w->expected_low);
    }
    return handled;
  }
};
void CheckCpuWriteFaultWindowNeighbors(VulkanHarness& vulkan) {
  constexpr const char* name="CpuWriteFaultWindowNeighbors";
  constexpr uint64_t base=ExactGpuReadGuest::Base,owner_bytes=ExactGpuReadGuest::Bytes,far_address=base+0x20000u,tail=base+0xf040u;
  constexpr uint32_t window=64u*1024u,far_bytes=64u*1024u,old=0x5a5a5a5au;
  constexpr uint32_t a_value=0x12345678u,high_value=0x89abcdefu,new_a=0xabcdef09u;
  constexpr uint32_t sibling_value=0x2468ace0u,tail_value=0x0badf00du,far_value=0x76543210u;
  constexpr uint32_t cpu_low=0x13579bdfu,cpu_high=0xabcdef67u,addend=0x11111111u;
  constexpr std::array labels{"atomic-RMW-write-fault","cross-page-eight-byte-write-fault","old-armed-A-new-native-writer"};
  struct Result {
    bool setup=false,initial=false,protected_old=false,foreign=false,handler=true,rmw=true;
    bool old_armed=true,new_unarmed=true,gate=true,true_fence=true,cpu_marked=false;
    bool local_backing=false,far_old=false,far_unarmed=false,native_all=false,backing_all=false,guards=false,clean=false;
    uint32_t writes=0,reads=0;uint64_t before=0,after=0,tick_before=0,tick_after=0;EagerNativeBudget budget;
  };
  std::array<Result,3> results{};
  Require(name,"actual host fault route",Common::HostException::InstallHandler(CpuWriteNeighborWitness::Handle),"bootstrap failure; NOT cost RED");
  {
    ExactGpuReadGuest guest;auto& graphics=vulkan.RuntimeContext();auto& renderer=vulkan.RuntimeRenderer();
    renderer.InitializeGpu(nullptr);LibKernel::Memory::InstallGpuResources(&renderer);renderer.MapMemory(base,owner_bytes);
    auto& gpu=renderer.GetGpu();auto& cache=renderer.GetBufferCache();auto& scheduler=renderer.GetCommandScheduler();
    std::unique_ptr<CommandProcessor> cp;Buffer* owner=nullptr;
    FetchGpuBuffer low_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer high_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer new_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer sibling_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer tail_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer proof(graphics,window+far_bytes,vk::BufferUsageFlagBits::eTransferDst);
    low_source.Flush();high_source.Flush();new_source.Flush();sibling_source.Flush();tail_source.Flush();proof.Flush();
    EopComputeProducer low_compute(vulkan.Device(),low_source.buffer,a_value),high_compute(vulkan.Device(),high_source.buffer,high_value);
    EopComputeProducer new_compute(vulkan.Device(),new_source.buffer,new_a),sibling_compute(vulkan.Device(),sibling_source.buffer,sibling_value);
    EopComputeProducer tail_compute(vulkan.Device(),tail_source.buffer,tail_value);
    const auto snapshot=[&]{CopyEopNativeLabel(scheduler,*owner,owner->Offset(base),proof,0u,window);
      CopyEopNativeLabel(scheduler,*owner,owner->Offset(far_address),proof,window,far_bytes);};
    gpu.SendCommandSync([&]{cp=std::make_unique<CommandProcessor>(renderer,0x31);cp->Reset();cp->BufferInit();
      owner=cache.ObtainBuffer(base,owner_bytes,true,true).first;});
    for(uint32_t test=0;test<results.size();++test) {
      auto& r=results[test];const bool cross=test==1u,armed=test==2u;
      const uint64_t a=base+(cross?0x5ffcu:0x5000u),sibling=base+(cross?0x6040u:0x5040u);
      const uint32_t bytes=cross?8u:4u,cpu_result=test==0u?a_value+addend:cpu_low;
      std::vector<uint32_t> initial((window+far_bytes)/4u,old),expected=initial;
      for(auto* words:{&initial,&expected}) {
        (*words)[(a-base)/4u]=a_value;if(cross)(*words)[(a+4u-base)/4u]=high_value;
        (*words)[(sibling-base)/4u]=sibling_value;(*words)[(tail-base)/4u]=tail_value;
        std::fill(words->begin()+window/4u,words->end(),far_value);
      }
      expected[(a-base)/4u]=cpu_result;if(cross)expected[(a+4u-base)/4u]=cpu_high;
      ArmedWindowGate gate;std::unique_ptr<std::jthread> releaser;
      CpuWriteNeighborWitness witness{a,bytes,armed?new_a:a_value,high_value};uint32_t rmw_old=0;
      gpu.SendCommandSync([&]{
        (void)cache.ObtainBuffer(base,owner_bytes,true,true);owner->Fill(0u,owner_bytes,old);cache.OnCommandRecorded();
        scheduler.Finish();scheduler.DrainPriorityOperations();cache.ReadMemory(base,owner_bytes);scheduler.Finish();scheduler.DrainPriorityOperations();
        PerformanceMemoryTestAccess::ResetEagerHotPolicy(cache);
        r.setup=GuestGpu::IsGpuThread()&&!CommandRecordingDeferred()&&!DrainStats::Enabled()&&!ReadbackAgeEnabled()&&
          !PerformanceMemoryTestAccess::HasQueue(cache)&&graphics.readback_queue==nullptr&&owner->CpuAddress()==base&&owner->Size()==owner_bytes;
        ReadbackBootstrapWrite(scheduler,cache,low_compute,low_source,a);
        if(cross)ReadbackBootstrapWrite(scheduler,cache,high_compute,high_source,a+4u);
        ReadbackBootstrapWrite(scheduler,cache,sibling_compute,sibling_source,sibling);
        ReadbackBootstrapWrite(scheduler,cache,tail_compute,tail_source,tail);
        auto [far_owner,offset]=cache.ObtainBuffer(far_address,far_bytes,true,true);r.setup=r.setup&&far_owner==owner;
        far_owner->Fill(offset,far_bytes,far_value);cache.OnCommandRecorded();snapshot();
        auto tick=scheduler.CurrentTick();scheduler.Flush();scheduler.GetMasterSemaphore().Wait(tick);scheduler.WaitPriorityOperations(tick);
        proof.Invalidate();r.initial=std::memcmp(proof.data,initial.data(),proof.size)==0;r.protected_old=true;
        for(const auto address:{a,sibling,tail,far_address}) {
          uint32_t value=0;const auto state=PerformanceMemoryTestAccess::Readback(cache,address,4u);
          r.protected_old=r.protected_old&&state.gpu_dirty&&state.unarmed&&
            LibKernel::Memory::TryReadBacking(address,&value,4u)&&value==old;
        }
        if(armed) {
          scheduler.DeferPriorityOperation([&]{gate.Hold();});tick=scheduler.CurrentTick();
          const bool ready=cache.PollReadMemory(a,4u);scheduler.GetMasterSemaphore().Wait(tick);
          {std::unique_lock lock(gate.mutex);r.gate=gate.cv.wait_for(lock,std::chrono::seconds(5),[&]{return gate.callback_entered;});}
          const auto state=PerformanceMemoryTestAccess::Readback(cache,a,4u);uint32_t value=0;
          r.old_armed=!ready&&state.gpu_dirty&&!state.unarmed&&state.tick==tick&&
            scheduler.GetMasterSemaphore().IsFree(tick)&&LibKernel::Memory::TryReadBacking(a,&value,4u)&&value==old;
          ReadbackBootstrapWrite(scheduler,cache,new_compute,new_source,a);
          const auto rewritten=PerformanceMemoryTestAccess::Readback(cache,a,4u);
          r.new_unarmed=rewritten.gpu_dirty&&rewritten.unarmed&&!scheduler.GetMasterSemaphore().IsFree(scheduler.CurrentTick());
        }
        r.tick_before=scheduler.CurrentTick();r.before=StreamBufferTestAccess::Offset(cache.GetUtilityBuffer(MemoryUsage::Download));
        r.budget.sources={static_cast<VkBuffer>(owner->Handle()),static_cast<VkBuffer>(owner->Handle())};
        r.budget.download=static_cast<VkBuffer>(cache.GetUtilityBuffer(MemoryUsage::Download).Handle());
      });
      if(armed)releaser=std::make_unique<std::jthread>([&]{gate.ReleaseAfterReadStarts();});
      {
        EagerNativeBudget::Scope observe(r.budget);gpu.SendCommandSync([&]{EagerNativeBudget::active=&r.budget;});
        std::jthread writer([&]{r.foreign=!GuestGpu::IsGpuThread();CpuWriteNeighborWitness::active=&witness;if(armed)gate.StartRead();
          if(test==0u)rmw_old=std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(a)).fetch_add(addend,std::memory_order_relaxed);
          else if(cross) {
            // Defined unaligned memcpy to real protected pages; width is NEVER given to the handler.
            const uint64_t cpu_pair=static_cast<uint64_t>(cpu_low)|(static_cast<uint64_t>(cpu_high)<<32u);
            std::memcpy(reinterpret_cast<void*>(a),&cpu_pair,sizeof(cpu_pair));
          } else *reinterpret_cast<volatile uint32_t*>(a)=cpu_low;
          CpuWriteNeighborWitness::active=nullptr;
        });writer.join();
        gpu.SendCommandSync([&]{EagerNativeBudget::active=nullptr;r.after=StreamBufferTestAccess::Offset(cache.GetUtilityBuffer(MemoryUsage::Download));r.tick_after=scheduler.CurrentTick();});
      }
      if(releaser){releaser->join();std::lock_guard lock(gate.mutex);r.gate=r.gate&&gate.callback_entered&&!gate.timeout;}
      r.writes=witness.writes;r.reads=witness.reads;r.handler=witness.writes>0u&&witness.reads==0u&&witness.before_retry;
      r.rmw=test!=0u||rmw_old==a_value;
      gpu.SendCommandSync([&]{
        scheduler.DrainPriorityOperations();uint32_t value=0,high_cpu=0;
        r.local_backing=LibKernel::Memory::TryReadBacking(a,&value,4u)&&value==cpu_result&&
          (!cross||(LibKernel::Memory::TryReadBacking(a+4u,&high_cpu,4u)&&high_cpu==cpu_high))&&
          LibKernel::Memory::TryReadBacking(sibling,&value,4u)&&value==sibling_value&&
          LibKernel::Memory::TryReadBacking(tail,&value,4u)&&value==tail_value;
        r.cpu_marked=cache.IsRegionCpuModified(a,bytes);r.true_fence=r.tick_after==r.tick_before+1u&&scheduler.GetMasterSemaphore().IsFree(r.tick_before);
        const auto state=PerformanceMemoryTestAccess::Readback(cache,far_address,far_bytes);
        r.far_unarmed=state.gpu_dirty&&state.unarmed;r.far_old=LibKernel::Memory::TryReadBacking(far_address,&value,4u)&&value==old;
        renderer.AdvanceBdaEpoch();renderer.PrepareBda();snapshot();scheduler.Finish();scheduler.DrainPriorityOperations();proof.Invalidate();
        r.native_all=std::memcmp(proof.data,expected.data(),proof.size)==0;
        cache.ReadMemory(base,owner_bytes);scheduler.Finish();scheduler.DrainPriorityOperations();
        std::vector<uint32_t> backing(expected.size());r.backing_all=LibKernel::Memory::TryReadBacking(base,backing.data(),window)&&
          LibKernel::Memory::TryReadBacking(far_address,backing.data()+window/4u,far_bytes)&&backing==expected;
        const std::array<uint64_t,8> guards{a-4u,a+bytes,sibling-4u,sibling+4u,tail-4u,tail+4u,far_address-4u,far_address+far_bytes};
        r.guards=true;for(const auto address:guards)r.guards=r.guards&&LibKernel::Memory::TryReadBacking(address,&value,4u)&&value==old;
        r.clean=!PerformanceMemoryTestAccess::Readback(cache,base,owner_bytes).gpu_dirty;scheduler.Finish();scheduler.DrainPriorityOperations();
      });
    }
    renderer.UnmapMemory(base,owner_bytes);renderer.ShutdownGpu();LibKernel::Memory::InstallGpuResources(nullptr);cp.reset();
  }
  for(uint32_t test=0;test<results.size();++test){const auto& r=results[test];
    std::printf("[cpu-write-fault-neighbor] %s setup=%u initialAllNative=%u oldProtected=%u foreign=%u actualWRITE=%u READ=%u handlerFresh=%u rmwOld=%u oldArmed=%u newUnarmed=%u gate=%u trueFence=%u local=%u cpuMark=%u farUnarmed=%u farOld=%u nativeAll=%u backingAll=%u guards=%u clean=%u bytes=%llu ring=%llu->%llu\n",
      labels[test],r.setup,r.initial,r.protected_old,r.foreign,r.writes,r.reads,r.handler,r.rmw,r.old_armed,r.new_unarmed,r.gate,r.true_fence,
      r.local_backing,r.cpu_marked,r.far_unarmed,r.far_old,r.native_all,r.backing_all,r.guards,r.clean,
      static_cast<unsigned long long>(r.budget.copied_bytes),static_cast<unsigned long long>(r.before),static_cast<unsigned long long>(r.after));
    Require(labels[test],"actual protected RMW/crosspage/rearmed writer, native every word/guards/publication/cleanup BEFORE cost",
      r.setup&&r.initial&&r.protected_old&&r.foreign&&r.handler&&r.rmw&&r.old_armed&&r.new_unarmed&&r.gate&&r.true_fence&&r.local_backing&&
      r.cpu_marked&&r.native_all&&r.backing_all&&r.guards&&r.clean,"neighbor true producer/native RMW/publication/lifetime control failed");
  }
  for(uint32_t test=0;test<results.size();++test){const auto& r=results[test];const uint64_t bytes=test==0u?12u:test==1u?16u:8u,reserve=test==2u?128u:192u;
    Require(labels[test],"write locality retains every sibling/current generation and excludes unrelated far64KiB",
      r.budget.copy_calls==1u&&r.budget.source_barriers==1u&&r.budget.host_barriers==1u&&r.budget.copied_bytes==bytes&&
      r.after-r.before==reserve&&r.far_unarmed&&r.far_old,"neighbor write RMW footprint widened or unrelated page published");
  }
}
} // namespace
} // namespace Libs::Graphics
