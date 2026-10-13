// Include AFTER CpuWriteFaultWindowNeighbors.inc; separate real 64KiB-window boundary case.
namespace Libs::Graphics {
namespace {
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void CpuWriteBoundaryObservedStore(uint64_t address,uint64_t value,CpuWriteNeighborWitness* witness) {
  // The real handler observes this TLS pointer asynchronously during the hardware fault.
  // Prevent an inline memcpy from eliminating/reordering its otherwise unobserved registration.
  CpuWriteNeighborWitness::active=witness;
  std::atomic_signal_fence(std::memory_order_seq_cst);
  std::memcpy(reinterpret_cast<void*>(address),&value,sizeof(value));
  std::atomic_signal_fence(std::memory_order_seq_cst);
  CpuWriteNeighborWitness::active=nullptr;
}
void CheckCpuWriteFaultWindowBoundary(VulkanHarness& vulkan) {
  constexpr const char* name="CpuWriteFaultWindowBoundary";
  constexpr uint64_t base=ExactGpuReadGuest::Base,owner_bytes=ExactGpuReadGuest::Bytes,far_address=base+0x20000u,tail=base+0xf040u;
  constexpr uint32_t window=128u*1024u,far_bytes=64u*1024u,old=0x5a5a5a5au;
  constexpr uint32_t a_value=0x12345678u,high_value=0x89abcdefu;
  constexpr uint32_t sibling_value=0x2468ace0u,tail_value=0x0badf00du,far_value=0x76543210u;
  constexpr uint32_t cpu_low=0x13579bdfu,cpu_high=0xabcdef67u;
  struct Result {
    bool setup=false,initial=false,protected_old=false,foreign=false,handler=false,true_fence=false,cpu_marked=false;
    bool local_backing=false,far_old=false,far_unarmed=false,native_all=false,backing_all=false,guards=false,clean=false;
    uint32_t writes=0,reads=0;uint64_t before=0,after=0,tick_before=0,tick_after=0;EagerNativeBudget budget;
  } r;
  Require(name,"actual host fault route",Common::HostException::InstallHandler(CpuWriteNeighborWitness::Handle),"bootstrap failure; NOT cost RED");
  {
    ExactGpuReadGuest guest;auto& graphics=vulkan.RuntimeContext();auto& renderer=vulkan.RuntimeRenderer();
    renderer.InitializeGpu(nullptr);LibKernel::Memory::InstallGpuResources(&renderer);renderer.MapMemory(base,owner_bytes);
    auto& gpu=renderer.GetGpu();auto& cache=renderer.GetBufferCache();auto& scheduler=renderer.GetCommandScheduler();
    std::unique_ptr<CommandProcessor> cp;Buffer* owner=nullptr;
    FetchGpuBuffer low_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer high_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer sibling_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer tail_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer proof(graphics,window+far_bytes,vk::BufferUsageFlagBits::eTransferDst);
    low_source.Flush();high_source.Flush();sibling_source.Flush();tail_source.Flush();proof.Flush();
    EopComputeProducer low_compute(vulkan.Device(),low_source.buffer,a_value),high_compute(vulkan.Device(),high_source.buffer,high_value);
    EopComputeProducer sibling_compute(vulkan.Device(),sibling_source.buffer,sibling_value);
    EopComputeProducer tail_compute(vulkan.Device(),tail_source.buffer,tail_value);
    const auto snapshot=[&]{CopyEopNativeLabel(scheduler,*owner,owner->Offset(base),proof,0u,window);
      CopyEopNativeLabel(scheduler,*owner,owner->Offset(far_address),proof,window,far_bytes);};
    gpu.SendCommandSync([&]{cp=std::make_unique<CommandProcessor>(renderer,0x31);cp->Reset();cp->BufferInit();
      owner=cache.ObtainBuffer(base,owner_bytes,true,true).first;});
    {
      const uint64_t a=base+0xfffcu,sibling=base+0xffc0u;
      constexpr uint32_t bytes=8u;
      std::vector<uint32_t> initial((window+far_bytes)/4u,old),expected=initial;
      for(auto* words:{&initial,&expected}) {
        (*words)[(a-base)/4u]=a_value;(*words)[(a+4u-base)/4u]=high_value;
        (*words)[(sibling-base)/4u]=sibling_value;(*words)[(tail-base)/4u]=tail_value;
        std::fill(words->begin()+window/4u,words->end(),far_value);
      }
      expected[(a-base)/4u]=cpu_low;expected[(a+4u-base)/4u]=cpu_high;
      CpuWriteNeighborWitness witness{a,bytes,a_value,high_value};
      gpu.SendCommandSync([&]{
        (void)cache.ObtainBuffer(base,owner_bytes,true,true);owner->Fill(0u,owner_bytes,old);cache.OnCommandRecorded();
        scheduler.Finish();scheduler.DrainPriorityOperations();cache.ReadMemory(base,owner_bytes);scheduler.Finish();scheduler.DrainPriorityOperations();
        PerformanceMemoryTestAccess::ResetEagerHotPolicy(cache);
        r.setup=GuestGpu::IsGpuThread()&&!CommandRecordingDeferred()&&!DrainStats::Enabled()&&!ReadbackAgeEnabled()&&
          !PerformanceMemoryTestAccess::HasQueue(cache)&&graphics.readback_queue==nullptr&&owner->CpuAddress()==base&&owner->Size()==owner_bytes;
        ReadbackBootstrapWrite(scheduler,cache,low_compute,low_source,a);
        ReadbackBootstrapWrite(scheduler,cache,high_compute,high_source,a+4u);
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
        r.tick_before=scheduler.CurrentTick();r.before=StreamBufferTestAccess::Offset(cache.GetUtilityBuffer(MemoryUsage::Download));
        r.budget.sources={static_cast<VkBuffer>(owner->Handle()),static_cast<VkBuffer>(owner->Handle())};
        r.budget.download=static_cast<VkBuffer>(cache.GetUtilityBuffer(MemoryUsage::Download).Handle());
      });
      {
        EagerNativeBudget::Scope observe(r.budget);gpu.SendCommandSync([&]{EagerNativeBudget::active=&r.budget;});
        std::jthread writer([&]{r.foreign=!GuestGpu::IsGpuThread();
          // Defined unaligned memcpy across real protected64KiB windows; no width given to the handler.
          const uint64_t cpu_pair=static_cast<uint64_t>(cpu_low)|(static_cast<uint64_t>(cpu_high)<<32u);
          CpuWriteBoundaryObservedStore(a,cpu_pair,&witness);
        });writer.join();
        gpu.SendCommandSync([&]{EagerNativeBudget::active=nullptr;r.after=StreamBufferTestAccess::Offset(cache.GetUtilityBuffer(MemoryUsage::Download));r.tick_after=scheduler.CurrentTick();});
      }
      r.writes=witness.writes;r.reads=witness.reads;r.handler=witness.writes>0u&&witness.reads==0u&&witness.before_retry;
      gpu.SendCommandSync([&]{
        scheduler.DrainPriorityOperations();uint32_t value=0,high_cpu=0;
        r.local_backing=LibKernel::Memory::TryReadBacking(a,&value,4u)&&value==cpu_low&&
          LibKernel::Memory::TryReadBacking(a+4u,&high_cpu,4u)&&high_cpu==cpu_high&&
          LibKernel::Memory::TryReadBacking(sibling,&value,4u)&&value==sibling_value&&
          LibKernel::Memory::TryReadBacking(tail,&value,4u)&&value==tail_value;
        r.cpu_marked=cache.IsRegionCpuModified(a,bytes);r.true_fence=r.tick_after>r.tick_before&&scheduler.GetMasterSemaphore().IsFree(r.tick_before);
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
    std::printf("[cpu-write-fault-boundary] setup=%u initialAllNative=%u oldProtected=%u foreign=%u actualWRITE=%u READ=%u handlerFresh=%u trueFence=%u local=%u cpuMark=%u farUnarmed=%u farOld=%u nativeAll=%u backingAll=%u guards=%u clean=%u bytes=%llu ring=%llu->%llu\n",
      r.setup,r.initial,r.protected_old,r.foreign,r.writes,r.reads,r.handler,r.true_fence,
      r.local_backing,r.cpu_marked,r.far_unarmed,r.far_old,r.native_all,r.backing_all,r.guards,r.clean,
      static_cast<unsigned long long>(r.budget.copied_bytes),static_cast<unsigned long long>(r.before),static_cast<unsigned long long>(r.after));
    Require(name,"actual protected cross64KiB store, native every word/guards/publication/cleanup BEFORE cost",
      r.setup&&r.initial&&r.protected_old&&r.foreign&&r.handler&&r.true_fence&&r.local_backing&&
      r.cpu_marked&&r.native_all&&r.backing_all&&r.guards&&r.clean,"boundary true producer/native store/publication/lifetime control failed");
    Require(name,"write locality retains both64KiB windows and excludes unrelated far64KiB",
      r.writes==2u&&r.tick_after==r.tick_before+2u&&r.budget.copy_calls==2u&&r.budget.source_barriers==2u&&r.budget.host_barriers==2u&&r.budget.copied_bytes==16u&&
      r.after-r.before==256u&&r.far_unarmed&&r.far_old,"boundary write footprint widened or unrelated page published");
}
} // namespace
} // namespace Libs::Graphics
