// Separate initialization neighbors; original allocation-reuse oracle remains unchanged.
namespace Libs::Graphics {
namespace {
void CheckFaultBitmapInitializationNeighborsGpu(VulkanHarness& vulkan) {
  constexpr const char* name="FaultBitmapInitializationNeighborsGpu";
  constexpr uint64_t guest_base=0x0000000201500000ull,guest_bytes=0x20000u;
  constexpr uint64_t poison_address=guest_base,current_address=guest_base+0x10000u;
  constexpr uint32_t poison_page=static_cast<uint32_t>(poison_address>>BufferCache::CACHING_PAGEBITS);
  constexpr uint32_t current_page=static_cast<uint32_t>(current_address>>BufferCache::CACHING_PAGEBITS);
  constexpr uint64_t poison_offset=uint64_t{poison_page/32u}*4u;
  constexpr uint32_t poison_value=1u<<(poison_page%32u),current_value=1u<<(current_page%32u);
  bool pinned_block=false,seeded=false,reused=false,clean_poison=false,present_current=false;
  bool observation_preserved=false,no_exposure_work=false,present_second=false,second_absent=false;
  constexpr uint64_t second_address=current_address+0x4000u;
  constexpr uint32_t second_page=static_cast<uint32_t>(second_address>>BufferCache::CACHING_PAGEBITS);
  constexpr uint32_t second_value=1u<<(second_page%32u);
  uint64_t requested_bytes=0u;
  {
    auto& graphics=vulkan.RuntimeContext();
    auto& prior=vulkan.RuntimeRenderer();auto& prior_scheduler=prior.GetCommandScheduler();
    HW::Context prior_registers{};HW::UserConfig prior_config{};HW::Shader prior_shaders{};
    prior_scheduler.Begin(prior_registers,prior_config,prior_shaders);
    auto& original_manager=BufferCacheTestAccess::Faults(prior.GetBufferCache());
    requested_bytes=FaultBitmapInitializationTestAccess::RawBitmap(original_manager).Size();
    Buffer anchor(graphics,prior_scheduler,MemoryUsage::DeviceLocal,0u,AllFlags,4096u);
    const auto anchor_info=FaultBitmapInitializationTestAccess::Allocation(anchor);
    VmaAllocationInfo previous{};
    FetchGpuBuffer probe(graphics,4u,vk::BufferUsageFlagBits::eTransferDst);
    {
      Buffer predecessor(graphics,prior_scheduler,MemoryUsage::DeviceLocal,0u,AllFlags,requested_bytes);
      previous=FaultBitmapInitializationTestAccess::Allocation(predecessor);
      pinned_block=previous.deviceMemory==anchor_info.deviceMemory;
      predecessor.Fill(0u,requested_bytes,0u);
      prior_scheduler.Current().Handle().updateBuffer(predecessor.Handle(),poison_offset,4u,&poison_value);
      CopyEopNativeLabel(prior_scheduler,predecessor,poison_offset,probe,0u,4u);
      prior_scheduler.Finish();prior_scheduler.WaitPriorityOperations(prior_scheduler.CurrentTick()-1u);
      probe.Invalidate();seeded=*static_cast<const uint32_t*>(probe.data)==poison_value;
    } // Free the completed predecessor; anchor keeps its real VMA block alive.
    {
      RenderContext tested(graphics);
      auto& cache=tested.GetBufferCache();
      auto& manager=BufferCacheTestAccess::Faults(cache);
      auto& raw=FaultBitmapInitializationTestAccess::RawBitmap(manager);
      const auto fresh=FaultBitmapInitializationTestAccess::Allocation(raw);
      reused=pinned_block&&fresh.deviceMemory==previous.deviceMemory&&fresh.offset==previous.offset&&
          fresh.size>=requested_bytes&&raw.Size()==requested_bytes;
      std::printf("[host] FaultInit predecessorMemory=%p offset=%llu size=%llu freshMemory=%p offset=%llu size=%llu pinned=%u seeded=%u exactReuse=%u\n",
          static_cast<void*>(previous.deviceMemory),static_cast<unsigned long long>(previous.offset),
          static_cast<unsigned long long>(previous.size),static_cast<void*>(fresh.deviceMemory),
          static_cast<unsigned long long>(fresh.offset),static_cast<unsigned long long>(fresh.size),
          pinned_block,seeded,reused);
      // Observe controlled raw bytes independently of any GPU-binding accessor.
      // The first-use context remains inactive, so its observations must record no commands.
      if(reused&&seeded) {
        auto& scheduler=tested.GetCommandScheduler();
        CopyEopNativeLabel(prior_scheduler,raw,poison_offset,probe,0u,4u);
        prior_scheduler.Finish();prior_scheduler.WaitPriorityOperations(prior_scheduler.CurrentTick()-1u);
        probe.Invalidate();const uint32_t before=*static_cast<const uint32_t*>(probe.data);
        const auto tick=scheduler.CurrentTick();
        const auto* observation=cache.PeekFaultBuffer();
        cache.ProcessFaultBuffer();
        no_exposure_work=observation!=nullptr&&!scheduler.Active()&&scheduler.CurrentTick()==tick;
        CopyEopNativeLabel(prior_scheduler,raw,poison_offset,probe,0u,4u);
        prior_scheduler.Finish();prior_scheduler.WaitPriorityOperations(prior_scheduler.CurrentTick()-1u);
        probe.Invalidate();observation_preserved=before==*static_cast<const uint32_t*>(probe.data);
      }
      // Never dispatch a parser over uncontrolled uninitialized memory on a setup mismatch.
      if(reused&&seeded) {
        int64_t direct=-1;
        Require(name,"real mapped guest allocation",LibKernel::Memory::KernelAllocateDirectMemory(0,
            LibKernel::Memory::KernelGetDirectMemorySize(),guest_bytes,0x10000u,0,&direct)==0,
            "owned guest page allocation failed; setup failure, not initialization RED");
        void* mapped=reinterpret_cast<void*>(guest_base);
        Require(name,"real mapped guest region",LibKernel::Memory::KernelMapDirectMemory(&mapped,
            guest_bytes,0x3,0x10,direct,0x10000u)==0&&mapped==reinterpret_cast<void*>(guest_base),
            "owned guest pages failed to map; setup failure, not initialization RED");
        std::memset(mapped,0,guest_bytes);
        LibKernel::Memory::InstallGpuResources(&tested);tested.MapMemory(guest_base,guest_bytes);
        HW::Context registers{};HW::UserConfig config{};HW::Shader shaders{};
        auto& scheduler=tested.GetCommandScheduler();scheduler.Begin(registers,config,shaders);
        // This is the real accessor used by the descriptor binder. Reuse observation above
        // deliberately used raw metadata so a future lazy first-use initializer is not triggered early.
        auto* bound=cache.GetFaultBuffer();
        FaultBitmapProducer producer(vulkan.Device(),bound->Handle(),bound->Size(),
            current_page/32u,current_value);
        auto command=scheduler.Current().Handle();
        vk::MemoryBarrier ready{};ready.srcAccessMask=vk::AccessFlagBits::eMemoryWrite;
        ready.dstAccessMask=vk::AccessFlagBits::eShaderRead|vk::AccessFlagBits::eShaderWrite;
        command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
            vk::PipelineStageFlagBits::eComputeShader,{},1u,&ready,0u,nullptr,0u,nullptr);
        producer.Record(command);
        // A repeated binding and an observational lookup after a genuine producer must
        // preserve that producer's still-pending bit rather than resetting its allocation.
        (void)cache.GetFaultBuffer();
        (void)cache.PeekFaultBuffer();
        // Actual parser/host callback: a first-Process clear would lose this legitimate bit.
        cache.ProcessFaultBuffer();
        scheduler.Finish();scheduler.WaitPriorityOperations(scheduler.CurrentTick()-1u);
        scheduler.PopPendingOperations();
        clean_poison=!cache.IsRegionRegistered(poison_address,1u);
        present_current=cache.IsRegionRegistered(current_address,1u);
        std::printf("[gpu] FaultInitNeighbor poisonRegistered=%u currentRegistered=%u observationPreserved=%u noExposureWork=%u\n",
            !clean_poison,present_current,observation_preserved,no_exposure_work);
        // A later real producer writes a different mapped cache page after the previous
        // batch has drained. Rebinding again before processing must not erase this new bit.
        second_absent=!cache.IsRegionRegistered(second_address,1u);
        bound=cache.GetFaultBuffer();
        FaultBitmapProducer second_producer(vulkan.Device(),bound->Handle(),bound->Size(),
            second_page/32u,second_value);
        command=scheduler.Current().Handle();
        command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
            vk::PipelineStageFlagBits::eComputeShader,{},1u,&ready,0u,nullptr,0u,nullptr);
        second_producer.Record(command);
        (void)cache.GetFaultBuffer();
        cache.ProcessFaultBuffer();
        scheduler.Finish();scheduler.WaitPriorityOperations(scheduler.CurrentTick()-1u);
        scheduler.PopPendingOperations();
        present_second=cache.IsRegionRegistered(second_address,1u);
        std::printf("[gpu] FaultInitNeighbor secondBatchRegistered=%u\n",present_second);
        tested.UnmapMemory(guest_base,guest_bytes);
        LibKernel::Memory::InstallGpuResources(nullptr);
        Require(name,"owned guest cleanup",LibKernel::Memory::KernelMunmap(guest_base,guest_bytes)==0&&
            LibKernel::Memory::KernelReleaseDirectMemory(direct,guest_bytes)==0,
            "owned guest mapping cleanup failed");
      }
    } // The actual second renderer and all callbacks are gone before assertions.
    prior_scheduler.Finish();prior_scheduler.WaitPriorityOperations(prior_scheduler.CurrentTick()-1u);
  }
  Require(name,"verified completed VMA predecessor reuse",pinned_block&&seeded&&reused,
      "actual live VMA allocation did not reuse the controlled predecessor; setup failure, not a valid RED");
  Require(name,"fresh bitmap excludes predecessor fault",clean_poison,
      "first real fault-buffer use exported a poisoned predecessor bit as a current fault");
  Require(name,"observational/no-exposure path",observation_preserved&&no_exposure_work,
      "observation changed controlled allocation bytes or no-producer processing recorded work");
  Require(name,"independent second mapped-page setup",second_absent,
      "second producer page was already cached, so registration cannot prove its new fault survived");
  Require(name,"later binding preserves second real fault",present_second,
      "repeated binding or processing erased a later batch's legitimate producer bit");
  Require(name,"first-use preserves real new fault",present_current,
      "initialization/parser dropped the legitimate producer bit; clearing at first Process is incorrect");
}
} // namespace
} // namespace Libs::Graphics
