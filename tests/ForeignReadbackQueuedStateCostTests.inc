// Include after ArmedReadbackWindowCostTests.inc and WaitRegMemNativeQueuedNeighborTests.inc.
// Existing public ReadMemory on a foreign CPU thread, actual native producers/publication.
// Add only the test-access PendingCommands getter described in the accompanying hook note.
namespace Libs::Graphics {
namespace {
struct ForeignQueuedReadbackResult {
  bool setup=false,initial_unarmed=false,initial_native=false,initial_old=false;
  bool gate_entered=false,captured_callback=false,gate_timeout=false;
  bool published_before_callback=false,b_open_before_callback=false,same_owner=false;
  bool current_a_state=false,read_correct=false,new_writer_wait=true;
  bool b_pending=false,b_unarmed=false,no_submit=false;
  bool final_native=false,final_backing=false,guards=false,clean=false;
  uint64_t ring_before=0,ring_after=0,tick_before=0,tick_after=0;
  uint32_t native_initial=0,read_value=0,native_a=0,native_b=0,backing_a=0,backing_b=0;
};

void CheckForeignReadbackQueuedStateCost(VulkanHarness& vulkan) {
  constexpr const char* name="ForeignReadbackQueuedStateCost";
  constexpr uint64_t a=FetchGpuGuest::Base+0x5000u,b=FetchGpuGuest::Base+0x6000u;
  constexpr uint32_t old=0x5a5a5a5au,first=0x12345678u,rewrite=0xabcdef09u,b_value=0x76543210u;
  std::array<ForeignQueuedReadbackResult,2> results{};
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
    FetchGpuBuffer first_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer rewrite_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer b_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer proof(graphics,16u,vk::BufferUsageFlagBits::eTransferDst);
    first_source.Flush();rewrite_source.Flush();b_source.Flush();proof.Flush();
    EopComputeProducer first_compute(vulkan.Device(),first_source.buffer,first);
    EopComputeProducer rewrite_compute(vulkan.Device(),rewrite_source.buffer,rewrite);
    EopComputeProducer b_compute(vulkan.Device(),b_source.buffer,b_value);
    auto& gpu=renderer.GetGpu();auto& cache=renderer.GetBufferCache();auto& scheduler=renderer.GetCommandScheduler();
    std::unique_ptr<CommandProcessor> cp;
    gpu.SendCommandSync([&]{cp=std::make_unique<CommandProcessor>(renderer,0x31);cp->Reset();cp->BufferInit();});
    for(uint32_t test=0;test<results.size();++test) {
      auto& out=results[test];
      std::memcpy(reinterpret_cast<void*>(a),&old,4u);std::memcpy(reinterpret_cast<void*>(b),&old,4u);
      for(const auto p:{a-4u,a+4u,b-4u,b+4u})std::memcpy(reinterpret_cast<void*>(p),&old,4u);
      gpu.SendCommandSync([&] {
        out.setup=GuestGpu::IsGpuThread()&&!PerformanceMemoryTestAccess::HasQueue(cache)&&
            graphics.readback_queue==nullptr&&!ReadbackAgeEnabled();
        ReadbackBootstrapWrite(scheduler,cache,first_compute,first_source,a);
        auto [owner,offset]=cache.ObtainBuffer(a,4u,false,true);
        CopyEopNativeLabel(scheduler,*owner,offset,proof,0u,4u);
        const auto tick=scheduler.CurrentTick();scheduler.Flush();scheduler.GetMasterSemaphore().Wait(tick);
        scheduler.WaitPriorityOperations(tick);proof.Invalidate();
        std::memcpy(&out.native_initial,proof.data,4u);out.initial_native=out.native_initial==first;
        uint32_t pending=0;out.initial_old=LibKernel::Memory::TryReadBacking(a,&pending,4u)&&pending==old;
        const auto state=PerformanceMemoryTestAccess::Readback(cache,a,4u);
        out.initial_unarmed=state.gpu_dirty&&state.unarmed;
      });
      std::atomic<bool> gate_started{false};
      // Hold the actual GPU dispatcher until its private atomic queue has a command.
      // At this point ONLY the foreign ReadMemory callback can enqueue one; it must
      // already have observed the initial unarmed A before SendCommandSync.
      gpu.SendCommand([&] {
        gate_started.store(true,std::memory_order_release);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(WaitRegMemNativeQueueTestAccess::PendingCommands(gpu)==0u&&
              std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        out.captured_callback=WaitRegMemNativeQueueTestAccess::PendingCommands(gpu)==1u;
        out.gate_timeout=!out.captured_callback;
        // A is genuinely published through the existing readback/fence/priority path.
        // B has no GPU write yet, so the ordinary512KiB window cannot contaminate this preparation.
        cache.ReadMemory(a,4u);uint32_t av=0;
        out.published_before_callback=LibKernel::Memory::TryReadBacking(a,&av,4u)&&av==first&&
            !PerformanceMemoryTestAccess::Readback(cache,a,4u).gpu_dirty;
        ReadbackBootstrapWrite(scheduler,cache,b_compute,b_source,b);
        out.same_owner=cache.GetBuffer(cache.FindBuffer(a,4u)).Handle()==cache.GetBuffer(cache.FindBuffer(b,4u)).Handle();
        uint32_t bv=0;const auto bs=PerformanceMemoryTestAccess::Readback(cache,b,4u);
        out.b_open_before_callback=bs.gpu_dirty&&bs.unarmed&&
            LibKernel::Memory::TryReadBacking(b,&bv,4u)&&bv==old&&!scheduler.IsFree(scheduler.CurrentTick());
        if(test==1u)ReadbackBootstrapWrite(scheduler,cache,rewrite_compute,rewrite_source,a);
        const auto as=PerformanceMemoryTestAccess::Readback(cache,a,4u);
        out.current_a_state=test==0u?!as.gpu_dirty:as.gpu_dirty&&as.unarmed;
        out.ring_before=StreamBufferTestAccess::Offset(cache.GetUtilityBuffer(MemoryUsage::Download));
        out.tick_before=scheduler.CurrentTick();
        // Returning runs the already-queued production ReadMemoryAsync callback next.
      });
      const auto started_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
      while(!gate_started.load(std::memory_order_acquire)&&std::chrono::steady_clock::now()<started_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      out.gate_entered=gate_started.load(std::memory_order_acquire);
      std::jthread reader([&] {
        cache.ReadMemory(a,4u); // Foreign thread: actual public dispatch into ReadMemoryAsync.
        out.read_correct=LibKernel::Memory::TryReadBacking(a,&out.read_value,4u)&&
            out.read_value==(test==0u?first:rewrite);
      });
      reader.join();
      gpu.SendCommandSync([&] {
        out.ring_after=StreamBufferTestAccess::Offset(cache.GetUtilityBuffer(MemoryUsage::Download));
        out.tick_after=scheduler.CurrentTick();
        const auto bs=PerformanceMemoryTestAccess::Readback(cache,b,4u);uint32_t bv=0;
        out.b_pending=bs.gpu_dirty&&LibKernel::Memory::TryReadBacking(b,&bv,4u)&&bv==old;
        out.b_unarmed=bs.unarmed;
        out.no_submit=out.tick_before==out.tick_after&&!scheduler.IsFree(out.tick_before);
        if(test==1u)out.new_writer_wait=out.tick_after>out.tick_before&&scheduler.IsFree(out.tick_before);
        // Normal cleanup is mandatory even on cost RED; native and CPU goldens precede budget assertions.
        auto [ao,ap]=cache.ObtainBuffer(a,4u,false,true);auto [bo,bp]=cache.ObtainBuffer(b,4u,false,true);
        CopyEopNativeLabel(scheduler,*ao,ap,proof,8u,4u);CopyEopNativeLabel(scheduler,*bo,bp,proof,12u,4u);
        scheduler.Finish();scheduler.WaitPriorityOperations(scheduler.CurrentTick()-1u);
        cache.ReadMemory(a,4u);cache.ReadMemory(b,4u);proof.Invalidate();
        std::memcpy(&out.native_a,static_cast<uint8_t*>(proof.data)+8u,4u);
        std::memcpy(&out.native_b,static_cast<uint8_t*>(proof.data)+12u,4u);
        out.final_native=out.native_a==(test==0u?first:rewrite)&&out.native_b==b_value;
        out.final_backing=LibKernel::Memory::TryReadBacking(a,&out.backing_a,4u)&&
            LibKernel::Memory::TryReadBacking(b,&out.backing_b,4u)&&
            out.backing_a==(test==0u?first:rewrite)&&out.backing_b==b_value;
        out.guards=true;
        for(const auto p:{a-4u,a+4u,b-4u,b+4u}) {
          uint32_t actual=0;out.guards=out.guards&&LibKernel::Memory::TryReadBacking(p,&actual,4u)&&actual==old;
        }
        out.clean=!PerformanceMemoryTestAccess::Readback(cache,a,4u).gpu_dirty&&
            !PerformanceMemoryTestAccess::Readback(cache,b,4u).gpu_dirty;
        scheduler.Finish();scheduler.DrainPriorityOperations();
      });
    }
    renderer.UnmapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    renderer.ShutdownGpu();LibKernel::Memory::InstallGpuResources(nullptr);cp.reset();
  }
  for(uint32_t test=0;test<results.size();++test) {
    const auto& r=results[test];
    std::printf("[foreign-readback] case=%u initial=%u/%u/%u queued=%u gateTimeout=%u publishedA=%u Bopen=%u sameOwner=%u state=%u read=%08x ring=%llu->%llu tick=%llu->%llu Bpending=%u Bunarmed=%u newWait=%u native=%08x/%08x backing=%08x/%08x guards=%u clean=%u\n",
      test,r.initial_unarmed,r.initial_native,r.initial_old,r.captured_callback,r.gate_timeout,
      r.published_before_callback,r.b_open_before_callback,r.same_owner,r.current_a_state,r.read_value,
      static_cast<unsigned long long>(r.ring_before),static_cast<unsigned long long>(r.ring_after),
      static_cast<unsigned long long>(r.tick_before),static_cast<unsigned long long>(r.tick_after),
      r.b_pending,r.b_unarmed,r.new_writer_wait,r.native_a,r.native_b,r.backing_a,r.backing_b,r.guards,r.clean);
    Require(name,"all real queue/producer/publication/new-writer/value/canary/cleanup controls BEFORE cost",
      r.setup&&r.initial_unarmed&&r.initial_native&&r.initial_old&&r.gate_entered&&r.captured_callback&&
      !r.gate_timeout&&r.published_before_callback&&r.b_open_before_callback&&r.same_owner&&
      r.current_a_state&&r.read_correct&&r.new_writer_wait&&r.final_native&&r.final_backing&&r.guards&&r.clean,
      "bootstrap/ownership/fence/value control failure; NOT the intended cost RED");
  }
  const auto& r=results[0];
  Require(name,"queued stale unarmed A must not readback already published A or dirty independent B",
    r.ring_before==r.ring_after&&r.b_pending&&r.b_unarmed&&r.no_submit,
    "foreign callback reused stale captured state and widened onto unrelated B/extra ring storage/submission");
}
} // namespace
} // namespace Libs::Graphics
