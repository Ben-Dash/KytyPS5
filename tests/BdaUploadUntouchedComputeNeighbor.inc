// Include AFTER PrepareBdaUploadBatchCostTests.inc. Functional neighbor only:
// a real finite compute writer ofC is chained before BDA uploads ofA/B.
namespace Libs::Graphics {
namespace {
void CheckBdaUploadUntouchedComputeNeighbor(VulkanHarness& vulkan) {
  constexpr const char* name="BdaUploadUntouchedComputeNeighbor";
  constexpr uint32_t bytes=0x4000u,words=bytes/4u,guard=0x5a5a5a5au,marker=0xa55a7391u;
  constexpr uint64_t a=FetchGpuGuest::Base+0x4000u,b=FetchGpuGuest::Base+0x8000u,c=FetchGpuGuest::Base+0xc000u;
  bool setup=false,pending_before=false,pending_after=false,c_untouched=false;
  bool shader_values=false,all_native=false,published=false;
  BdaUploadNativeBudget uploads;
  Require(name,"existing real mapped fault route",Common::HostException::InstallHandler(
    [](const Common::HostException::ExceptionInfo& i) {
      if(i.type!=Common::HostException::ExceptionType::AccessViolation ||
         (i.access_violation_type!=Common::HostException::AccessViolationType::Read &&
          i.access_violation_type!=Common::HostException::AccessViolationType::Write))return false;
      return LibKernel::Memory::HandleGpuFault(
          i.access_violation_type==Common::HostException::AccessViolationType::Write?
            PageFaultAccess::Write:PageFaultAccess::Read,i.access_violation_vaddr);
    }),"real fault bootstrap unavailable; not native visibility proof");
  {
    FetchGpuGuest guest;auto& graphics=vulkan.RuntimeContext();auto& renderer=vulkan.RuntimeRenderer();
    renderer.InitializeGpu(nullptr);LibKernel::Memory::InstallGpuResources(&renderer);
    renderer.MapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    auto& gpu=renderer.GetGpu();auto& cache=renderer.GetBufferCache();auto& scheduler=renderer.GetCommandScheduler();
    std::unique_ptr<CommandProcessor> cp;std::array<Buffer*,3> owners{};
    std::array<std::vector<uint32_t>,3> golden;
    for(auto& values:golden)values.assign(words,guard);
    FetchGpuBuffer output(graphics,128u*4u+32u,vk::BufferUsageFlagBits::eStorageBuffer);
    std::fill_n(static_cast<uint32_t*>(output.data),output.size/4u,0xdeadbeefu);output.Flush();
    FetchGpuBuffer proof(graphics,bytes*3u,vk::BufferUsageFlagBits::eTransferDst);proof.Flush();
    gpu.SendCommandSync([&] {
      cp=std::make_unique<CommandProcessor>(renderer,0x31);cp->Reset();cp->BufferInit();
      uint32_t index=0u;
      for(const auto address:{a,b,c}) {
        owners[index]=cache.ObtainBuffer(address,bytes,true,true).first;
        owners[index]->Fill(owners[index]->Offset(address),bytes,guard);++index;
      }
      cache.OnCommandRecorded();renderer.AdvanceBdaEpoch();renderer.PrepareBda();
      scheduler.Finish();scheduler.DrainPriorityOperations();
      for(const auto address:{a,b,c})cache.ReadMemory(address,bytes);
      scheduler.Finish();scheduler.DrainPriorityOperations();
    });
    // Exactly the existing native3-buffer reader: A,C -> output,64words each.
    BdaUploadNativeConsumer reader(vulkan.Device(),owners[0]->Handle(),owners[2]->Handle(),output.buffer,64u);
    EopComputeProducer producer(vulkan.Device(),owners[2]->Handle(),marker);
    gpu.SendCommandSync([&] {
      setup=GuestGpu::IsGpuThread()&&!CommandRecordingDeferred()&&!DrainStats::Enabled() &&
          owners[0]!=owners[1]&&owners[0]!=owners[2]&&owners[1]!=owners[2];
      uint32_t index=0u;
      for(const auto address:{a,b,c}) {
        setup=setup&&owners[index]->CpuAddress()==address&&owners[index]->Size()==bytes;
        ++index;
      }
      // Genuine CPU generations forA/B. C remains CPU-clean; its later write
      // is real native shader work, not an injected dirty/readiness verdict.
      cache.InvalidateMemory(a,bytes);cache.InvalidateMemory(b,bytes);
      for(uint32_t i=32u;i<48u;++i){golden[0][i]=0x12340000u+i;golden[1][i]=0x56780000u+i;}
      std::memcpy(reinterpret_cast<void*>(a),golden[0].data(),bytes);
      std::memcpy(reinterpret_cast<void*>(b),golden[1].data(),bytes);
      renderer.AdvanceBdaEpoch();
      (void)cache.ObtainBuffer(c,4u,true,true);
      auto& command=scheduler.Current();
      producer.Record(command.ChainHandle());command.ContinueComputeChain();cache.OnCommandRecorded();
      pending_before=command.ComputeChainPending();
      uploads.destinations={static_cast<VkBuffer>(owners[0]->Handle()),static_cast<VkBuffer>(owners[1]->Handle())};
      {BdaUploadNativeBudget::Scope observe(uploads);renderer.PrepareBda();}
      pending_after=command.ComputeChainPending();
      uint32_t old_cpu=0u;
      c_untouched=cache.IsRegionGpuModified(c,4u)&&!cache.IsRegionCpuModified(c,bytes)&&
          LibKernel::Memory::TryReadBacking(c,&old_cpu,4u)&&old_cpu==guard;
      // NO new transfer/compute pre-barrier: the actual upload dependency chain
      // must make the unrelated previous compute write visible to this read.
      reader.Record(command.Handle());
      vk::MemoryBarrier host{};host.srcAccessMask=vk::AccessFlagBits::eShaderWrite;
      host.dstAccessMask=vk::AccessFlagBits::eHostRead;
      command.Handle().pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
          vk::PipelineStageFlagBits::eHost,{},1u,&host,0u,nullptr,0u,nullptr);
      // These independent snapshots follow the critical shader consumer and
      // cannot provide its missing pre-read visibility dependency retroactively.
      for(uint32_t i=0u;i<3u;++i)CopyEopNativeLabel(scheduler,*owners[i],0u,proof,bytes*i,bytes);
      scheduler.Finish();scheduler.DrainPriorityOperations();output.Invalidate();proof.Invalidate();
      golden[2][0]=marker;
      const auto* actual=static_cast<const uint32_t*>(output.data);
      shader_values=std::equal(golden[0].begin(),golden[0].begin()+64u,actual)&&
          std::equal(golden[2].begin(),golden[2].begin()+64u,actual+64u)&&
          std::all_of(actual+128u,actual+output.size/4u,[](uint32_t v){return v==0xdeadbeefu;});
      all_native=true;
      for(uint32_t i=0u;i<3u;++i)all_native=all_native&&
          std::memcmp(static_cast<const uint8_t*>(proof.data)+i*bytes,golden[i].data(),bytes)==0;
      cache.ReadMemory(c,bytes);scheduler.Finish();scheduler.DrainPriorityOperations();
      std::vector<uint32_t> backing(words);published=LibKernel::Memory::TryReadBacking(c,backing.data(),bytes)&&backing==golden[2];
      renderer.UnmapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    });
    renderer.ShutdownGpu();LibKernel::Memory::InstallGpuResources(nullptr);cp.reset();
  } // Actual shader resources, mapping and direct allocation retire before assertions.
  std::printf("[BDA-untouched-compute] setup=%u realComputePending=%u pendingAfterUploads=%u CcpuOldGpuOwned=%u "
      "ABcopies=%u bytes=%llu shaderACvaluesAndTail=%u all3OwnersAndGuards=%u Cpublication=%u cleanup=PASS\n",
      setup,pending_before,pending_after,c_untouched,uploads.calls,static_cast<unsigned long long>(uploads.bytes),
      shader_values,all_native,published);
  Require(name,"real untouched compute producer, both BDA uploads, shader visibility and lifetime",
      setup&&pending_before&&!pending_after&&c_untouched&&uploads.calls==2u&&uploads.bytes==bytes*2u&&
      shader_values&&all_native&&published,
      "old compute write outside upload destinations lost visibility, or actual native/ownership/cleanup controls failed");
}
} // namespace
} // namespace Libs::Graphics
