// Include AFTER TargetedReadbackPublicationCostTests.inc; native older/wider overlap safety.
namespace Libs::Graphics {
namespace {
struct OlderReadbackPublicationProbe {
  ForeignReadyAfterWaitGate* b_gate=nullptr;BufferCache* cache=nullptr;CommandScheduler* scheduler=nullptr;
  uint64_t a=0,b=0,c=0,tick=0,old_tick=0;uint32_t value=0,old=0,tick_waits=0,priority_waits=0;
  bool a_pending_at_wait=false,b_pending_at_wait=false,c_pending_at_wait=false,native_complete=false;
  bool b_held_at_return=false,b_pending_at_return=false;
  static inline thread_local OlderReadbackPublicationProbe* active=nullptr;
  static void Observe(DrainStats::Kind kind) {
    auto* p=active;if(p==nullptr)return;
    if(kind==DrainStats::Kind::TickWait) {
      ++p->tick_waits;
      const auto a=PerformanceMemoryTestAccess::Readback(*p->cache,p->a,4u);
      const auto b=PerformanceMemoryTestAccess::Readback(*p->cache,p->b,4u);
      const auto c=PerformanceMemoryTestAccess::Readback(*p->cache,p->c,4u);
      uint32_t av=0,bv=0,cv=0;
      p->a_pending_at_wait=a.gpu_dirty&&!a.unarmed&&a.tick==p->tick&&
          LibKernel::Memory::TryReadBacking(p->a,&av,4u)&&av==p->old;
      p->b_pending_at_wait=b.gpu_dirty&&!b.unarmed&&b.tick==p->old_tick&&
          LibKernel::Memory::TryReadBacking(p->b,&bv,4u)&&bv==p->old;
      p->c_pending_at_wait=c.gpu_dirty&&!c.unarmed&&c.tick==p->old_tick&&
          LibKernel::Memory::TryReadBacking(p->c,&cv,4u)&&cv==p->old;
      p->native_complete=p->scheduler->GetMasterSemaphore().IsFree(p->tick);
    } else if(kind==DrainStats::Kind::PriorityWait) {
      ++p->priority_waits;p->b_gate->Release();
    }
  }
};
void CheckTargetedReadbackOlderPublisherNeighbor(VulkanHarness& vulkan) {
  constexpr const char* name="TargetedReadbackOlderPublisherNeighbor";
  constexpr uint64_t a=FetchGpuGuest::Base+0x5000u,b=FetchGpuGuest::Base+0x8000u,c=FetchGpuGuest::Base+0x6000u;
  constexpr uint32_t old=0x5a5a5a5au,a_value=0x12345678u,b_value=0x76543210u,new_value=0xabcdef09u;
  ForeignReadyAfterWaitGate b_gate;
  OlderReadbackPublicationProbe probe;
  probe.b_gate=&b_gate;probe.a=a;probe.b=b;
  probe.value=new_value;probe.old=old;probe.c=c;
  bool setup=false,initial_armed=false,old_backing=false,native_initial=false;
  bool reader_value=false,final_native=false,final_backing=false,guards=true,clean=false;
  bool arm_a=false,arm_b=false,same_owner=false,gate_entered=false;
  bool no_reservation=false,no_submit=false,wider_old=false,new_unarmed=false,new_armed=false;
  bool native_new=false,c_initial=false,c_final=false,c_backing_final=false;
  uint64_t token_a1=0,token_a2=0;uint32_t native_c=0,backing_c=0;
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
    for(const auto address:{a-4u,a,a+4u,b-4u,b,b+4u,c-4u,c,c+4u})std::memcpy(reinterpret_cast<void*>(address),&old,4u);
    FetchGpuBuffer a_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer b_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer c_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    c_source.Flush();
    FetchGpuBuffer proof(graphics,32u,vk::BufferUsageFlagBits::eTransferDst);
    a_source.Flush();b_source.Flush();proof.Flush();
    EopComputeProducer a_compute(vulkan.Device(),a_source.buffer,a_value);
    EopComputeProducer b_compute(vulkan.Device(),b_source.buffer,b_value);
    EopComputeProducer c_compute(vulkan.Device(),c_source.buffer,a_value);
    EopComputeProducer new_compute(vulkan.Device(),a_source.buffer,new_value);
    auto& gpu=renderer.GetGpu();auto& cache=renderer.GetBufferCache();auto& scheduler=renderer.GetCommandScheduler();
    probe.cache=&cache;probe.scheduler=&scheduler;
    std::unique_ptr<CommandProcessor> cp;
    gpu.SendCommandSync([&] {
      cp=std::make_unique<CommandProcessor>(renderer,0x31);cp->Reset();cp->BufferInit();
      setup=GuestGpu::IsGpuThread()&&!PerformanceMemoryTestAccess::HasQueue(cache)&&
          graphics.readback_queue==nullptr&&!ReadbackAgeEnabled()&&!Config::RelaxedReadbackEnabled()&&
          cache.GetUtilityBuffer(MemoryUsage::Download).IsCoherent();
      // Prime the registered owner for the wider A+C request. A CPU-clean
      // ObtainBuffer may instead return ActiveStream; B remains independent.
      (void)cache.FindBuffer(a-4u,c-a+12u);
      cache.SynchronizeBuffersInRange(a-4u,c-a+12u);
      ReadbackBootstrapWrite(scheduler,cache,a_compute,a_source,a);
      ReadbackBootstrapWrite(scheduler,cache,c_compute,c_source,c);
      ReadbackBootstrapWrite(scheduler,cache,b_compute,b_source,b);
      auto [ao,ap]=cache.ObtainBuffer(a,4u,false,true);auto [bo,bp]=cache.ObtainBuffer(b,4u,false,true);
      auto [co,cp_offset]=cache.ObtainBuffer(c,4u,false,true);
      same_owner=ao->Handle()==co->Handle();
      CopyEopNativeLabel(scheduler,*ao,ap,proof,0u,4u);CopyEopNativeLabel(scheduler,*bo,bp,proof,4u,4u);
      CopyEopNativeLabel(scheduler,*co,cp_offset,proof,8u,4u);
      probe.tick=scheduler.CurrentTick();probe.old_tick=probe.tick;
      // Independent B's withheld callback PRECEDES both real publishers.
      // A is GPU-complete yet still ARMED/old behind unrelated CPU publication.
      scheduler.DeferPriorityOperation([&]{b_gate.Hold();});
      arm_b=PerformanceMemoryTestAccess::ArmDownload(cache,b,4u);
      arm_a=PerformanceMemoryTestAccess::ArmDownload(cache,a,c-a+4u);
      token_a1=PerformanceMemoryTestAccess::ReadbackToken(cache);
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
          bs.gpu_dirty&&!bs.unarmed&&bs.tick==probe.old_tick&&scheduler.GetMasterSemaphore().IsFree(probe.tick);
      old_backing=LibKernel::Memory::TryReadBacking(a,&backing_a,4u)&&backing_a==old&&
          LibKernel::Memory::TryReadBacking(b,&backing_b,4u)&&backing_b==old;
      const auto cs=PerformanceMemoryTestAccess::Readback(cache,c,4u);
      std::memcpy(&native_c,static_cast<uint8_t*>(proof.data)+8u,4u);
      c_initial=native_c==a_value&&cs.gpu_dirty&&!cs.unarmed&&cs.tick==probe.old_tick;
      wider_old=LibKernel::Memory::TryReadBacking(c,&backing_c,4u)&&backing_c==old;
      // Native A2 is newer, with a distinct readback token/tick. A1 publisher
      // remains withheld; C keeps A1's wider request armed and accessible to it.
      ReadbackBootstrapWrite(scheduler,cache,new_compute,a_source,a);
      const auto dirty_new=PerformanceMemoryTestAccess::Readback(cache,a,4u);
      new_unarmed=dirty_new.gpu_dirty&&dirty_new.unarmed;
      const bool arm_new=PerformanceMemoryTestAccess::ArmDownload(cache,a,4u);
      token_a2=PerformanceMemoryTestAccess::ReadbackToken(cache);probe.tick=scheduler.CurrentTick();
      const auto fresh=PerformanceMemoryTestAccess::Readback(cache,a,4u);
      new_armed=arm_new&&fresh.gpu_dirty&&!fresh.unarmed&&fresh.tick==probe.tick&&
          probe.tick>probe.old_tick&&token_a2!=token_a1;
      auto [latest,latest_offset]=cache.ObtainBuffer(a,4u,false,true);
      CopyEopNativeLabel(scheduler,*latest,latest_offset,proof,12u,4u);
      scheduler.Flush();completion.pValues=&probe.tick;
      RequireVulkanSuccess(vulkan.Device().waitSemaphores(&completion,2'000'000'000ull),
          "finite2s newer native A2 and exact page download");
      scheduler.GetMasterSemaphore().Refresh();proof.Invalidate();
      uint32_t new_native=0;std::memcpy(&new_native,static_cast<uint8_t*>(proof.data)+12u,4u);
      native_new=new_native==new_value&&scheduler.GetMasterSemaphore().IsFree(probe.tick)&&
          LibKernel::Memory::TryReadBacking(a,&backing_a,4u)&&backing_a==old;
      ring_before=StreamBufferTestAccess::Offset(cache.GetUtilityBuffer(MemoryUsage::Download));
      tick_before=scheduler.CurrentTick();
    });
    std::jthread reader([&] {
      const auto previous=DrainStats::g_wait_timer_test_observer;
      OlderReadbackPublicationProbe::active=&probe;
      DrainStats::g_wait_timer_test_observer=OlderReadbackPublicationProbe::Observe;
      cache.ReadMemory(a,4u); // Actual foreign public dispatch; snapshot was ARMED/old.
      DrainStats::g_wait_timer_test_observer=previous;OlderReadbackPublicationProbe::active=nullptr;
      reader_value=LibKernel::Memory::TryReadBacking(a,&read_a,4u)&&read_a==new_value;
      const auto bs=PerformanceMemoryTestAccess::Readback(cache,b,4u);
      uint32_t bv=0;
      probe.b_pending_at_return=bs.gpu_dirty&&!bs.unarmed&&bs.tick==probe.old_tick&&
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
      CopyEopNativeLabel(scheduler,*ao,ap,proof,16u,4u);CopyEopNativeLabel(scheduler,*bo,bp,proof,20u,4u);
      auto [co,cp_offset]=cache.ObtainBuffer(c,4u,false,true);
      CopyEopNativeLabel(scheduler,*co,cp_offset,proof,24u,4u);
      scheduler.Finish();scheduler.WaitPriorityOperations(scheduler.CurrentTick()-1u);proof.Invalidate();
      std::memcpy(&native_a,static_cast<uint8_t*>(proof.data)+16u,4u);
      std::memcpy(&native_b,static_cast<uint8_t*>(proof.data)+20u,4u);
      std::memcpy(&native_c,static_cast<uint8_t*>(proof.data)+24u,4u);
      c_final=native_c==a_value;
      c_backing_final=LibKernel::Memory::TryReadBacking(c,&backing_c,4u)&&backing_c==a_value;
      final_native=native_a==new_value&&native_b==b_value;
      final_backing=LibKernel::Memory::TryReadBacking(a,&backing_a,4u)&&backing_a==new_value&&
          LibKernel::Memory::TryReadBacking(b,&backing_b,4u)&&backing_b==b_value;
      for(const auto address:{a-4u,a+4u,b-4u,b+4u,c-4u,c+4u}) {
        uint32_t actual=0;guards=guards&&LibKernel::Memory::TryReadBacking(address,&actual,4u)&&actual==old;
      }
      clean=!PerformanceMemoryTestAccess::Readback(cache,a,4u).gpu_dirty&&
          !PerformanceMemoryTestAccess::Readback(cache,b,4u).gpu_dirty&&
          !PerformanceMemoryTestAccess::Readback(cache,c,4u).gpu_dirty;
      scheduler.Finish();scheduler.DrainPriorityOperations();
    });
    renderer.UnmapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    renderer.ShutdownGpu();LibKernel::Memory::InstallGpuResources(nullptr);cp.reset();
  }
  const bool gates_good=b_gate.Good();
  std::printf("[targeted-older-neighbor] setup=%u arms=%u/%u sameOwner=%u oldArmed=%u oldBacking=%u "
      "nativeA1B=%u oldWiderC=%u/%u newUnarmed=%u newArmed=%u nativeA2=%u tokens=%llu->%llu "
      "tickWait=%u A2armedOldAtWait=%u B1armedOldAtWait=%u C1armedOldAtWait=%u nativeFree=%u "
      "priorityWait=%u BheldAtReadReturn=%u readA=%08x expectedA2=%08x "
      "lateNativeA2=%08x lateBackingA2=%08x finalC=%u/%u Bfinal=%u guards=%u clean=%u "
      "noReservation=%u noSubmit=%u gate=%u/%u ownedReaderJoined=1 ownedGPUJoined=1 mappingRetired=1\n",
      setup,arm_a,arm_b,same_owner,initial_armed,old_backing,native_initial,c_initial,wider_old,
      new_unarmed,new_armed,native_new,static_cast<unsigned long long>(token_a1),
      static_cast<unsigned long long>(token_a2),probe.tick_waits,probe.a_pending_at_wait,
      probe.b_pending_at_wait,probe.c_pending_at_wait,probe.native_complete,probe.priority_waits,
      probe.b_held_at_return,read_a,new_value,native_a,backing_a,c_final,c_backing_final,
      final_backing,guards,clean,no_reservation,no_submit,gate_entered,gates_good);
  std::fflush(stdout);
  Require(name,"actual old-wider/newer producer/token/current numeric/canary/cleanup controls",
      setup&&arm_a&&arm_b&&same_owner&&initial_armed&&old_backing&&native_initial&&
      c_initial&&wider_old&&new_unarmed&&new_armed&&native_new&&gate_entered&&gates_good&&
      probe.tick_waits==1u&&probe.a_pending_at_wait&&probe.b_pending_at_wait&&
      probe.c_pending_at_wait&&probe.native_complete&&reader_value&&no_reservation&&no_submit&&
      final_native&&final_backing&&c_final&&c_backing_final&&guards&&clean,
      "older wider publisher corrupted newer current A2 or native/setup/guard/cleanup failed");
  Require(name,"newer A2 must not overtake older overlapping wider A1 publisher",
      probe.priority_waits==1u&&!probe.b_held_at_return,
      "exact newer A2 bypassed older A1 backing ordering; conservative dependency was required");
}
} // namespace
} // namespace Libs::Graphics
