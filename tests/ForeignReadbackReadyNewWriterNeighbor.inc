// Separate new-writer negative; original cost RED/GREEN fixture remains unchanged.
// Actual A publication completes, then an independent real new A compute write
// re-dirties/disarms A before the foreign reader finishes its older native wait.
namespace Libs::Graphics {
namespace {
struct ForeignReadyNewWriterGate {
  std::mutex mutex;
  std::condition_variable cv;
  bool entered=false,released=false,timed_out=false;
  void Hold() {
    std::unique_lock lock(mutex);entered=true;cv.notify_all();
    if(!cv.wait_for(lock,std::chrono::seconds(2),[&]{return released;}))timed_out=true;
  }
  void Release() {
    {std::lock_guard lock(mutex);released=true;}cv.notify_all();
  }
  bool WaitEntered() {
    std::unique_lock lock(mutex);
    return cv.wait_for(lock,std::chrono::seconds(2),[&]{return entered;});
  }
  bool Good() {
    std::lock_guard lock(mutex);return entered&&released&&!timed_out;
  }
};
struct ForeignReadyNewWriterProbe {
  ForeignReadyNewWriterGate *a_gate=nullptr,*b_gate=nullptr;
  BufferCache* cache=nullptr;CommandScheduler* scheduler=nullptr;
  uint64_t a=0,b=0,tick=0;
  uint32_t value=0,old=0,tick_waits=0,priority_waits=0;
  bool old_armed_before_wait=false,a_published=false,b_entered=false;
  bool a_native_complete=false,b_still_armed=false,b_backing_old=false;
  Common::UniqueFunction<void> record_new_a;
  bool a_redirtied=false,a_old_after_redirty=false;
  uint64_t new_tick=0;
  static inline thread_local ForeignReadyNewWriterProbe* active=nullptr;
  static void Observe(DrainStats::Kind kind) {
    auto* p=active;if(p==nullptr)return;
    if(kind==DrainStats::Kind::TickWait) {
      ++p->tick_waits;
      const auto before=PerformanceMemoryTestAccess::Readback(*p->cache,p->a,4u);
      uint32_t av=0;
      p->old_armed_before_wait=before.gpu_dirty&&!before.unarmed&&before.tick==p->tick&&
          LibKernel::Memory::TryReadBacking(p->a,&av,4u)&&av==p->old;
      p->a_gate->Release();
      // Wait only for the synthetic CPU publication gate, never for a GPU loop.
      // A's real publisher precedes B's gate on the same actual native tick.
      p->b_entered=p->b_gate->WaitEntered();
      const auto as=PerformanceMemoryTestAccess::Readback(*p->cache,p->a,4u);
      p->a_published=!as.gpu_dirty&&LibKernel::Memory::TryReadBacking(p->a,&av,4u)&&av==p->value;
      p->a_native_complete=p->scheduler->GetMasterSemaphore().IsFree(p->tick);
      const auto bs=PerformanceMemoryTestAccess::Readback(*p->cache,p->b,4u);
      uint32_t bv=0;
      p->b_still_armed=bs.gpu_dirty&&!bs.unarmed&&bs.tick==p->tick;
      p->b_backing_old=LibKernel::Memory::TryReadBacking(p->b,&bv,4u)&&bv==p->old;
      p->record_new_a();
      const auto fresh=PerformanceMemoryTestAccess::Readback(*p->cache,p->a,4u);
      p->a_redirtied=fresh.gpu_dirty&&fresh.unarmed;
      p->a_old_after_redirty=LibKernel::Memory::TryReadBacking(p->a,&av,4u)&&av==p->value;
    } else if(kind==DrainStats::Kind::PriorityWait) {
      ++p->priority_waits;
      // RED can complete normally. This records the unnecessary wait, then releases
      // it before any control/canary/cleanup assertion; not a timeout-based oracle.
      p->b_gate->Release();
    }
  }
};

void CheckForeignReadbackReadyNewWriterNeighbor(VulkanHarness& vulkan) {
  constexpr const char* name="ForeignReadbackReadyNewWriterNeighbor";
  constexpr uint64_t a=FetchGpuGuest::Base+0x5000u,b=FetchGpuGuest::Base+0x6000u;
  constexpr uint32_t old=0x5a5a5a5au,a_value=0x12345678u,b_value=0x76543210u,new_value=0xabcdef09u;
  ForeignReadyNewWriterGate a_gate,b_gate;
  ForeignReadyNewWriterProbe probe;
  probe.a_gate=&a_gate;probe.b_gate=&b_gate;probe.a=a;probe.b=b;
  probe.value=a_value;probe.old=old;
  bool setup=false,initial_armed=false,old_backing=false,native_initial=false;
  bool reader_value=false,final_native=false,final_backing=false,guards=true,clean=false;
  bool arm_a=false,arm_b=false,same_owner=false,gate_entered=false;
  bool no_reservation=false,no_submit=false;
  bool new_pending_after_old_wait=false,new_backing_old_after_old_wait=false,new_protected=false,new_read=false;
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
    FetchGpuBuffer new_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer proof(graphics,16u,vk::BufferUsageFlagBits::eTransferDst);
    a_source.Flush();b_source.Flush();new_source.Flush();proof.Flush();
    EopComputeProducer a_compute(vulkan.Device(),a_source.buffer,a_value);
    EopComputeProducer b_compute(vulkan.Device(),b_source.buffer,b_value);
    EopComputeProducer new_compute(vulkan.Device(),new_source.buffer,new_value);
    auto& gpu=renderer.GetGpu();auto& cache=renderer.GetBufferCache();auto& scheduler=renderer.GetCommandScheduler();
    probe.cache=&cache;probe.scheduler=&scheduler;
    probe.record_new_a=[&] {
      gpu.SendCommandSync([&] {
        ReadbackBootstrapWrite(scheduler,cache,new_compute,new_source,a);
        probe.new_tick=scheduler.CurrentTick();
      });
    };
    std::unique_ptr<CommandProcessor> cp;
    gpu.SendCommandSync([&] {
      cp=std::make_unique<CommandProcessor>(renderer,0x31);cp->Reset();cp->BufferInit();
      setup=GuestGpu::IsGpuThread()&&!PerformanceMemoryTestAccess::HasQueue(cache)&&
          graphics.readback_queue==nullptr&&!ReadbackAgeEnabled();
      ReadbackBootstrapWrite(scheduler,cache,a_compute,a_source,a);
      ReadbackBootstrapWrite(scheduler,cache,b_compute,b_source,b);
      auto [ao,ap]=cache.ObtainBuffer(a,4u,false,true);auto [bo,bp]=cache.ObtainBuffer(b,4u,false,true);
      same_owner=ao->Handle()==bo->Handle();
      CopyEopNativeLabel(scheduler,*ao,ap,proof,0u,4u);CopyEopNativeLabel(scheduler,*bo,bp,proof,4u,4u);
      probe.tick=scheduler.CurrentTick();
      scheduler.DeferPriorityOperation([&]{a_gate.Hold();});
      arm_a=PerformanceMemoryTestAccess::ArmDownload(cache,a,4u);
      scheduler.DeferPriorityOperation([&]{b_gate.Hold();});
      arm_b=PerformanceMemoryTestAccess::ArmDownload(cache,b,4u);
      // Both requests belong to the SAME submitted tick. Native completion is real.
      scheduler.Flush();scheduler.GetMasterSemaphore().Wait(probe.tick);
      gate_entered=a_gate.WaitEntered();
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
      ForeignReadyNewWriterProbe::active=&probe;
      DrainStats::g_wait_timer_test_observer=ForeignReadyNewWriterProbe::Observe;
      cache.ReadMemory(a,4u); // Actual foreign public dispatch; snapshot was ARMED/old.
      DrainStats::g_wait_timer_test_observer=previous;ForeignReadyNewWriterProbe::active=nullptr;
      reader_value=LibKernel::Memory::TryReadBacking(a,&read_a,4u)&&read_a==a_value;
      const auto fresh=PerformanceMemoryTestAccess::Readback(cache,a,4u);
      new_pending_after_old_wait=fresh.gpu_dirty&&fresh.unarmed;
      uint32_t pending=0;
      new_backing_old_after_old_wait=LibKernel::Memory::TryReadBacking(a,&pending,4u)&&pending==a_value;
      new_protected=!LibKernel::Memory::TryReadGpuCleanBacking(a,&pending,4u);
      b_gate.Release();
      // Original API expects the actual protected guest load to retry a newer
      // producer. Restore the observer first: this retry uses normal production.
      const auto latest=*reinterpret_cast<volatile uint32_t*>(a);
      new_read=latest==new_value;
    });
    reader.join();a_gate.Release();b_gate.Release();
    gpu.SendCommandSync([&] {
      ring_after=StreamBufferTestAccess::Offset(cache.GetUtilityBuffer(MemoryUsage::Download));
      tick_after=scheduler.CurrentTick();
      no_reservation=ring_after>ring_before;
      no_submit=tick_after>probe.new_tick&&probe.new_tick==tick_before&&scheduler.GetMasterSemaphore().IsFree(probe.new_tick);
      scheduler.WaitPriorityOperations(probe.tick);
      auto [ao,ap]=cache.ObtainBuffer(a,4u,false,true);auto [bo,bp]=cache.ObtainBuffer(b,4u,false,true);
      CopyEopNativeLabel(scheduler,*ao,ap,proof,8u,4u);CopyEopNativeLabel(scheduler,*bo,bp,proof,12u,4u);
      scheduler.Finish();scheduler.WaitPriorityOperations(scheduler.CurrentTick()-1u);proof.Invalidate();
      std::memcpy(&native_a,static_cast<uint8_t*>(proof.data)+8u,4u);
      std::memcpy(&native_b,static_cast<uint8_t*>(proof.data)+12u,4u);
      final_native=native_a==new_value&&native_b==b_value;
      final_backing=LibKernel::Memory::TryReadBacking(a,&backing_a,4u)&&backing_a==new_value&&
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
  const bool gates_good=a_gate.Good()&&b_gate.Good();
  std::printf("[foreign-ready-new-writer] redirtied=%u oldBacking=%u newPending=%u newOld=%u protected=%u latestLoad=%u newTick=%llu\n",
    probe.a_redirtied,probe.a_old_after_redirty,new_pending_after_old_wait,new_backing_old_after_old_wait,
    new_protected,new_read,static_cast<unsigned long long>(probe.new_tick));
  std::printf("[foreign-ready-after-wait] setup=%u arms=%u/%u sameOwner=%u oldArmed=%u oldBacking=%u nativeBefore=%u gate=%u/%u tickWait=%u oldAtWait=%u Apublished=%u nativeFree=%u Bheld=%u Barmed=%u Bold=%u read=%08x priorityWait=%u ring=%llu->%llu tick=%llu->%llu native=%08x/%08x backing=%08x/%08x guards=%u clean=%u\n",
    setup,arm_a,arm_b,same_owner,initial_armed,old_backing,native_initial,gate_entered,gates_good,
    probe.tick_waits,probe.old_armed_before_wait,probe.a_published,probe.a_native_complete,probe.b_entered,
    probe.b_still_armed,probe.b_backing_old,read_a,probe.priority_waits,
    static_cast<unsigned long long>(ring_before),static_cast<unsigned long long>(ring_after),
    static_cast<unsigned long long>(tick_before),static_cast<unsigned long long>(tick_after),
    native_a,native_b,backing_a,backing_b,guards,clean);
  Require(name,"all real native producer/fence/token/publication/same-tick/old/value/canary/cleanup controls BEFORE cost",
    setup&&arm_a&&arm_b&&same_owner&&initial_armed&&old_backing&&native_initial&&gate_entered&&gates_good&&
    probe.tick_waits==1u&&probe.old_armed_before_wait&&probe.a_published&&probe.a_native_complete&&
    probe.b_entered&&probe.b_still_armed&&probe.b_backing_old&&reader_value&&no_reservation&&no_submit&&
    probe.a_redirtied&&probe.a_old_after_redirty&&new_pending_after_old_wait&&
    new_backing_old_after_old_wait&&new_protected&&new_read&&
    final_native&&final_backing&&guards&&clean,
    "setup/native/fence/token/value/gate/cleanup failure is NOT the intended unnecessary-wait RED");
  Require(name,"new producer after older publication is not a clean current-range completion",
    probe.priority_waits==1u,
    "redirtied A was incorrectly treated as current published backing");
}
} // namespace
} // namespace Libs::Graphics
