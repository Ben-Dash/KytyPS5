// Additional test-only coverage. Include after LiteralEopPublicationCostTests.inc.
// Reuses StreamBufferTestAccess, native EOP producer/copy helpers and real Thread_Gpu.
namespace Libs::Graphics {
namespace {
void CheckLiteralEopPublicationSpanNeighbors(VulkanHarness& vulkan) {
  constexpr const char* name = "LiteralEopPublicationSpanNeighbors";
  namespace Exception = Common::HostException;
  Require(name, "real fault route", Exception::InstallHandler([](const Exception::ExceptionInfo& i) {
    if (i.type != Exception::ExceptionType::AccessViolation ||
        (i.access_violation_type != Exception::AccessViolationType::Read &&
         i.access_violation_type != Exception::AccessViolationType::Write)) return false;
    return LibKernel::Memory::HandleGpuFault(
        i.access_violation_type == Exception::AccessViolationType::Write ?
            PageFaultAccess::Write : PageFaultAccess::Read, i.access_violation_vaddr);
  }), "same production guest fault route must be installed before protections");
  struct Result {
    bool setup=false,pending=false,clean_refused=false,complete=false,backing_read=false,clean=false;
    uint64_t before_ring=0u,after_ring=0u;
    std::vector<uint8_t> native,backing,expected;
  };
  std::array<Result,3> results{};
  GraphicsInitJmpTables();
  {
    FetchGpuGuest guest;
    auto& renderer=vulkan.RuntimeRenderer();
    renderer.InitializeGpu(nullptr);
    LibKernel::Memory::InstallGpuResources(&renderer);
    renderer.MapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    for(uint32_t test=0u;test<results.size();++test) {
      const bool mixed=test==2u;
      const uint64_t address=FetchGpuGuest::Base+
          (test==0u?0x100u:test==1u?0x3ffcu:0x8104u);
      const uint64_t width=test==0u?64u:8u;
      const uint64_t observed_address=mixed?address-4u:address;
      const uint64_t observed_width=mixed?16u:width;
      const uint32_t gpu_marker=0xcdef4400u+test;
      std::vector<uint32_t> words(width/4u);
      for(uint32_t i=0u;i<words.size();++i)words[i]=0x11110000u+test*0x100u+i;
      auto& result=results[test];
      result.expected.resize(observed_width);
      if(mixed) {
        std::memcpy(result.expected.data(),&gpu_marker,4u);
        std::memcpy(result.expected.data()+4u,words.data(),width);
        std::memcpy(result.expected.data()+12u,&gpu_marker,4u);
      } else std::memcpy(result.expected.data(),words.data(),width);
      std::memset(reinterpret_cast<void*>(observed_address),0x5a,observed_width);
      FetchGpuBuffer producer(vulkan.RuntimeContext(),4u,
          vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
      FetchGpuBuffer readback(vulkan.RuntimeContext(),observed_width,vk::BufferUsageFlagBits::eTransferDst);
      producer.Flush();readback.Flush();
      EopComputeProducer compute(vulkan.Device(),producer.buffer,gpu_marker);
      renderer.GetGpu().SendCommandSync([&] {
        auto& scheduler=renderer.GetCommandScheduler();auto& cache=renderer.GetBufferCache();
        CommandProcessor cp(renderer,0x31);cp.Reset();cp.BufferInit();
        (void)cache.FindBuffer(observed_address,observed_width);
        cache.SynchronizeBuffersInRange(observed_address,observed_width);
        result.setup=GuestGpu::IsGpuThread()&&!cache.HasGpuDirtyBytes(observed_address,observed_width);
        const auto command=scheduler.Current().Handle();
        vk::MemoryBarrier ready{};ready.srcAccessMask=vk::AccessFlagBits::eHostWrite;
        ready.dstAccessMask=vk::AccessFlagBits::eShaderWrite;
        command.pipelineBarrier(vk::PipelineStageFlagBits::eHost,vk::PipelineStageFlagBits::eComputeShader,
            {},1u,&ready,0u,nullptr,0u,nullptr);
        compute.Record(command);
        if(mixed) {
          // Mark ONE contiguous unknown dirty interval spanning prefix+literal+suffix.
          (void)cache.ObtainBuffer(observed_address,observed_width,true,true);
          for(uint32_t i=0u;i<4u;++i)
            LiteralCostCopyUnknown(scheduler,cache,producer,observed_address+i*4u);
        }
        const auto tick=scheduler.CurrentTick();
        auto& ring=cache.GetUtilityBuffer(MemoryUsage::Download);
        result.before_ring=StreamBufferTestAccess::Offset(ring);
        cache.WriteOrderedMemory(address,
            {reinterpret_cast<const uint8_t*>(words.data()),static_cast<size_t>(width)});
        result.after_ring=StreamBufferTestAccess::Offset(ring);
        // Caller storage is mutable and dies separately; publication must own its bytes.
        std::fill(words.begin(),words.end(),0xbad0bad0u);
        auto [owner,offset]=cache.ObtainBuffer(observed_address,observed_width,false,true);
        CopyEopNativeLabel(scheduler,*owner,offset,readback,0u,observed_width);
        std::vector<uint8_t> old(observed_width),clean_probe(width);
        result.pending=LibKernel::Memory::TryReadBacking(observed_address,old.data(),observed_width)&&
            std::all_of(old.begin(),old.end(),[](uint8_t value){return value==0x5a;})&&
            scheduler.CurrentTick()==tick&&!scheduler.IsFree(tick)&&cache.HasGpuDirtyBytes(address,width);
        result.clean_refused=!LibKernel::Memory::TryReadGpuCleanBacking(address,clean_probe.data(),width);
        cache.ReadMemory(observed_address,observed_width);
        scheduler.Finish();scheduler.WaitPriorityOperations(scheduler.CurrentTick()-1u);
        producer.Invalidate();readback.Invalidate();
        uint32_t producer_value=0u;std::memcpy(&producer_value,producer.data,4u);
        result.complete=scheduler.IsFree(tick)&&producer_value==gpu_marker;
        result.native.resize(observed_width);result.backing.resize(observed_width);
        std::memcpy(result.native.data(),readback.data,observed_width);
        result.backing_read=LibKernel::Memory::TryReadBacking(
            observed_address,result.backing.data(),observed_width);
        result.clean=!cache.HasGpuDirtyBytes(observed_address,observed_width);
      });
    }
    renderer.GetGpu().SendCommandSync([&]{
      auto& scheduler=renderer.GetCommandScheduler();scheduler.Finish();scheduler.DrainPriorityOperations();
    });
    renderer.UnmapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    renderer.ShutdownGpu();LibKernel::Memory::InstallGpuResources(nullptr);
  }
  for(uint32_t test=0u;test<results.size();++test) {
    const auto& r=results[test];
    std::printf("[gpu] LiteralSpanNeighbor%u setup=%u pending=%u complete=%u ring=%llu->%llu native=%u backing=%u\n",
        test,r.setup,r.pending,r.complete,static_cast<unsigned long long>(r.before_ring),
        static_cast<unsigned long long>(r.after_ring),r.native==r.expected,r.backing==r.expected);
    Require(name,"real pending/completed producer and byte ownership",
        r.setup&&r.pending&&r.clean_refused&&r.complete&&r.clean,
        "fixture did not establish native producer/coherent lifetime; not a semantic result");
    Require(name,"wide/cross-page/split-prefix-suffix and caller payload lifetime",
        r.native==r.expected&&r.backing_read&&r.backing==r.expected,
        "known payload or actual unknown prefix/suffix bytes changed or were packed at wrong offsets");
  }
  for(uint32_t test=0u;test<results.size();++test) {
    const auto& r=results[test];
    Require(name,"only unknown prefix/suffix consume download staging",
        test==2u?r.after_ring>r.before_ring:r.after_ring==r.before_ring,
        "pure known wide/cross-page data used a download, or unknown neighbors were skipped");
  }
}
} // namespace
} // namespace Libs::Graphics
