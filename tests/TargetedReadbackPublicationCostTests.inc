// Include AFTER ForeignReadbackReadyAfterWaitCostTests.inc for real finite gate helper.
// Distinct from ready-after-wait: requested A is still ARMED/unpublished, not already clean.
namespace Libs::Graphics {
namespace {
struct TargetedReadbackPublicationProbe {
  ForeignReadyAfterWaitGate* b_gate=nullptr;
  BufferCache* cache=nullptr;CommandScheduler* scheduler=nullptr;
  uint64_t a=0,b=0,tick=0;uint32_t value=0,old=0,tick_waits=0,priority_waits=0;
  bool a_pending_at_wait=false,b_pending_at_wait=false,native_complete=false;
  bool b_held_at_return=false,b_pending_at_return=false;
  static inline thread_local TargetedReadbackPublicationProbe* active=nullptr;
  static void Observe(DrainStats::Kind kind) {
    auto* p=active;if(p==nullptr)return;
    if(kind==DrainStats::Kind::TickWait) {
      ++p->tick_waits;
      const auto as=PerformanceMemoryTestAccess::Readback(*p->cache,p->a,4u);
      const auto bs=PerformanceMemoryTestAccess::Readback(*p->cache,p->b,4u);
      uint32_t av=0,bv=0;
      p->a_pending_at_wait=as.gpu_dirty&&!as.unarmed&&as.tick==p->tick&&
          LibKernel::Memory::TryReadBacking(p->a,&av,4u)&&av==p->old;
      p->b_pending_at_wait=bs.gpu_dirty&&!bs.unarmed&&bs.tick==p->tick&&
          LibKernel::Memory::TryReadBacking(p->b,&bv,4u)&&bv==p->old;
      p->native_complete=p->scheduler->GetMasterSemaphore().IsFree(p->tick);
    } else if(kind==DrainStats::Kind::PriorityWait) {
      ++p->priority_waits;
      // RED records real production dependency, then releases finite CPU gate
      // before all native numerical/guard/worker-cleanup controls are evaluated.
      p->b_gate->Release();
    }
  }
};
void CheckTargetedReadbackPublicationCost(VulkanHarness& vulkan) {
  constexpr const char* name="TargetedReadbackPublicationCost";
  constexpr uint64_t a=FetchGpuGuest::Base+0x5000u,b=FetchGpuGuest::Base+0x6000u;
  constexpr uint32_t old=0x5a5a5a5au,a_value=0x12345678u,b_value=0x76543210u;
  ForeignReadyAfterWaitGate b_gate;
  TargetedReadbackPublicationProbe probe;
  probe.b_gate=&b_gate;probe.a=a;probe.b=b;
  probe.value=a_value;probe.old=old;
  bool setup=false,initial_armed=false,old_backing=false,native_initial=false;
  bool reader_value=false,final_native=false,final_backing=false,guards=true,clean=false;
  bool arm_a=false,arm_b=false,same_owner=false,gate_entered=false;
  bool no_reservation=false,no_submit=false;
  uint64_t ring_before=0,ring_after=0,tick_before=0,tick_after=0;
  uint32_t read_a=0,native_a=0,native_b=0,backing_a=0,backing_b=0;
  namespace Exception=Common::HostException;
  Require(name,"existing protected fault route",Exception::InstallHandler([](const Exception::ExceptionInfo& i) {
    if(i.type!=Exception::ExceptionType::AccessViolation||
       (i.access_violation_type!=Exception::AccessViolationType::Read&&
        i.access_violation_type!=Exception::AccessViolationType::Write))return false;
    return LibKernel::Memory::HandleGpuFault(
      i.access_violation_type==Exception::AccessViolationType::Write?PageFaultAccess::Write:PageFaultAccess::Read,
      i.access_violation_vaddr);
  }),"fault-route bootstrap failure is NOT valid RED");
  {
    FetchGpuGuest guest;auto& graphics=vulkan.RuntimeContext();auto& renderer=vulkan.RuntimeRenderer();
    renderer.InitializeGpu(nullptr);LibKernel::Memory::InstallGpuResources(&renderer);
    renderer.MapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    for(const auto address:{a-4u,a,a+4u,b-4u,b,b+4u})std::memcpy(reinterpret_cast<void*>(address),&old,4u);
    FetchGpuBuffer a_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer b_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer proof(graphics,16u,vk::BufferUsageFlagBits::eTransferDst);
    a_source.Flush();b_source.Flush();proof.Flush();
    EopComputeProducer a_compute(vulkan.Device(),a_source.buffer,a_value);
    EopComputeProducer b_compute(vulkan.Device(),b_source.buffer,b_value);
    auto& gpu=renderer.GetGpu();auto& cache=renderer.GetBufferCache();auto& scheduler=renderer.GetCommandScheduler();
    probe.cache=&cache;probe.scheduler=&scheduler;
    std::unique_ptr<CommandProcessor> cp;
    gpu.SendCommandSync([&] {
      cp=std::make_unique<CommandProcessor>(renderer,0x31);cp->Reset();cp->BufferInit();
      setup=GuestGpu::IsGpuThread()&&!PerformanceMemoryTestAccess::HasQueue(cache)&&
          graphics.readback_queue==nullptr&&!ReadbackAgeEnabled()&&!Config::RelaxedReadbackEnabled();
      ReadbackBootstrapWrite(scheduler,cache,a_compute,a_source,a);
      ReadbackBootstrapWrite(scheduler,cache,b_compute,b_source,b);
      auto [ao,ap]=cache.ObtainBuffer(a,4u,false,true);auto [bo,bp]=cache.ObtainBuffer(b,4u,false,true);
      same_owner=ao->Handle()==bo->Handle();
      CopyEopNativeLabel(scheduler,*ao,ap,proof,0u,4u);CopyEopNativeLabel(scheduler,*bo,bp,proof,4u,4u);
      probe.tick=scheduler.CurrentTick();
      // Independent B's withheld callback PRECEDES both real publishers.
      // A is GPU-complete yet still ARMED/old behind unrelated CPU publication.
      scheduler.DeferPriorityOperation([&]{b_gate.Hold();});
      arm_b=PerformanceMemoryTestAccess::ArmDownload(cache,b,4u);
      arm_a=PerformanceMemoryTestAccess::ArmDownload(cache,a,4u);
      // Both requests belong to the SAME submitted tick. Native completion is real.
      scheduler.Flush();
      const auto semaphore=scheduler.GetMasterSemaphore().Handle();
      vk::SemaphoreWaitInfo completion{};completion.semaphoreCount=1u;
      completion.pSemaphores=&semaphore;completion.pValues=&probe.tick;
      RequireVulkanSuccess(vulkan.Device().waitSemaphores(&completion,2'000'000'000ull),
          "finite2s native A+B producers and readback copies");
      scheduler.GetMasterSemaphore().Refresh();gate_entered=b_gate.WaitEntered();
      proof.Invalidate();std::memcpy(&native_a,proof.data,4u);
      std::memcpy(&native_b,static_cast<uint8_t*>(proof.data)+4u,4u);
      native_initial=native_a==a_value&&native_b==b_value;
      const auto as=PerformanceMemoryTestAccess::Readback(cache,a,4u);
      const auto bs=PerformanceMemoryTestAccess::Readback(cache,b,4u);
      initial_armed=as.gpu_dirty&&!as.unarmed&&as.tick==probe.tick&&
          bs.gpu_dirty&&!bs.unarmed&&bs.tick==probe.tick&&scheduler.GetMasterSemaphore().IsFree(probe.tick);
      old_backing=LibKernel::Memory::TryReadBacking(a,&backing_a,4u)&&backing_a==old&&
          LibKernel::Memory::TryReadBacking(b,&backing_b,4u)&&backing_b==old;
      ring_before=StreamBufferTestAccess::Offset(cache.GetUtilityBuffer(MemoryUsage::Download));
      tick_before=scheduler.CurrentTick();
    });
    std::jthread reader([&] {
      const auto previous=DrainStats::g_wait_timer_test_observer;
      TargetedReadbackPublicationProbe::active=&probe;
      DrainStats::g_wait_timer_test_observer=TargetedReadbackPublicationProbe::Observe;
      cache.ReadMemory(a,4u); // Actual foreign public dispatch; snapshot was ARMED/old.
      DrainStats::g_wait_timer_test_observer=previous;TargetedReadbackPublicationProbe::active=nullptr;
      reader_value=LibKernel::Memory::TryReadBacking(a,&read_a,4u)&&read_a==a_value;
      const auto bs=PerformanceMemoryTestAccess::Readback(cache,b,4u);
      uint32_t bv=0;
      probe.b_pending_at_return=bs.gpu_dirty&&!bs.unarmed&&bs.tick==probe.tick&&
          LibKernel::Memory::TryReadBacking(b,&bv,4u)&&bv==old;
      {std::lock_guard lock(b_gate.mutex);
       probe.b_held_at_return=b_gate.entered&&!b_gate.released;}
      // GREEN releases B only AFTER real public readA returns with its current value.
      b_gate.Release();
    });
    reader.join();b_gate.Release();
    gpu.SendCommandSync([&] {
      ring_after=StreamBufferTestAccess::Offset(cache.GetUtilityBuffer(MemoryUsage::Download));
      tick_after=scheduler.CurrentTick();no_reservation=ring_before==ring_after;no_submit=tick_before==tick_after;
      scheduler.WaitPriorityOperations(probe.tick);
      auto [ao,ap]=cache.ObtainBuffer(a,4u,false,true);auto [bo,bp]=cache.ObtainBuffer(b,4u,false,true);
      CopyEopNativeLabel(scheduler,*ao,ap,proof,8u,4u);CopyEopNativeLabel(scheduler,*bo,bp,proof,12u,4u);
      scheduler.Finish();scheduler.WaitPriorityOperations(scheduler.CurrentTick()-1u);proof.Invalidate();
      std::memcpy(&native_a,static_cast<uint8_t*>(proof.data)+8u,4u);
      std::memcpy(&native_b,static_cast<uint8_t*>(proof.data)+12u,4u);
      final_native=native_a==a_value&&native_b==b_value;
      final_backing=LibKernel::Memory::TryReadBacking(a,&backing_a,4u)&&backing_a==a_value&&
          LibKernel::Memory::TryReadBacking(b,&backing_b,4u)&&backing_b==b_value;
      for(const auto address:{a-4u,a+4u,b-4u,b+4u}) {
        uint32_t actual=0;guards=guards&&LibKernel::Memory::TryReadBacking(address,&actual,4u)&&actual==old;
      }
      clean=!PerformanceMemoryTestAccess::Readback(cache,a,4u).gpu_dirty&&
          !PerformanceMemoryTestAccess::Readback(cache,b,4u).gpu_dirty;
      scheduler.Finish();scheduler.DrainPriorityOperations();
    });
    renderer.UnmapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    renderer.ShutdownGpu();LibKernel::Memory::InstallGpuResources(nullptr);cp.reset();
  }
  const bool gates_good=b_gate.Good();
  std::printf("[targeted-publication] setup=%u armsA_B=%u/%u sameOwner=%u armedOld=%u backingOld=%u "
      "nativeA_BBefore=%u Bgate=%u/%u tickWait=%u ApendingAtWait=%u BpendingAtWait=%u "
      "nativeFreeAtWait=%u readA=%08x BheldAtReturn=%u BpendingOldAtReturn=%u priorityWait=%u "
      "noReservation=%u noSubmit=%u nativeA_BFinal=%u backingA_BFinal=%u guards=%u clean=%u "
      "ownedReaderJoined=1 ownedGPUJoined=1 mappingRetired=1 existingReadyAfterWait=1 staleRead=0\n",
      setup,arm_a,arm_b,same_owner,initial_armed,old_backing,native_initial,gate_entered,gates_good,
      probe.tick_waits,probe.a_pending_at_wait,probe.b_pending_at_wait,probe.native_complete,
      read_a,probe.b_held_at_return,probe.b_pending_at_return,probe.priority_waits,
      no_reservation,no_submit,final_native,final_backing,guards,clean);
  std::fflush(stdout);
  Require(name,"native current producers/tokens/old guards/cleanup BEFORE dependency cost oracle",
      setup&&arm_a&&arm_b&&same_owner&&initial_armed&&old_backing&&native_initial&&
      gate_entered&&gates_good&&probe.tick_waits==1u&&probe.a_pending_at_wait&&
      probe.b_pending_at_wait&&probe.native_complete&&reader_value&&no_reservation&&no_submit&&
      final_native&&final_backing&&guards&&clean,
      "setup/native/token/value/gate/cleanup failure is NOT intended unrelated-publication wait RED");
  Require(name,"requested A publication must not depend on disjoint B's withheld CPU callback",
      probe.priority_waits==0u&&probe.b_held_at_return&&probe.b_pending_at_return,
      "GPU-complete requested A remained behind unrelated B's priority callback");
}
} // namespace
} // namespace Libs::Graphics
