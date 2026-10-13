// Include after ArmedReadbackWindowCostTests.inc. Actual public API, one graphics queue.
namespace Libs::Graphics {
namespace {
void ConfigureArmedReadbackTypedNeighbors() {
  ConfigureArmedReadbackWindowCostTests();
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  _putenv_s("KYTY_DEBUG_BDA_EPOCH","1");_putenv_s("KYTY_DEBUG_AB","");
#else
  setenv("KYTY_DEBUG_BDA_EPOCH","1",1);unsetenv("KYTY_DEBUG_AB");
#endif
}
struct ArmedTypedResult {
  bool setup=false,native_first=false,armed_before=false,shape=false,unarmed_guard=true;
  bool true_fence=true,read_a=false,read_pair=true,guards=false,final_a=false,final_b=false;
  bool cpu_dirty=true,epoch=true,b_pending=true,no_submit=true,b_unarmed=true,gate_ok=true;
  uint64_t before=0u,after=0u,tick_before=0u,tick_after=0u;
  uint32_t a_backing=0u,b_backing=0u,native_a=0u,native_b=0u;
};
void CheckArmedReadbackTypedNeighbors(VulkanHarness& vulkan) {
  constexpr const char* name="ArmedReadbackTypedNeighbors";
  constexpr uint32_t old=0x5a5a5a5au,literal=0x12345678u,b_value=0x76543210u,cpu_value=0x13579bdfu;
  constexpr std::array labels{"mixed-armed-unarmed-pages","same-page-new-sibling",
      "old-armed-is-write","clean-is-write"};
  std::array<ArmedTypedResult,4> results{};
  namespace Exception=Common::HostException;
  Require(name,"real fault route",Exception::InstallHandler([](const Exception::ExceptionInfo& i) {
    if(i.type!=Exception::ExceptionType::AccessViolation||
       (i.access_violation_type!=Exception::AccessViolationType::Read&&
        i.access_violation_type!=Exception::AccessViolationType::Write))return false;
    return LibKernel::Memory::HandleGpuFault(
        i.access_violation_type==Exception::AccessViolationType::Write?PageFaultAccess::Write:PageFaultAccess::Read,
        i.access_violation_vaddr);
  }),"fault bootstrap failed; not a semantic neighbor");
  {
    FetchGpuGuest guest;auto& graphics=vulkan.RuntimeContext();auto& renderer=vulkan.RuntimeRenderer();
    renderer.InitializeGpu(nullptr);LibKernel::Memory::InstallGpuResources(&renderer);
    renderer.MapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    FetchGpuBuffer source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer proof(graphics,12u,vk::BufferUsageFlagBits::eTransferDst);
    source.Flush();proof.Flush();EopComputeProducer compute(vulkan.Device(),source.buffer,b_value);
    std::unique_ptr<CommandProcessor> cp;
    renderer.GetGpu().SendCommandSync([&] {
      cp=std::make_unique<CommandProcessor>(renderer,0x31);cp->Reset();cp->BufferInit();
    });
    for(uint32_t test=0u;test<results.size();++test) {
      auto& out=results[test];const bool writer=test>=2u;
      const uint64_t a=FetchGpuGuest::Base+(test==0u?0x5ffcu:0x5000u);
      const uint64_t b=test==1u?a+64u:FetchGpuGuest::Base+0x6000u;
      ArmedWindowGate gate;std::unique_ptr<std::jthread> release_worker;
      if(test!=3u)release_worker=std::make_unique<std::jthread>([&]{gate.ReleaseAfterReadStarts();});
      for(const auto address:{a-4u,a,b,b+4u})std::memcpy(reinterpret_cast<void*>(address),&old,4u);
      renderer.GetGpu().SendCommandSync([&] {
        auto& cache=renderer.GetBufferCache();auto& scheduler=renderer.GetCommandScheduler();
        out.setup=GuestGpu::IsGpuThread()&&graphics.readback_queue==nullptr&&
            !PerformanceMemoryTestAccess::HasQueue(cache)&&renderer.CurrentBdaEpoch()!=0;
        if(test!=3u)scheduler.DeferPriorityOperation([&]{gate.Hold();});
        cp->WriteAtEndOfPipe32(0u,0u,0x2fu,0u,6u,2u,reinterpret_cast<void*>(a),literal,0u);
        auto [owner,offset]=cache.ObtainBuffer(a,4u,false,true);
        CopyEopNativeLabel(scheduler,*owner,offset,proof,0u,4u);
        const auto done=scheduler.CurrentTick();scheduler.Flush();scheduler.GetMasterSemaphore().Wait(done);
        proof.Invalidate();uint32_t first=0u;std::memcpy(&first,proof.data,4u);out.native_first=first==literal;
        if(test!=3u) {
          std::unique_lock lock(gate.mutex);
          out.gate_ok=gate.cv.wait_for(lock,std::chrono::seconds(5),[&]{return gate.callback_entered;});
          const auto state=PerformanceMemoryTestAccess::Readback(cache,a,4u);
          out.armed_before=state.gpu_dirty&&!state.unarmed&&state.tick==done;
        } else {
          scheduler.WaitPriorityOperations(done);
          out.armed_before=!PerformanceMemoryTestAccess::Readback(cache,a,4u).gpu_dirty;
        }
        ReadbackBootstrapWrite(scheduler,cache,compute,source,b);
        out.shape=owner->Handle()==cache.GetBuffer(cache.FindBuffer(b,4u)).Handle();
        if(test==0u) {
          const auto state=PerformanceMemoryTestAccess::Readback(cache,a,8u);
          out.unarmed_guard=state.gpu_dirty&&state.unarmed;
        } else if(test==1u) {
          const auto state=PerformanceMemoryTestAccess::Readback(cache,a,4u);
          out.unarmed_guard=state.gpu_dirty&&state.unarmed;
        }
        auto& ring=cache.GetUtilityBuffer(MemoryUsage::Download);
        out.before=StreamBufferTestAccess::Offset(ring);out.tick_before=scheduler.CurrentTick();
        const auto epoch_before=renderer.CurrentBdaEpoch();
        if(test!=3u)gate.StartRead();
        cache.ReadMemory(a,test==0u?8u:4u,writer);
        out.after=StreamBufferTestAccess::Offset(ring);out.tick_after=scheduler.CurrentTick();
        out.read_a=LibKernel::Memory::TryReadBacking(a,&out.a_backing,4u)&&out.a_backing==literal;
        if(test==0u) {
          std::array<uint32_t,2> pair{};
          out.read_pair=LibKernel::Memory::TryReadBacking(a,pair.data(),8u)&&
              pair[0]==literal&&pair[1]==b_value;
        }
        if(!writer) {
          out.true_fence=scheduler.IsFree(out.tick_before)&&out.tick_after>out.tick_before;
        } else {
          out.cpu_dirty=PerformanceMemoryTestAccess::CpuDirty(cache,a,4u);
          out.epoch=renderer.CurrentBdaEpoch()==epoch_before+1u;
          const auto state=PerformanceMemoryTestAccess::Readback(cache,b,4u);uint32_t previous=0u;
          out.b_pending=state.gpu_dirty&&LibKernel::Memory::TryReadBacking(b,&previous,4u)&&previous==old;
          out.b_unarmed=state.unarmed;
          out.no_submit=out.tick_after==out.tick_before&&!scheduler.IsFree(out.tick_before);
          *reinterpret_cast<volatile uint32_t*>(a)=cpu_value;
        }
        auto [last,last_offset]=cache.ObtainBuffer(a,4u,false,true);
        CopyEopNativeLabel(scheduler,*last,last_offset,proof,4u,4u);
        auto& b_owner=cache.GetBuffer(cache.FindBuffer(b,4u));
        CopyEopNativeLabel(scheduler,b_owner,b_owner.Offset(b),proof,8u,4u);
        scheduler.Finish();scheduler.WaitPriorityOperations(scheduler.CurrentTick()-1u);
        cache.ReadMemory(a,4u);cache.ReadMemory(b,4u);proof.Invalidate();
        std::memcpy(&out.native_a,static_cast<uint8_t*>(proof.data)+4u,4u);
        std::memcpy(&out.native_b,static_cast<uint8_t*>(proof.data)+8u,4u);
        const uint32_t expected_a=writer?cpu_value:literal;
        out.final_a=LibKernel::Memory::TryReadBacking(a,&out.a_backing,4u)&&
            out.a_backing==expected_a&&out.native_a==expected_a;
        out.final_b=LibKernel::Memory::TryReadBacking(b,&out.b_backing,4u)&&
            out.b_backing==b_value&&out.native_b==b_value;
        uint32_t prefix=0u,suffix=0u;
        out.guards=LibKernel::Memory::TryReadBacking(a-4u,&prefix,4u)&&prefix==old&&
            LibKernel::Memory::TryReadBacking(b+4u,&suffix,4u)&&suffix==old;
        scheduler.Finish();scheduler.DrainPriorityOperations();
      });
      if(release_worker)release_worker->join();
      {std::lock_guard lock(gate.mutex);out.gate_ok=out.gate_ok&&!gate.timeout;}
    }
    renderer.UnmapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    renderer.ShutdownGpu();LibKernel::Memory::InstallGpuResources(nullptr);cp.reset();
  }
  for(uint32_t test=0u;test<results.size();++test) {
    const auto& out=results[test];
    std::printf("[gpu] ArmedTyped %s setup=%u oldArm=%u unarmedGuard=%u ring=%llu->%llu "
        "tick=%llu->%llu fence=%u dirty=%u epoch=%u Bpending=%u Bunarmed=%u A=%08x B=%08x guards=%u\n",
        labels[test],out.setup,out.armed_before,out.unarmed_guard,
        static_cast<unsigned long long>(out.before),static_cast<unsigned long long>(out.after),
        static_cast<unsigned long long>(out.tick_before),static_cast<unsigned long long>(out.tick_after),
        out.true_fence,out.cpu_dirty,out.epoch,out.b_pending,out.b_unarmed,out.a_backing,out.b_backing,out.guards);
    Require(labels[test],"real typed/fence/write-marker/native outputs and cleanup",
        out.setup&&out.native_first&&out.armed_before&&out.shape&&out.unarmed_guard&&out.true_fence&&
        out.read_a&&out.read_pair&&out.guards&&out.final_a&&out.final_b&&out.cpu_dirty&&out.epoch&&
        out.b_pending&&out.no_submit&&out.gate_ok,
        "mixed/new page readback, real fence, is_write CPU/BDA marker or exact native bytes were lost");
  }
  for(uint32_t test=2u;test<results.size();++test)
    Require(labels[test],"typed is_write ready range does not copy unrelated B",
        results[test].before==results[test].after&&results[test].b_unarmed,
        "is_write fast path widened a ready range onto unrelated GPU bytes");
}
} // namespace
} // namespace Libs::Graphics
