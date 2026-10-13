// Include AFTER PrepareBdaUploadBatchCostTests.inc. Deferred recorder ownership/numeric neighbors only.
// Observer counts in this file are RECORD-TIME hook calls, not actual submit-thread driver cost.
namespace Libs::Graphics {
namespace {
void ConfigurePrepareBdaUploadBatchAsyncNeighbors() {
  ConfigurePrepareBdaUploadBatchCostTests();
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  _putenv_s("KYTY_RECORD_THREAD","1");
#else
  setenv("KYTY_RECORD_THREAD","1",1);
#endif
}
void CheckPrepareBdaUploadBatchAsyncNeighbors(VulkanHarness& vulkan) {
  constexpr const char* name="PrepareBdaUploadBatchAsyncNeighbors";constexpr uint32_t owner_bytes=0x4000u,words=owner_bytes/4u;
  constexpr uint64_t a=FetchGpuGuest::Base+0x4000u,b=FetchGpuGuest::Base+0xc000u;
  constexpr uint32_t guard=0x5a5a5a5au;
  struct Result{bool setup=false,initial=false,dirty=false,clean=false,native=false,repeated=false,rewrite=false;
    BdaUploadNativeBudget budget{};uint64_t before=0,after=0;};
  std::array<Result,3> results{};constexpr std::array labels{"normal-ring","actual-ring-wrap","temporary-upload"};
  Require(name,"existing guest-fault route",Common::HostException::InstallHandler([](const Common::HostException::ExceptionInfo& i){
    if(i.type!=Common::HostException::ExceptionType::AccessViolation||
       (i.access_violation_type!=Common::HostException::AccessViolationType::Read&&i.access_violation_type!=Common::HostException::AccessViolationType::Write))return false;
    return LibKernel::Memory::HandleGpuFault(i.access_violation_type==Common::HostException::AccessViolationType::Write?PageFaultAccess::Write:PageFaultAccess::Read,i.access_violation_vaddr);
  }),"bootstrap failed; NOT RED");
  {
    FetchGpuGuest guest;auto& graphics=vulkan.RuntimeContext();auto& renderer=vulkan.RuntimeRenderer();
    renderer.InitializeGpu(nullptr);LibKernel::Memory::InstallGpuResources(&renderer);renderer.MapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    auto& gpu=renderer.GetGpu();auto& cache=renderer.GetBufferCache();auto& scheduler=renderer.GetCommandScheduler();
    std::unique_ptr<CommandProcessor> cp;Buffer* first=nullptr;Buffer* second=nullptr;
    std::vector<uint32_t> initial(words,guard),expected(words*2u,guard);
    FetchGpuBuffer output(graphics,owner_bytes*2u,vk::BufferUsageFlagBits::eStorageBuffer);output.Flush();
    gpu.SendCommandSync([&]{cp=std::make_unique<CommandProcessor>(renderer,0x31);cp->Reset();cp->BufferInit();
      std::memcpy(reinterpret_cast<void*>(a),initial.data(),owner_bytes);std::memcpy(reinterpret_cast<void*>(b),initial.data(),owner_bytes);
      first=cache.ObtainBuffer(a,owner_bytes,true,true).first;second=cache.ObtainBuffer(b,owner_bytes,true,true).first;
      // Public written bindings force genuine cache owners; actual finite GPU fills justify ownership.
      first->Fill(first->Offset(a),owner_bytes,guard);second->Fill(second->Offset(b),owner_bytes,guard);cache.OnCommandRecorded();
      renderer.AdvanceBdaEpoch();renderer.PrepareBda();scheduler.Finish();scheduler.DrainPriorityOperations();
      cache.ReadMemory(a,owner_bytes);cache.ReadMemory(b,owner_bytes);scheduler.Finish();scheduler.DrainPriorityOperations();});
    BdaUploadNativeConsumer reader(vulkan.Device(),first->Handle(),second->Handle(),output.buffer,words);
    const auto consume=[&](const std::vector<uint32_t>& golden){
      reader.Record(scheduler.Current().Handle()); // NO extra transfer->compute pre-barrier: actual upload visibility must suffice.
      const auto native=scheduler.Current().Handle();vk::MemoryBarrier host{};host.srcAccessMask=vk::AccessFlagBits::eShaderWrite;host.dstAccessMask=vk::AccessFlagBits::eHostRead;
      native.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,vk::PipelineStageFlagBits::eHost,{},1u,&host,0u,nullptr,0u,nullptr);
      scheduler.Finish();scheduler.DrainPriorityOperations();output.Invalidate();
      return std::memcmp(output.data,golden.data(),golden.size()*4u)==0;
    };
    for(uint32_t test=0;test<results.size();++test){auto& r=results[test];
      gpu.SendCommandSync([&]{
        scheduler.Finish();scheduler.DrainPriorityOperations();
        // Existing friend only replaces an idle host staging ring, preserving actual Map/Commit/Wait behavior.
        const uint64_t ring=test==0u?4u*1024u*1024u:test==1u?24u*1024u:8u*1024u;
        PerformanceMemoryTestAccess::RecreateUploadRing(cache,ring);
        r.setup=GuestGpu::IsGpuThread()&&CommandRecordingDeferred()&&!DrainStats::Enabled()&&
          first!=second&&first->CpuAddress()==a&&second->CpuAddress()==b&&first->Size()==owner_bytes&&second->Size()==owner_bytes;
        r.initial=consume(expected);
        // Genuine CPU writes, real invalidation and new BDA epoch: no direct tracker-state fabrication.
        cache.InvalidateMemory(a,owner_bytes);cache.InvalidateMemory(b,owner_bytes);
        for(uint32_t i=32u;i<48u;++i){expected[i]=0x12340000u+test*0x100u+i;expected[words+i]=0x56780000u+test*0x100u+i;}
        std::memcpy(reinterpret_cast<void*>(a),expected.data(),owner_bytes);std::memcpy(reinterpret_cast<void*>(b),expected.data()+words,owner_bytes);
        r.dirty=cache.IsRegionCpuModified(a,owner_bytes)&&cache.IsRegionCpuModified(b,owner_bytes);
        renderer.AdvanceBdaEpoch();r.budget.destinations={static_cast<VkBuffer>(first->Handle()),static_cast<VkBuffer>(second->Handle())};
        r.before=scheduler.CurrentTick();{BdaUploadNativeBudget::Scope observe(r.budget);renderer.PrepareBda();}r.after=scheduler.CurrentTick();
        r.clean=!cache.IsRegionCpuModified(a,owner_bytes)&&!cache.IsRegionCpuModified(b,owner_bytes);r.native=consume(expected);
        BdaUploadNativeBudget repeat;repeat.destinations=r.budget.destinations;
        renderer.AdvanceBdaEpoch();{BdaUploadNativeBudget::Scope observe(repeat);renderer.PrepareBda();}
        r.repeated=repeat.calls==0u&&repeat.bytes==0u&&repeat.pre==0u&&repeat.post==0u&&consume(expected);
        // Real second CPU generation: old prepared bytes cannot be reused.
        cache.InvalidateMemory(a,owner_bytes);expected[40u]^=0x00abcdefu;
        std::memcpy(reinterpret_cast<void*>(a),expected.data(),owner_bytes);renderer.AdvanceBdaEpoch();
        BdaUploadNativeBudget rewrite;rewrite.destinations=r.budget.destinations;
        {BdaUploadNativeBudget::Scope observe(rewrite);renderer.PrepareBda();}
        r.rewrite=rewrite.calls==1u&&rewrite.bytes==owner_bytes&&rewrite.pre==1u&&rewrite.post==1u&&consume(expected);
      });
    }
    renderer.UnmapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);renderer.ShutdownGpu();LibKernel::Memory::InstallGpuResources(nullptr);cp.reset();
  }
  for(uint32_t test=0;test<results.size();++test){const auto& r=results[test];
    std::printf("[BDA-upload-async] %s setup=%u initial=%u dirty=%u clean=%u nativeAllWordsGuards=%u repeated=%u rewrite=%u pre=%u post=%u copies=%u regions=%u bytes=%llu tick=%llu->%llu\n",
      labels[test],r.setup,r.initial,r.dirty,r.clean,r.native,r.repeated,r.rewrite,r.budget.pre,r.budget.post,r.budget.calls,r.budget.regions,
      static_cast<unsigned long long>(r.budget.bytes),static_cast<unsigned long long>(r.before),static_cast<unsigned long long>(r.after));
    Require(labels[test],"ALL real native SSBO words/guards, CPU generations, ring/temporary cleanup BEFORE cost",
      r.setup&&r.initial&&r.dirty&&r.clean&&r.native&&r.repeated&&r.rewrite&&r.budget.calls==2u&&r.budget.regions==2u&&r.budget.bytes==owner_bytes*2u&&
      r.after==r.before+(test==1u?1u:0u),"producer/native bytes/guards/generation/real ring-wrap lifetime failed; NOT valid cost RED");
  }
}
} // namespace
} // namespace Libs::Graphics
