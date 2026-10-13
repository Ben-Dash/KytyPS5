// Include AFTER WaitRegMemNativeNonblockingCostTests.inc.
// Actual graphics/compute GuestGpu submission queues, not manual Process retries.
namespace Libs::Graphics {
struct WaitRegMemNativeQueueTestAccess {
  static uint32_t PendingCommands(const GuestGpu& gpu) {
    return gpu.m_pending_commands.load(std::memory_order_acquire);
  }
  struct State { bool both_blocked=false,third_completed=false,idle=false; };
  static State Read(GuestGpu& gpu) {
    Common::LockGuard lock(gpu.m_queue_mutex);
    return {!gpu.m_queues[0].empty() && gpu.m_queues[0].front().blocked &&
            !gpu.m_queues[1].empty() && gpu.m_queues[1].front().blocked,
            gpu.m_queues[2].empty() && gpu.m_outstanding.size()==2u,
            !gpu.m_processing && gpu.m_commands.empty() && gpu.m_submission_count==0};
  }
};
namespace {
struct WaitRegMemNativeQueueLatencyProbe {
  BufferCache* cache=nullptr;uint64_t flag=0;uint32_t expected=0;
  Common::UniqueFunction<void> signal_gate;
  std::atomic<bool> armed{false},wait_seen{false};
  bool awaiting=false,published=false,notified=false;
  uint32_t native_word=0,ready_timeouts=0;
  uint64_t cv_wait_ns=0;
  std::chrono::steady_clock::time_point cv_start{};
  static inline thread_local WaitRegMemNativeQueueLatencyProbe* active=nullptr;
  static void Enter() {
    auto* probe=active;if(probe==nullptr || !probe->armed.exchange(false,std::memory_order_acq_rel))return;
    // A finite real GPU producer is released at a real all-blocked queue boundary.
    // The dispatcher already holds queue_mutex; ordinary publication does not need it.
    probe->signal_gate();
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(1);
    do {
      if(!probe->cache->IsRegionGpuModified(probe->flag,4u) &&
          LibKernel::Memory::TryReadBacking(probe->flag,&probe->native_word,4u) &&
          probe->native_word==probe->expected) {probe->published=true;break;}
      std::this_thread::yield();
    }while(std::chrono::steady_clock::now()<end);
    // Bounded scheduling stimulus: let the real publisher reach its notification.
    // This1ms is not claimed as natural game latency or benchmarked as a speedup.
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    probe->cv_start=std::chrono::steady_clock::now();probe->awaiting=true;
  }
  static void Leave(bool signalled) {
    auto* probe=active;if(probe==nullptr || !probe->awaiting)return;
    probe->awaiting=false;probe->notified=signalled;
    probe->cv_wait_ns=static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now()-probe->cv_start).count());
    if(!signalled && probe->published)++probe->ready_timeouts;
    probe->wait_seen.store(true,std::memory_order_release);
  }
};

