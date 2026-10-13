// Include AFTER PrepareBdaUploadBatchCostTests.inc. New internal upload API neighbors AFTER real unchanged cost GREEN.
namespace Libs::Graphics {
namespace {
void CheckBdaUploadOverlapAndConsumerNeighbors(VulkanHarness& vulkan) {
  constexpr const char* name="BdaUploadOverlapAndConsumerNeighbors";
  constexpr uint32_t bytes=0x4000u,words=bytes/4u,guard=0x5a5a5a5au;
  constexpr uint64_t a=FetchGpuGuest::Base+0x4000u,b=FetchGpuGuest::Base+0xc000u;
  constexpr std::array labels{"overlapping-same-owner","disjoint-same-owner-conservative-split","public-consumer-inside-scope"};
  struct Result{bool setup=false,first=true,final=false,backing=false,closed=false;BdaUploadNativeBudget cost{};};
  std::array<Result,3> results{};
  Require(name,"existing real fault route",Common::HostException::InstallHandler([](const Common::HostException::ExceptionInfo& i){
    if(i.type!=Common::HostException::ExceptionType::AccessViolation||
       (i.access_violation_type!=Common::HostException::AccessViolationType::Read&&i.access_violation_type!=Common::HostException::AccessViolationType::Write))return false;
    return LibKernel::Memory::HandleGpuFault(i.access_violation_type==Common::HostException::AccessViolationType::Write?PageFaultAccess::Write:PageFaultAccess::Read,i.access_violation_vaddr);
  }),"fixture bootstrap failed");
  {
    FetchGpuGuest guest;auto& graphics=vulkan.RuntimeContext();auto& renderer=vulkan.RuntimeRenderer();
    renderer.InitializeGpu(nullptr);LibKernel::Memory::InstallGpuResources(&renderer);renderer.MapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    auto& gpu=renderer.GetGpu();auto& cache=renderer.GetBufferCache();auto& scheduler=renderer.GetCommandScheduler();
    std::unique_ptr<CommandProcessor> cp;Buffer* first=nullptr;Buffer* second=nullptr;
    std::vector<uint32_t> source1(words,guard),source2(words,guard),first_expected(words*2u,guard),golden(words*2u,guard);
    for(uint32_t i=0;i<words;++i){if((i%1024u)>=32u&&(i%1024u)<48u){source1[i]=0x12340000u+i;source2[i]=0x56780000u+i;}}
    FetchGpuBuffer x(graphics,bytes,vk::BufferUsageFlagBits::eTransferSrc),y(graphics,bytes,vk::BufferUsageFlagBits::eTransferSrc);
    std::memcpy(x.data,source1.data(),bytes);std::memcpy(y.data,source2.data(),bytes);x.Flush();y.Flush();
    FetchGpuBuffer output(graphics,bytes*2u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer first_proof(graphics,bytes*2u,vk::BufferUsageFlagBits::eTransferDst);output.Flush();first_proof.Flush();
    gpu.SendCommandSync([&]{cp=std::make_unique<CommandProcessor>(renderer,0x31);cp->Reset();cp->BufferInit();
      first=cache.ObtainBuffer(a,bytes,true,true).first;second=cache.ObtainBuffer(b,bytes,true,true).first;
      first->Fill(first->Offset(a),bytes,guard);second->Fill(second->Offset(b),bytes,guard);cache.OnCommandRecorded();
      renderer.AdvanceBdaEpoch();renderer.PrepareBda();scheduler.Finish();scheduler.DrainPriorityOperations();
      cache.ReadMemory(a,bytes);cache.ReadMemory(b,bytes);scheduler.Finish();scheduler.DrainPriorityOperations();});
    BdaUploadNativeConsumer reader(vulkan.Device(),first->Handle(),second->Handle(),output.buffer,words);
    for(uint32_t test=0;test<results.size();++test){auto& r=results[test];
      gpu.SendCommandSync([&]{
        PerformanceMemoryTestAccess::ResetEagerHotPolicy(cache);
        r.setup=GuestGpu::IsGpuThread()&&!CommandRecordingDeferred()&&!DrainStats::Enabled()&&first!=second&&
          first->CpuAddress()==a&&second->CpuAddress()==b&&first->Size()==bytes&&second->Size()==bytes;
        // The test's two real writes reserve the actual cached owner, then record finite native copies.
        (void)cache.ObtainBuffer(a,bytes,true,true);
        r.cost.destinations={static_cast<VkBuffer>(first->Handle()),static_cast<VkBuffer>(second->Handle())};
        std::copy(source1.begin(),source1.end(),first_expected.begin());
        std::copy(source2.begin(),source2.end(),golden.begin());
        const uint32_t half=bytes/2u;
        if(test==1u){std::copy(source1.begin(),source1.begin()+words/2u,golden.begin());}
        auto& command=scheduler.Current();
        {
          BdaUploadNativeBudget::Scope observe(r.cost);command.BeginUploadBatch();
          const vk::BufferCopy one{0u,0u,test==1u?half:bytes};
          Require(name,"first actual finite upload",command.RecordBdaUploadCopy(x.buffer,first->Handle(),std::span{&one,1u}),"first upload not recorded");cache.OnCommandRecorded();
          if(test==2u){
            // Ordinary public Handle must publish the pending transfer writes BEFORE a genuine SSBO read.
            reader.Record(command.Handle());
            const auto native=command.Handle();vk::MemoryBarrier ready{};ready.srcAccessMask=vk::AccessFlagBits::eShaderWrite;
            ready.dstAccessMask=vk::AccessFlagBits::eTransferRead;
            native.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,vk::PipelineStageFlagBits::eTransfer,{},1u,&ready,0u,nullptr,0u,nullptr);
            const vk::BufferCopy save{0u,0u,bytes*2u};native.copyBuffer(output.buffer,first_proof.buffer,1u,&save);
            vk::MemoryBarrier host{};host.srcAccessMask=vk::AccessFlagBits::eTransferWrite;host.dstAccessMask=vk::AccessFlagBits::eHostRead;
            native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eHost,{},1u,&host,0u,nullptr,0u,nullptr);
          }
          const vk::BufferCopy two{test==1u?half:0u,test==1u?half:0u,test==1u?half:bytes};
          Require(name,"second actual finite upload",command.RecordBdaUploadCopy(y.buffer,first->Handle(),std::span{&two,1u}),"second upload not recorded");cache.OnCommandRecorded();
          command.EndUploadBatch();r.closed=!command.UploadBatchActive();
        }
        reader.Record(command.Handle());const auto native=command.Handle();vk::MemoryBarrier host{};
        host.srcAccessMask=vk::AccessFlagBits::eShaderWrite;host.dstAccessMask=vk::AccessFlagBits::eHostRead;
        native.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,vk::PipelineStageFlagBits::eHost,{},1u,&host,0u,nullptr,0u,nullptr);
        scheduler.Finish();scheduler.DrainPriorityOperations();output.Invalidate();r.final=std::memcmp(output.data,golden.data(),bytes*2u)==0;
        if(test==2u){first_proof.Invalidate();r.first=std::memcmp(first_proof.data,first_expected.data(),bytes*2u)==0;}
        cache.ReadMemory(a,bytes);cache.ReadMemory(b,bytes);scheduler.Finish();scheduler.DrainPriorityOperations();
        std::vector<uint32_t> actual(words*2u);
        r.backing=LibKernel::Memory::TryReadBacking(a,actual.data(),bytes)&&LibKernel::Memory::TryReadBacking(b,actual.data()+words,bytes)&&actual==golden;
      });
    }
    renderer.UnmapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);renderer.ShutdownGpu();LibKernel::Memory::InstallGpuResources(nullptr);cp.reset();
  }
  for(uint32_t test=0;test<results.size();++test){const auto& r=results[test];
    std::printf("[BDA-upload-required] %s setup=%u first=%u finalAllWordsGuards=%u backing=%u closed=%u pre=%u post=%u copies=%u bytes=%llu\n",
      labels[test],r.setup,r.first,r.final,r.backing,r.closed,r.cost.pre,r.cost.post,r.cost.calls,static_cast<unsigned long long>(r.cost.bytes));
    Require(labels[test],"actual SSBO and final backing/publication/all-word guards BEFORE dependency count",
      r.setup&&r.first&&r.final&&r.backing&&r.closed&&r.cost.calls==2u&&r.cost.regions==2u&&r.cost.bytes==(test==1u?bytes:bytes*2u),
      "real consumer/overlap/new generation/native bytes/guard/publication failure");
  }
  for(uint32_t test=0;test<results.size();++test){const auto& r=results[test];
    Require(labels[test],"repeated owner/true consumer retains separate complete dependency pairs",
      r.cost.pre==2u&&r.cost.post==2u,"required upload overlap/consumer dependency was silently combined");}
}
} // namespace
} // namespace Libs::Graphics
