// Include AFTER ArmedReadbackWindowCostTests.inc in BindlessLifecycleTests.cpp.
// Actual GuestGpu graphics/compute queues, protected guest memory and finite native producer.
// No timed GPU loop and no production behavior change. Cost assertions run after cleanup.
namespace Libs::Graphics {
namespace {
struct GpuPm4FetchGate {
  std::mutex mutex;
  std::condition_variable cv;
  bool requested=false;
  std::atomic<bool> timed_out{false},signal_failed{false},signalled{false};
  void Release() {
    {std::lock_guard lock(mutex);requested=true;}
    cv.notify_one();
  }
};
struct GpuPm4FetchWaitProbe {
  GpuPm4FetchGate* gate=nullptr;
  uint32_t full=0,tick=0,priority=0;
  static inline thread_local GpuPm4FetchWaitProbe* active=nullptr;
  static void Observe(DrainStats::Kind kind) {
    auto* p=active;if(p==nullptr)return;
    if(kind==DrainStats::Kind::FullDrain)++p->full;
    else if(kind==DrainStats::Kind::TickWait)++p->tick;
    else if(kind==DrainStats::Kind::PriorityWait)++p->priority;
    else return;
    // OLD implementation completes normally: release at the real dispatcher wait.
    // A timeout never serves as the expected RED or as proof of independent progress.
    p->gate->Release();
  }
};
struct GpuPm4FetchResult {
  bool header=false,setup=false,old_backing=false,real_gpu_dirty=false;
  bool idle=false,progress_before_release=false,blocked_before_release=false;
  bool marker_old_before_release=false,native=false,backing=false,markers=false,guards=false;
  bool native_producer=false,clean=false,observer_restored=false,helper_joined=false;
  bool signal_failed=false,gate_timeout=false,gate_signalled=false;
  uint32_t a=0,b=0,tail=0,native_word=0,full=0,tick=0,priority=0;
};
void ConfigureGpuPm4FetchYieldTests() {
  ConfigureArmedReadbackWindowCostTests();
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  _putenv_s("KYTY_RECORD_THREAD","0");_putenv_s("KYTY_DEBUG_MRT_SHADER","");
#else
  setenv("KYTY_RECORD_THREAD","0",1);unsetenv("KYTY_DEBUG_MRT_SHADER");
#endif
}
void CheckGpuPm4FetchResourcePlan(VulkanHarness& vulkan) {
  constexpr const char* name="GpuPm4FetchResourcePlan";
  std::array<bool,2> streamed{},returned_bounds{},guest_offset_rejected{},registered{},owner_bounds{};
  std::array<uint64_t,2> stream_offsets{},bad_offsets{},owner_offsets{},owner_sizes{};
  auto& renderer=vulkan.RuntimeRenderer();renderer.InitializeGpu(nullptr);
  {
    FetchGpuGuest guest;auto& gpu=renderer.GetGpu();auto& scheduler=renderer.GetCommandScheduler();
    auto& cache=renderer.GetBufferCache();
    LibKernel::Memory::InstallGpuResources(&renderer);renderer.MapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    for(uint32_t test=0;test<2u;++test) {
      const uint64_t packet=FetchGpuGuest::Base+0x1000u+test*0x4000u;
      std::array<uint32_t,12> cpu{};cpu.fill(0x5a5a5a5au);
      std::memcpy(reinterpret_cast<void*>(packet-4u),cpu.data(),sizeof(cpu));
      gpu.SendCommandSync([&] {
        CommandProcessor cp(renderer,0x31);cp.Reset();cp.BufferInit();
        // Actual CPU-only acquisition: it is allowed to stream even is_texel_buffer=true.
        auto [stream,offset]=cache.ObtainBuffer(packet,40u,false,true);
        stream_offsets[test]=offset;bad_offsets[test]=stream->Offset(packet-4u);
        streamed[test]=stream->CpuAddress()==0u;
        returned_bounds[test]=offset<=stream->Size()&&40u<=stream->Size()-offset;
        guest_offset_rejected[test]=!stream->IsInBounds(packet-4u,48u);
        const auto id=cache.FindBuffer(packet-4u,48u);
        cache.SynchronizeBuffersInRange(packet-4u,48u);
        auto& owner=cache.GetBuffer(id);
        registered[test]=owner.Usage()==MemoryUsage::DeviceLocal&&owner.CpuAddress()!=0u&&
            cache.IsRegionRegistered(packet-4u,48u);
        owner_offsets[test]=owner.Offset(packet-4u);owner_sizes[test]=owner.Size();
        owner_bounds[test]=owner.IsInBounds(packet-4u,48u)&&owner_offsets[test]<=owner.Size()&&
            48u<=owner.Size()-owner_offsets[test];
        // Only bounded existing CPU uploads/normal teardown. No shader/gate/PM4 execution.
        scheduler.Finish();scheduler.DrainPriorityOperations();
      });
    }
    renderer.UnmapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    LibKernel::Memory::InstallGpuResources(nullptr);
  }
  renderer.ShutdownGpu();
  for(uint32_t test=0;test<2u;++test) {
    std::printf("[pm4-fetch-plan] case=%u realStream=%u returnedOffset=%llu returnedBounds=%u "
        "badGuestDerivedOffset=%llx rejected=%u registered=%u ownerOffset=%llu ownerSize=%llu "
        "ownerBounds=%u gatedShader=0 guestPm4Execution=0 cleanup=PASS\n",test,streamed[test],
        static_cast<unsigned long long>(stream_offsets[test]),returned_bounds[test],
        static_cast<unsigned long long>(bad_offsets[test]),guest_offset_rejected[test],registered[test],
        static_cast<unsigned long long>(owner_offsets[test]),static_cast<unsigned long long>(owner_sizes[test]),owner_bounds[test]);
  }
  std::fflush(stdout);
  for(uint32_t test=0;test<2u;++test) Require(name,"actual stream route rejected and registered native proof bounds valid",
      streamed[test]&&returned_bounds[test]&&guest_offset_rejected[test]&&registered[test]&&owner_bounds[test],
      "actual acquisition/bounds/cleanup controls failed; no gated shader or failed copy is allowed");
}

void CheckGpuPm4FetchYield(VulkanHarness& vulkan) {
  constexpr const char* name="GpuPm4FetchYield";
  constexpr uint32_t guard=0x5a5a5a5au,a_value=0x31415926u,b_value=0x27182818u,tail_value=0x1234abcd;
  Require(name,"actual host controller/direct recorder",!GuestGpu::IsGpuThread()&&!CommandRecordingDeferred(),
      "invoke host-side after Vulkan initialization; wrong recorder is NOT intended RED");
  Require(name,"existing protected-memory fault route",Common::HostException::InstallHandler(
    [](const Common::HostException::ExceptionInfo& i) {
      if(i.type!=Common::HostException::ExceptionType::AccessViolation||
         (i.access_violation_type!=Common::HostException::AccessViolationType::Read&&
          i.access_violation_type!=Common::HostException::AccessViolationType::Write))return false;
      return LibKernel::Memory::HandleGpuFault(
        i.access_violation_type==Common::HostException::AccessViolationType::Write ?
          PageFaultAccess::Write:PageFaultAccess::Read,i.access_violation_vaddr);
    }),"protected-memory bootstrap failure is NOT intended RED");
  std::array<GpuPm4FetchResult,2> results{};
  GraphicsInitJmpTables();
  auto& renderer=vulkan.RuntimeRenderer();renderer.InitializeGpu(nullptr);
  {
    FetchGpuGuest guest;
    auto& gpu=renderer.GetGpu();
    auto& scheduler=renderer.GetCommandScheduler();auto& cache=renderer.GetBufferCache();
    LibKernel::Memory::InstallGpuResources(&renderer);renderer.MapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    for(uint32_t test=0u;test<results.size();++test) {
      auto& r=results[test];r.header=test==0u;
      const uint64_t packet=FetchGpuGuest::Base+0x1000u+test*0x4000u;
      const uint64_t done_a=FetchGpuGuest::Base+0xa000u+test*0x100u;
      const uint64_t done_b=done_a+4u,done_tail=done_a+8u;
      const std::array<uint32_t,10> golden{
        KYTY_PM4(5,Pm4::IT_WRITE_DATA,0),0u,static_cast<uint32_t>(done_a),
        static_cast<uint32_t>(done_a>>32u),a_value,
        KYTY_PM4(5,Pm4::IT_WRITE_DATA,0),0u,static_cast<uint32_t>(done_tail),
        static_cast<uint32_t>(done_tail>>32u),tail_value};
      auto initial=golden;
      const uint32_t changed=r.header?0u:4u;
      initial[changed]=r.header?KYTY_PM4(5,Pm4::IT_NOP,0):0u;
      const uint64_t write_address=packet+changed*4u;
      std::memcpy(reinterpret_cast<void*>(packet-4u),&guard,4u);
      std::memcpy(reinterpret_cast<void*>(packet),initial.data(),sizeof(initial));
      std::memcpy(reinterpret_cast<void*>(packet+sizeof(initial)),&guard,4u);
      for(const auto address:{done_a,done_b,done_tail}) {
        const uint32_t zero=0;std::memcpy(reinterpret_cast<void*>(address),&zero,4u);
      }
      FetchGpuBuffer source(vulkan.RuntimeContext(),4u,
        vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
      FetchGpuBuffer proof(vulkan.RuntimeContext(),sizeof(golden)+8u,vk::BufferUsageFlagBits::eTransferDst);
      source.Flush();proof.Flush();
      EopComputeProducer compute(vulkan.Device(),source.buffer,golden[changed]);
      vk::SemaphoreTypeCreateInfo type{};type.semaphoreType=vk::SemaphoreType::eTimeline;
      vk::SemaphoreCreateInfo create{};create.pNext=&type;vk::Semaphore native_gate=nullptr;
      RequireVulkanSuccess(vulkan.Device().createSemaphore(&create,nullptr,&native_gate),"bounded PM4 producer gate");
      GpuPm4FetchGate gate;GpuPm4FetchWaitProbe probe;probe.gate=&gate;
      const auto release_device=vulkan.Device();
      // Only this helper signals the native timeline. Even unexpected setup cannot retain the GPU.
      std::jthread releaser([&] {
        {
          std::unique_lock lock(gate.mutex);
          if(!gate.cv.wait_for(lock,std::chrono::seconds(3),[&]{return gate.requested;}))
            gate.timed_out.store(true,std::memory_order_release);
        }
        vk::SemaphoreSignalInfo signal{};signal.semaphore=native_gate;signal.value=1u;
        gate.signal_failed.store(release_device.signalSemaphore(&signal)!=vk::Result::eSuccess,std::memory_order_release);
        gate.signalled.store(true,std::memory_order_release);
      });
      void (*previous_wait)(DrainStats::Kind)=nullptr;
      GpuPm4FetchWaitProbe* previous_probe=nullptr;
      gpu.SendCommandSync([&] {
        HW::Context context{};HW::UserConfig ucfg{};HW::Shader shader{};
        scheduler.Begin(context,ucfg,shader);
        const uint64_t proof_bytes=sizeof(golden)+8u;
        (void)cache.FindBuffer(packet-4u,proof_bytes);
        cache.SynchronizeBuffersInRange(packet-4u,proof_bytes);
        auto& before_write=cache.GetBuffer(cache.FindBuffer(write_address,4u));
        Require(name,"bounded registered producer source/destination BEFORE native copy",
            source.size>=4u&&before_write.Usage()==MemoryUsage::DeviceLocal&&
            before_write.IsInBounds(write_address,4u),
            "producer acquisition/copy bounds failure; never submit the invalid span");
        ReadbackBootstrapWrite(scheduler,cache,compute,source,write_address);
        // Reacquire actual registered ownership after write preparation can create/merge buffers.
        auto& owner=cache.GetBuffer(cache.FindBuffer(packet-4u,proof_bytes));
        const auto proof_offset=owner.Offset(packet-4u);
        Require(name,"actual registered proof source/destination capacity BEFORE native copy",
            owner.Usage()==MemoryUsage::DeviceLocal&&owner.CpuAddress()!=0u&&
            owner.IsInBounds(packet-4u,proof_bytes)&&proof_offset<=owner.Size()&&
            proof_bytes<=owner.Size()-proof_offset&&proof_bytes<=proof.size,
            "proof acquisition/copy bounds failure; never submit the invalid span");
        CopyEopNativeLabel(scheduler,owner,proof_offset,proof,0u,proof_bytes);
        uint32_t old=UINT32_MAX;
        r.old_backing=LibKernel::Memory::TryReadBacking(write_address,&old,4u)&&old==initial[changed];
        r.real_gpu_dirty=cache.HasGpuDirtyBytes(write_address,4u);
        r.setup=GuestGpu::IsGpuThread()&&!PerformanceMemoryTestAccess::HasQueue(cache)&&
          !ReadbackAgeEnabled()&&renderer.IsMapped(packet,sizeof(golden))&&r.old_backing&&r.real_gpu_dirty;
        SubmitInfo submit{};submit.AddWait(native_gate,1u);scheduler.Flush(submit);
        previous_wait=DrainStats::g_wait_timer_test_observer;previous_probe=GpuPm4FetchWaitProbe::active;
        GpuPm4FetchWaitProbe::active=&probe;
        DrainStats::g_wait_timer_test_observer=GpuPm4FetchWaitProbe::Observe;
      });
      const std::array<uint32_t,5> other{
        KYTY_PM4(5,Pm4::IT_WRITE_DATA,0),0u,static_cast<uint32_t>(done_b),
        static_cast<uint32_t>(done_b>>32u),b_value};
      // Borrowed spans remain alive until observed queue idle/native Finish.
      gpu.Submit(std::span<const uint32_t>(reinterpret_cast<const uint32_t*>(packet),golden.size()),{});
      gpu.SubmitCompute(0x20u,other);
      const auto before_release_end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
      while(!gate.signalled.load(std::memory_order_acquire)&&std::chrono::steady_clock::now()<before_release_end) {
        uint32_t b=0,a=UINT32_MAX,old=UINT32_MAX;
        if(WaitRegMemNativeQueueTestAccess::GraphicsBlocked(gpu)&&
           LibKernel::Memory::TryReadBacking(done_b,&b,4u)&&b==b_value&&
           LibKernel::Memory::TryReadBacking(done_a,&a,4u)&&a==0u&&
           LibKernel::Memory::TryReadBacking(write_address,&old,4u)&&old==initial[changed]) {
          r.blocked_before_release=true;r.progress_before_release=true;r.marker_old_before_release=true;
          break;
        }
        std::this_thread::yield();
      }
      gate.Release();releaser.join();r.helper_joined=true;
      const auto completion_end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
      do {
        r.idle=WaitRegMemNativeQueueTestAccess::Read(gpu).idle;
        if(!r.idle)std::this_thread::yield();
      }while(!r.idle&&std::chrono::steady_clock::now()<completion_end);
      Require(name,"bounded actual queues retired after finite signal",r.idle&&!gate.signal_failed.load(),
          "unexpected native/queue retirement failure; not intended synchronous-fetch RED");
      gpu.SendCommandSync([&] {
        DrainStats::g_wait_timer_test_observer=previous_wait;GpuPm4FetchWaitProbe::active=previous_probe;
        r.observer_restored=DrainStats::g_wait_timer_test_observer==previous_wait&&GpuPm4FetchWaitProbe::active==previous_probe;
        r.full=probe.full;r.tick=probe.tick;r.priority=probe.priority;
        scheduler.Finish();scheduler.DrainPriorityOperations();
        proof.Invalidate();source.Invalidate();
        std::array<uint32_t,10> native{},backing{};uint32_t low=0,high=0;
        std::memcpy(&low,proof.data,4u);
        std::memcpy(native.data(),static_cast<uint8_t*>(proof.data)+4u,sizeof(native));
        std::memcpy(&high,static_cast<uint8_t*>(proof.data)+4u+sizeof(native),4u);
        r.native=native==golden;r.guards=low==guard&&high==guard;
        std::memcpy(&r.native_word,source.data,4u);r.native_producer=r.native_word==golden[changed];
        r.backing=LibKernel::Memory::TryReadBacking(packet,backing.data(),sizeof(backing))&&backing==golden;
        r.markers=LibKernel::Memory::TryReadBacking(done_a,&r.a,4u)&&r.a==a_value&&
          LibKernel::Memory::TryReadBacking(done_b,&r.b,4u)&&r.b==b_value&&
          LibKernel::Memory::TryReadBacking(done_tail,&r.tail,4u)&&r.tail==tail_value;
        uint32_t before=0,after=0;
        r.guards=r.guards&&LibKernel::Memory::TryReadBacking(packet-4u,&before,4u)&&before==guard&&
          LibKernel::Memory::TryReadBacking(packet+sizeof(golden),&after,4u)&&after==guard;
        r.clean=!cache.IsRegionGpuModified(packet,sizeof(golden));
      });
      r.signal_failed=gate.signal_failed.load();r.gate_timeout=gate.timed_out.load();r.gate_signalled=gate.signalled.load();
      vulkan.Device().destroySemaphore(native_gate,nullptr);
    }
    gpu.SendCommandSync([&] {scheduler.Finish();scheduler.DrainPriorityOperations();});
    renderer.UnmapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    LibKernel::Memory::InstallGpuResources(nullptr);
  } // Native buffers/producer pipelines and owned kernel mapping retired before assertions.
  renderer.ShutdownGpu(); // Actual guest worker joined before the intended RED assertions.
  for(const auto& r:results) {
    std::printf("[pm4-fetch-yield] header=%u setup=%u old=%u gpuDirty=%u independentBeforeRelease=%u "
      "Ablocked=%u Aold=%u idle=%u native=%u producer=%08x backing=%u markers=%08x/%08x/%08x "
      "guards=%u clean=%u restored=%u helperJoined=%u signalFailed=%u gateTimeout=%u signalled=%u "
      "dispatcherFullTickPriority=%u/%u/%u cleanup=PASS\n",r.header,r.setup,r.old_backing,r.real_gpu_dirty,
      r.progress_before_release,r.blocked_before_release,r.marker_old_before_release,r.idle,r.native,
      r.native_word,r.backing,r.a,r.b,r.tail,r.guards,r.clean,r.observer_restored,r.helper_joined,
      r.signal_failed,r.gate_timeout,r.gate_signalled,r.full,r.tick,r.priority);
  }
  std::fflush(stdout);
  for(const auto& r:results) Require(name,"real native packet/publication/markers/guards/cleanup BEFORE cost",
      r.setup&&r.old_backing&&r.real_gpu_dirty&&r.idle&&r.native&&r.native_producer&&r.backing&&r.markers&&
      r.guards&&r.clean&&r.observer_restored&&r.helper_joined&&!r.signal_failed&&!r.gate_timeout&&r.gate_signalled,
      "native value, old-byte, continuation, bounds or cleanup failure is NOT intended RED");
  for(const auto& r:results) Require(name,"GPU-owned PM4 fetch suspends instead of monopolizing dispatcher",
      r.full+r.tick+r.priority==0u&&r.blocked_before_release&&r.progress_before_release&&r.marker_old_before_release,
      "actual GPU-written header/payload entered host wait or prevented independent compute-queue progress");
}
} // namespace
} // namespace Libs::Graphics
