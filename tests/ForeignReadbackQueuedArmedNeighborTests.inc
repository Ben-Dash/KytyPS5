// Include after ForeignReadbackQueuedStateCostTests.inc. Existing APIs/test-access only.
namespace Libs::Graphics {
namespace {
struct ForeignArmedPublicationGate {
  std::mutex mutex;std::condition_variable cv;
  bool entered=false,released=false,timed_out=false;
  uint64_t address=0,tick=0;BufferCache* cache=nullptr;
  uint32_t tick_waits=0;bool old_armed_at_reader_wait=false;
  static inline thread_local ForeignArmedPublicationGate* active=nullptr;
  void Hold() {
    std::unique_lock lock(mutex);entered=true;cv.notify_all();
    if(!cv.wait_for(lock,std::chrono::seconds(2),[&]{return released;}))timed_out=true;
  }
  static void Observe(DrainStats::Kind kind) {
    if(active==nullptr||kind!=DrainStats::Kind::TickWait)return;
    auto& g=*active;++g.tick_waits;
    const auto s=PerformanceMemoryTestAccess::Readback(*g.cache,g.address,4u);
    uint32_t backing=0;
    g.old_armed_at_reader_wait=s.gpu_dirty&&!s.unarmed&&s.tick==g.tick&&
        LibKernel::Memory::TryReadBacking(g.address,&backing,4u)&&backing==0x5a5a5a5au;
    {std::lock_guard lock(g.mutex);g.released=true;}g.cv.notify_all();
  }
};

void CheckForeignReadbackQueuedArmedNeighbor(VulkanHarness& vulkan) {
  constexpr const char* name="ForeignReadbackQueuedArmedNeighbor";
  constexpr uint64_t a=FetchGpuGuest::Base+0x5000u,b=FetchGpuGuest::Base+0x6000u;
  constexpr uint32_t old=0x5a5a5a5au,value=0x12345678u,b_value=0x76543210u;
  bool setup=false,initial_unarmed=false,native_initial=false,initial_old=false;
  bool queued=false,dispatcher_entered=false,armed_before_callback=false,before_b=false,same_owner=false;
  bool reader_value=false,final_native=false,final_backing=false,guards=true,clean=false;
  bool b_pending=false,b_unarmed=false,no_submit=false,publisher_entered=false,publisher_ok=false;
  uint64_t ring_before=0,ring_after=0,tick_before=0,tick_after=0;
  uint32_t actual_a=0,native_a=0,native_b=0,backing_a=0,backing_b=0;
  ForeignArmedPublicationGate publish;
  namespace Exception=Common::HostException;
  Require(name,"existing real fault route",Exception::InstallHandler([](const Exception::ExceptionInfo& i) {
    if(i.type!=Exception::ExceptionType::AccessViolation||
       (i.access_violation_type!=Exception::AccessViolationType::Read&&
        i.access_violation_type!=Exception::AccessViolationType::Write))return false;
    return LibKernel::Memory::HandleGpuFault(
      i.access_violation_type==Exception::AccessViolationType::Write?PageFaultAccess::Write:PageFaultAccess::Read,
      i.access_violation_vaddr);
  }),"fault route bootstrap failed; NOT valid RED");
  {
    FetchGpuGuest guest;auto& graphics=vulkan.RuntimeContext();auto& renderer=vulkan.RuntimeRenderer();
    renderer.InitializeGpu(nullptr);LibKernel::Memory::InstallGpuResources(&renderer);
    renderer.MapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    auto& gpu=renderer.GetGpu();auto& cache=renderer.GetBufferCache();auto& scheduler=renderer.GetCommandScheduler();
    publish.cache=&cache;publish.address=a;
    for(const auto p:{a-4u,a,a+4u,b-4u,b,b+4u})std::memcpy(reinterpret_cast<void*>(p),&old,4u);
    FetchGpuBuffer a_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer b_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer proof(graphics,16u,vk::BufferUsageFlagBits::eTransferDst);
    a_source.Flush();b_source.Flush();proof.Flush();
    EopComputeProducer a_compute(vulkan.Device(),a_source.buffer,value);
    EopComputeProducer b_compute(vulkan.Device(),b_source.buffer,b_value);
    std::unique_ptr<CommandProcessor> cp;
    gpu.SendCommandSync([&] {
      cp=std::make_unique<CommandProcessor>(renderer,0x31);cp->Reset();cp->BufferInit();
      setup=GuestGpu::IsGpuThread()&&!PerformanceMemoryTestAccess::HasQueue(cache)&&
          graphics.readback_queue==nullptr&&!ReadbackAgeEnabled();
      ReadbackBootstrapWrite(scheduler,cache,a_compute,a_source,a);
      auto [owner,offset]=cache.ObtainBuffer(a,4u,false,true);
      CopyEopNativeLabel(scheduler,*owner,offset,proof,0u,4u);
      const auto tick=scheduler.CurrentTick();scheduler.Flush();scheduler.GetMasterSemaphore().Wait(tick);
      scheduler.WaitPriorityOperations(tick);proof.Invalidate();
      uint32_t native=0,backing=0;std::memcpy(&native,proof.data,4u);native_initial=native==value;
      initial_old=LibKernel::Memory::TryReadBacking(a,&backing,4u)&&backing==old;
      const auto s=PerformanceMemoryTestAccess::Readback(cache,a,4u);initial_unarmed=s.gpu_dirty&&s.unarmed;
    });
    std::atomic<bool> started{false};
    gpu.SendCommand([&] {
      started.store(true,std::memory_order_release);
      const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
      while(WaitRegMemNativeQueueTestAccess::PendingCommands(gpu)==0u&&std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      queued=WaitRegMemNativeQueueTestAccess::PendingCommands(gpu)==1u;
      // The foreign production callback is now queued with its captured UNARMED A.
      // Arm A through the public poll path, but hold its priority publication before
      // FinalizeReadback. This callback precedes the download's callback on the SAME tick.
      publish.tick=scheduler.CurrentTick();scheduler.DeferPriorityOperation([&]{publish.Hold();});
      const bool initially_not_ready=!cache.PollReadMemory(a,4u);
      scheduler.GetMasterSemaphore().Wait(publish.tick);
      {
        std::unique_lock lock(publish.mutex);
        publisher_entered=publish.cv.wait_for(lock,std::chrono::seconds(2),[&]{return publish.entered;});
      }
      const auto as=PerformanceMemoryTestAccess::Readback(cache,a,4u);uint32_t av=0;
      armed_before_callback=initially_not_ready&&as.gpu_dirty&&!as.unarmed&&as.tick==publish.tick&&
          scheduler.GetMasterSemaphore().IsFree(publish.tick)&&
          LibKernel::Memory::TryReadBacking(a,&av,4u)&&av==old;
      // B did not exist during A download planning; it is independent real work on the new open tick.
      ReadbackBootstrapWrite(scheduler,cache,b_compute,b_source,b);
      same_owner=cache.GetBuffer(cache.FindBuffer(a,4u)).Handle()==cache.GetBuffer(cache.FindBuffer(b,4u)).Handle();
      const auto bs=PerformanceMemoryTestAccess::Readback(cache,b,4u);uint32_t bv=0;
      before_b=bs.gpu_dirty&&bs.unarmed&&LibKernel::Memory::TryReadBacking(b,&bv,4u)&&bv==old;
      ring_before=StreamBufferTestAccess::Offset(cache.GetUtilityBuffer(MemoryUsage::Download));
      tick_before=scheduler.CurrentTick();
      // Returning invokes the real foreign ReadMemoryAsync callback while A is still ARMED.
    });
    const auto start_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(!started.load(std::memory_order_acquire)&&std::chrono::steady_clock::now()<start_deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    dispatcher_entered=started.load(std::memory_order_acquire);
    std::jthread reader([&] {
      const auto previous=DrainStats::g_wait_timer_test_observer;
      ForeignArmedPublicationGate::active=&publish;
      DrainStats::g_wait_timer_test_observer=ForeignArmedPublicationGate::Observe;
      cache.ReadMemory(a,4u);
      DrainStats::g_wait_timer_test_observer=previous;ForeignArmedPublicationGate::active=nullptr;
      reader_value=LibKernel::Memory::TryReadBacking(a,&actual_a,4u)&&actual_a==value;
    });
    reader.join();
    gpu.SendCommandSync([&] {
      ring_after=StreamBufferTestAccess::Offset(cache.GetUtilityBuffer(MemoryUsage::Download));
      tick_after=scheduler.CurrentTick();const auto bs=PerformanceMemoryTestAccess::Readback(cache,b,4u);
      uint32_t bv=0;b_pending=bs.gpu_dirty&&LibKernel::Memory::TryReadBacking(b,&bv,4u)&&bv==old;b_unarmed=bs.unarmed;
      no_submit=tick_before==tick_after&&!scheduler.IsFree(tick_before);
      auto [ao,ap]=cache.ObtainBuffer(a,4u,false,true);auto [bo,bp]=cache.ObtainBuffer(b,4u,false,true);
      CopyEopNativeLabel(scheduler,*ao,ap,proof,8u,4u);CopyEopNativeLabel(scheduler,*bo,bp,proof,12u,4u);
      scheduler.Finish();scheduler.WaitPriorityOperations(scheduler.CurrentTick()-1u);
      cache.ReadMemory(a,4u);cache.ReadMemory(b,4u);proof.Invalidate();
      std::memcpy(&native_a,static_cast<uint8_t*>(proof.data)+8u,4u);
      std::memcpy(&native_b,static_cast<uint8_t*>(proof.data)+12u,4u);
      final_native=native_a==value&&native_b==b_value;
      final_backing=LibKernel::Memory::TryReadBacking(a,&backing_a,4u)&&backing_a==value&&
          LibKernel::Memory::TryReadBacking(b,&backing_b,4u)&&backing_b==b_value;
      for(const auto p:{a-4u,a+4u,b-4u,b+4u}) {
        uint32_t actual=0;guards=guards&&LibKernel::Memory::TryReadBacking(p,&actual,4u)&&actual==old;
      }
      clean=!PerformanceMemoryTestAccess::Readback(cache,a,4u).gpu_dirty&&
          !PerformanceMemoryTestAccess::Readback(cache,b,4u).gpu_dirty;
      scheduler.Finish();scheduler.DrainPriorityOperations();
    });
    {std::lock_guard lock(publish.mutex);publisher_ok=publish.entered&&publish.released&&!publish.timed_out;}
    renderer.UnmapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    renderer.ShutdownGpu();LibKernel::Memory::InstallGpuResources(nullptr);cp.reset();
  }
  std::printf("[foreign-armed] initial=%u/%u/%u queued=%u armed=%u publisher=%u/%u wait=%u oldAtWait=%u Bbefore=%u sameOwner=%u read=%08x ring=%llu->%llu tick=%llu->%llu Bpending=%u Bunarmed=%u native=%08x/%08x backing=%08x/%08x guards=%u clean=%u\n",
    initial_unarmed,native_initial,initial_old,queued,armed_before_callback,publisher_entered,publisher_ok,
    publish.tick_waits,publish.old_armed_at_reader_wait,before_b,same_owner,actual_a,
    static_cast<unsigned long long>(ring_before),static_cast<unsigned long long>(ring_after),
    static_cast<unsigned long long>(tick_before),static_cast<unsigned long long>(tick_after),b_pending,b_unarmed,
    native_a,native_b,backing_a,backing_b,guards,clean);
  Require(name,"real initial query/fully-armed callback/native fence/old bytes/priority publication/value controls BEFORE cost",
    setup&&initial_unarmed&&native_initial&&initial_old&&dispatcher_entered&&queued&&armed_before_callback&&
    publisher_entered&&publisher_ok&&publish.tick_waits==1u&&publish.old_armed_at_reader_wait&&before_b&&
    same_owner&&reader_value&&final_native&&final_backing&&guards&&clean,
    "arming/fence/publication/queue/value/canary/cleanup control failure; NOT a ring cost failure");
  Require(name,"fully armed callback reuses only actual A publication tick",
    ring_before==ring_after&&b_pending&&b_unarmed&&no_submit,
    "fully armed A unnecessarily downloaded/submitted independent open B");
}
} // namespace
} // namespace Libs::Graphics