void CheckWaitRegMemNativeQueuedNeighbor(VulkanHarness& vulkan, bool latency_control = false) {
  constexpr const char* name="WaitRegMemNativeQueuedNeighbor";
  constexpr uint64_t base=0x00000002c2400000ull,bytes=0x40000,flag_address=base+0x10000;
  constexpr uint64_t done_a=base+0x20000,done_b=base+0x20004,done_c=base+0x20008;
  constexpr uint32_t value=0x5145a7c3u,guard=0x5a5a5a5au;
  Require(name,"real host-side queue control",!GuestGpu::IsGpuThread() && !CommandRecordingDeferred(),
      "call outside GPU worker after InitializeGpu; setup failure is not RED");
  Require(name,"existing protected fault route",Common::HostException::InstallHandler(
    [](const Common::HostException::ExceptionInfo& info) {
      if(info.type!=Common::HostException::ExceptionType::AccessViolation ||
         (info.access_violation_type!=Common::HostException::AccessViolationType::Read &&
          info.access_violation_type!=Common::HostException::AccessViolationType::Write))return false;
      return LibKernel::Memory::HandleGpuFault(
        info.access_violation_type==Common::HostException::AccessViolationType::Write ?
          PageFaultAccess::Write : PageFaultAccess::Read,info.access_violation_vaddr);
    }),"real memory handler unavailable; setup failure is not RED");
  int64_t direct=-1;
  Require(name,"actual guest allocation setup",LibKernel::Memory::KernelAllocateDirectMemory(
      0,LibKernel::Memory::KernelGetDirectMemorySize(),bytes,0x10000,0,&direct)==0,
      "synthetic direct allocation failed; not an intended RED");
  void* mapped=reinterpret_cast<void*>(base);
  Require(name,"actual guest mapping setup",LibKernel::Memory::KernelMapDirectMemory(
      &mapped,bytes,0x3,0x10,direct,0x10000)==0 && mapped==reinterpret_cast<void*>(base),
      "owned mapped pages unavailable; not RED");
  std::memset(mapped,0x5a,bytes);*reinterpret_cast<uint32_t*>(flag_address)=0;
  for(const auto p:{done_a,done_b,done_c})*reinterpret_cast<uint32_t*>(p)=0;
  auto& renderer=vulkan.RuntimeRenderer();auto& gpu=renderer.GetGpu();
  auto& scheduler=renderer.GetCommandScheduler();auto& cache=renderer.GetBufferCache();
  HW::Context registers{};HW::UserConfig user_config{};HW::Shader shaders{};
  std::unique_ptr<EopComputeProducer> producer;
  vk::SemaphoreTypeCreateInfo type{};type.semaphoreType=vk::SemaphoreType::eTimeline;
  vk::SemaphoreCreateInfo create{};create.pNext=&type;vk::Semaphore gate=nullptr;
  RequireVulkanSuccess(vulkan.Device().createSemaphore(&create,nullptr,&gate),"bounded native queue producer gate");
  WaitRegMemWaitBudget budget;bool setup=false;
  WaitRegMemNativeQueueLatencyProbe latency;latency.cache=&cache;latency.flag=flag_address;latency.expected=value;
  gpu.SendCommandSync([&] {
    scheduler.Begin(registers,user_config,shaders);
    LibKernel::Memory::InstallGpuResources(&renderer);renderer.MapMemory(base,bytes);
    auto [owner,offset]=cache.ObtainBuffer(flag_address,4,true,true);
    Require(name,"real native scalar owner setup",owner!=nullptr && offset==0,
        "finite producer first word offset changed; not RED");
    producer=std::make_unique<EopComputeProducer>(vulkan.Device(),owner->Handle(),value);
      auto command=scheduler.Current().Handle();
    vk::BufferMemoryBarrier2 before{};
    before.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands|vk::PipelineStageFlagBits2::eHost;
    before.srcAccessMask=vk::AccessFlagBits2::eMemoryWrite|vk::AccessFlagBits2::eHostWrite;
    before.dstStageMask=vk::PipelineStageFlagBits2::eComputeShader;
    before.dstAccessMask=vk::AccessFlagBits2::eShaderWrite;
    before.srcQueueFamilyIndex=before.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    before.buffer=owner->Handle();before.offset=0;before.size=4;
    vk::DependencyInfo dependency{};dependency.bufferMemoryBarrierCount=1;dependency.pBufferMemoryBarriers=&before;
    command.pipelineBarrier2(dependency);producer->Record(command);cache.OnCommandRecorded();
    uint32_t old=UINT32_MAX;setup=cache.IsRegionGpuModified(flag_address,4) &&
        LibKernel::Memory::TryReadBacking(flag_address,&old,4) && old==0;
    SubmitInfo submission{};submission.AddWait(gate,1);scheduler.Flush(submission);
    WaitRegMemWaitBudget::active=&budget;DrainStats::g_wait_timer_test_observer=WaitRegMemWaitBudget::Observe;
  });
  Require(name,"independent GPU producer old backing setup",setup,"flag was not genuinely GPU-owned");
  const auto commands_a=LiteralCostWaitThenDone(flag_address,4,value,UINT32_MAX,done_a,11);
  const uint64_t wide_value=(static_cast<uint64_t>(guard)<<32u)|value;
  const auto commands_b=LiteralCostWaitThenDone(flag_address,8,wide_value,UINT64_MAX,done_b,22);
  const std::array<uint32_t,5> commands_c{KYTY_PM4(5,Pm4::IT_WRITE_DATA,0),0u,
      static_cast<uint32_t>(done_c),static_cast<uint32_t>(done_c>>32u),33};
  std::mutex mutex;std::condition_variable cv;bool release=false,signal_failed=false;
  std::atomic<bool> gate_signalled{false};
  const auto signal_native_gate=[&] {
    if(gate_signalled.exchange(true,std::memory_order_acq_rel))return;
    vk::SemaphoreSignalInfo signal{};signal.semaphore=gate;signal.value=1;
    signal_failed=vulkan.Device().signalSemaphore(&signal)!=vk::Result::eSuccess;
  };
  latency.signal_gate=signal_native_gate;
  if(latency_control)gpu.SendCommandSync([&] {
    WaitRegMemNativeQueueLatencyProbe::active=&latency;
    DrainStats::g_blocked_wait_enter_test_observer=WaitRegMemNativeQueueLatencyProbe::Enter;
    DrainStats::g_blocked_wait_exit_test_observer=WaitRegMemNativeQueueLatencyProbe::Leave;
  });
  std::jthread release_gate([&] {
    {std::unique_lock lock(mutex);cv.wait_for(lock,std::chrono::milliseconds(latency_control?2000:500),[&]{return release;});}
    signal_native_gate();
  });
  // Borrowed command vectors remain alive through observed queue idle and native Finish.
  gpu.Submit(commands_a,{});gpu.SubmitCompute(0x20u,commands_b);gpu.SubmitCompute(0x21u,commands_c);
  bool both_pending=false,other_progress_before_publication=false;
  const auto observation_end=std::chrono::steady_clock::now()+std::chrono::milliseconds(400);
  while(std::chrono::steady_clock::now()<observation_end) {
    const auto state=WaitRegMemNativeQueueTestAccess::Read(gpu);
    if(state.both_blocked && state.third_completed) {
      uint32_t flag=UINT32_MAX,third=0;
      both_pending=true;other_progress_before_publication=
          LibKernel::Memory::TryReadBacking(flag_address,&flag,4) && flag==0 &&
          LibKernel::Memory::TryReadBacking(done_c,&third,4) && third==33;
      break;
    }
    std::this_thread::yield();
  }
  if(latency_control && both_pending && other_progress_before_publication) {
    latency.armed.store(true,std::memory_order_release);
    const auto probe_end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(!latency.wait_seen.load(std::memory_order_acquire) && std::chrono::steady_clock::now()<probe_end)
      std::this_thread::yield();
  }
  {std::lock_guard lock(mutex);release=true;}cv.notify_all();release_gate.join();
  const auto completion_end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  bool idle=false;
  do {
    idle=WaitRegMemNativeQueueTestAccess::Read(gpu).idle;
    if(!idle)std::this_thread::yield();
  }while(!idle && std::chrono::steady_clock::now()<completion_end);
  Require(name,"bounded real queues complete",idle && !signal_failed,
      "real WAIT queues did not retire after finite GPU producer; do not enter an unbounded idle wait");
  uint32_t actual=0,a=0,b=0,c=0;bool guards=false,clean=false;
  gpu.SendCommandSync([&] {
    DrainStats::g_wait_timer_test_observer=nullptr;WaitRegMemWaitBudget::active=nullptr;
    DrainStats::g_blocked_wait_enter_test_observer=nullptr;DrainStats::g_blocked_wait_exit_test_observer=nullptr;
    WaitRegMemNativeQueueLatencyProbe::active=nullptr;
    scheduler.Finish();
    actual=*reinterpret_cast<const uint32_t*>(flag_address);
    a=*reinterpret_cast<const uint32_t*>(done_a);b=*reinterpret_cast<const uint32_t*>(done_b);
    c=*reinterpret_cast<const uint32_t*>(done_c);
    guards=*reinterpret_cast<const uint32_t*>(flag_address-4)==guard &&
           *reinterpret_cast<const uint32_t*>(flag_address+4)==guard;
    clean=!cache.IsRegionGpuModified(flag_address,8);
    renderer.UnmapMemory(base,bytes);LibKernel::Memory::InstallGpuResources(nullptr);
    producer.reset();
  });
  vulkan.Device().destroySemaphore(gate,nullptr);
  Require(name,"owned mapping cleanup",LibKernel::Memory::KernelMunmap(base,bytes)==0 &&
      LibKernel::Memory::KernelReleaseDirectMemory(direct,bytes)==0,"native queue fixture mapping did not retire");
  Require(name,"true queues native32/64 compare and guards",actual==value && a==11 && b==22 && c==33 && guards && clean,
      "native GPU bytes/PM4 mask/continuation/guard/publication controls failed");
  std::printf("[wait-queues] nativeWord=PASS graphics32=11 compute64=22 otherCompute=33 guards=PASS bothBlocked=%u thirdBeforePublish=%u full/tick/priority=%u/%u/%u cleanup=PASS\n",
      both_pending?1u:0u,other_progress_before_publication?1u:0u,budget.full,budget.tick,budget.priority);
  Require(name,"both actual queues yield while third progresses",both_pending && other_progress_before_publication,
      "no real graphics+compute all-blocked state or independent compute progress before GPU publication");
  if(latency_control) {
    std::printf("[ready-continuation] actualNativePublished=%u native=%08x realCVNotified=%u readyTimeouts=%u cvWaitNs=%llu injectedPreWaitMs=1\n",
        latency.published?1u:0u,latency.native_word,latency.notified?1u:0u,latency.ready_timeouts,
        static_cast<unsigned long long>(latency.cv_wait_ns));
    Require(name,"bounded actual native ready stimulus",latency.wait_seen.load(std::memory_order_acquire) &&
        latency.published && latency.native_word==value,"native publisher/control did not reach the real blocked wait; not intended RED");
    Require(name,"actual ready queue receives notification",latency.ready_timeouts==0,
        "real PM4 queues timed out after authoritative native publication instead of receiving its notification");
  }
  Require(name,"true queues avoid per-predicate synchronization",budget.full+budget.tick+budget.priority==0,
      "true WAIT queues still synchronize the dispatcher instead of retrying actual publication");
}
} // namespace
} // namespace Libs::Graphics
