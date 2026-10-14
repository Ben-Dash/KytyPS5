// Include after existing ReadbackQueueBootstrapTests.inc. Original bootstrap
// fixture and goldens remain unchanged. No source/timestamp state is fabricated.
namespace Libs::Graphics {
namespace {
void CheckReadbackQueuePreparedRollover(VulkanHarness& vulkan) {
  constexpr const char* name = "ReadbackQueuePreparedRollover";
  constexpr uint32_t old = 0x5a5a5a5au, warm_value = 0x11223344u;
  constexpr uint32_t a_value = 0xabcdef09u, b_value = 0x76543210u;
  constexpr uint64_t warm = FetchGpuGuest::Base + 0x1000u;
  constexpr uint64_t a = FetchGpuGuest::Base + 0x5000u, b = a + 0x40u;
  bool setup=false,warm_correct=false,prepared_rejected=false,rollover=false;
  bool open_rejected=false,same_page_rejected=false,new_a_rejected=false;
  bool old_before=false,native_correct=false,final_correct=false,guards_correct=false,native_guards=false;
  uint64_t prepared_tick=0u,recorded_tick=0u;
  uint32_t native_a=0u,final_a=0u,final_b=0u;
  namespace Exception = Common::HostException;
  Require(name,"real fault route",Exception::InstallHandler([](const Exception::ExceptionInfo& i) {
    if(i.type!=Exception::ExceptionType::AccessViolation ||
       (i.access_violation_type!=Exception::AccessViolationType::Read &&
        i.access_violation_type!=Exception::AccessViolationType::Write)) return false;
    return LibKernel::Memory::HandleGpuFault(
      i.access_violation_type==Exception::AccessViolationType::Write ? PageFaultAccess::Write : PageFaultAccess::Read,
      i.access_violation_vaddr);
  }),"native fault setup failed; not intended RED");
  {
    FetchGpuGuest guest;
    auto& graphics=vulkan.RuntimeContext(); auto& renderer=vulkan.RuntimeRenderer();
    renderer.InitializeGpu(nullptr); LibKernel::Memory::InstallGpuResources(&renderer);
    renderer.MapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    std::memset(reinterpret_cast<void*>(FetchGpuGuest::Base),0x5a,FetchGpuGuest::Bytes);
    FetchGpuBuffer warm_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer a_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer b_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer proof(graphics,12u,vk::BufferUsageFlagBits::eTransferDst);
    warm_source.Flush();a_source.Flush();b_source.Flush();proof.Flush();
    EopComputeProducer warm_compute(vulkan.Device(),warm_source.buffer,warm_value);
    EopComputeProducer a_compute(vulkan.Device(),a_source.buffer,a_value);
    EopComputeProducer b_compute(vulkan.Device(),b_source.buffer,b_value);
    PerformanceMemoryTestAccess::Request accepted,same_page,fresh;
    renderer.GetGpu().SendCommandSync([&] {
      auto& cache=renderer.GetBufferCache();auto& scheduler=renderer.GetCommandScheduler();
      CommandProcessor cp(renderer,0x32);cp.Reset();cp.BufferInit();
      setup=GuestGpu::IsGpuThread() && !ReadbackAgeEnabled() && PerformanceMemoryTestAccess::HasQueue(cache) &&
        graphics.readback_queue!=nullptr && graphics.readback_queue!=graphics.queue &&
        graphics.readback_family!=graphics.queue_family;
      ReadbackBootstrapWrite(scheduler,cache,warm_compute,warm_source,warm);
      cache.ReadMemory(warm,4u);uint32_t warm_actual=0u;
      warm_correct=LibKernel::Memory::TryReadBacking(warm,&warm_actual,4u)&&warm_actual==warm_value;
      // A's writable binding is prepared but its guest native write does not
      // exist yet. Existing query stamps it here on the buggy implementation.
      auto [owner,offset]=cache.ObtainBuffer(a,4u,true,true);
      prepared_tick=scheduler.CurrentTick();
      const auto pre=PerformanceMemoryTestAccess::Plan(cache,a,4u);
      prepared_rejected=!pre;
      Require(name,"safe prepared-binding control",prepared_rejected,
        "prepared open writer was accepted before actual native work; separate admission defect, not rollover RED");
      // Put genuinely finite GPU work on T2, then cause a real tick rollover.
      // It writes only a_source, not A. No fake scheduler/producer metadata.
      const auto before=scheduler.Current().Handle();
      vk::MemoryBarrier host{};host.srcAccessMask=vk::AccessFlagBits::eHostWrite;
      host.dstAccessMask=vk::AccessFlagBits::eShaderWrite;
      before.pipelineBarrier(vk::PipelineStageFlagBits::eHost,vk::PipelineStageFlagBits::eComputeShader,
        {},1u,&host,0u,nullptr,0u,nullptr);
      a_compute.Record(before);
      scheduler.Flush();scheduler.GetMasterSemaphore().Wait(prepared_tick);
      scheduler.WaitPriorityOperations(prepared_tick);
      recorded_tick=scheduler.CurrentTick();
      rollover=recorded_tick>prepared_tick && scheduler.IsFree(prepared_tick) && !scheduler.IsFree(recorded_tick);
      // Record A on T3 through its SAME previously prepared binding, without
      // reacquiring/adding a new NoteGpuWrite that would hide the defect.
      const auto command=scheduler.Current().Handle();
      vk::MemoryBarrier transfer{};transfer.srcAccessMask=vk::AccessFlagBits::eShaderWrite;
      transfer.dstAccessMask=vk::AccessFlagBits::eTransferRead|vk::AccessFlagBits::eTransferWrite;
      command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,vk::PipelineStageFlagBits::eTransfer,
        {},1u,&transfer,0u,nullptr,0u,nullptr);
      const vk::BufferCopy write{0u,offset,4u};
      command.copyBuffer(a_source.buffer,owner->Handle(),1u,&write);
      cache.OnCommandRecorded();
      uint32_t backing=0u;
      old_before=LibKernel::Memory::TryReadBacking(a,&backing,4u)&&backing==old &&
        cache.HasGpuDirtyBytes(a,4u)&&scheduler.CurrentTick()==recorded_tick&&!scheduler.IsFree(recorded_tick);
      accepted=PerformanceMemoryTestAccess::Plan(cache,a,4u);
      open_rejected=!accepted;
      // Never publish an incorrectly admitted transfer while T3 is OPEN.
      // Completing T3 first makes any priority publication safe for cleanup.
      CopyEopNativeLabel(scheduler,*owner,offset,proof,4u,4u);
      scheduler.Flush();scheduler.GetMasterSemaphore().Wait(recorded_tick);
      scheduler.WaitPriorityOperations(recorded_tick);proof.Invalidate();
      std::memcpy(&native_a,static_cast<const uint8_t*>(proof.data)+4u,4u);
      native_correct=native_a==a_value;
      const auto* proof_words=static_cast<const uint32_t*>(proof.data);
      native_guards=proof_words[0]==0u&&proof_words[2]==0u;
    });
    if(accepted)PerformanceMemoryTestAccess::Publish(renderer.GetBufferCache(),accepted);
    renderer.GetGpu().SendCommandSync([&] {
      auto& cache=renderer.GetBufferCache();auto& scheduler=renderer.GetCommandScheduler();
      // Fresh completed A with an OPEN B on the same tracker page must reject
      // downloading that whole page, preserving B's real future write.
      ReadbackBootstrapWrite(scheduler,cache,a_compute,a_source,a);
      const auto a_tick=scheduler.CurrentTick();scheduler.Flush();
      scheduler.GetMasterSemaphore().Wait(a_tick);scheduler.WaitPriorityOperations(a_tick);
      ReadbackBootstrapWrite(scheduler,cache,b_compute,b_source,b);
      same_page=PerformanceMemoryTestAccess::Plan(cache,a,4u);
      same_page_rejected=!same_page;
      scheduler.Finish();scheduler.DrainPriorityOperations();
      cache.ReadMemory(a,4u);cache.ReadMemory(b,4u);
      // A subsequent freshly recorded OPEN A writer is also not eligible.
      ReadbackBootstrapWrite(scheduler,cache,a_compute,a_source,a);
      fresh=PerformanceMemoryTestAccess::Plan(cache,a,4u);new_a_rejected=!fresh;
      scheduler.Finish();scheduler.DrainPriorityOperations();
      cache.ReadMemory(a,4u);cache.ReadMemory(b,4u);
      final_correct=LibKernel::Memory::TryReadBacking(a,&final_a,4u)&&final_a==a_value&&
        LibKernel::Memory::TryReadBacking(b,&final_b,4u)&&final_b==b_value;
      uint32_t low=0u,high=0u;
      guards_correct=LibKernel::Memory::TryReadBacking(a-4u,&low,4u)&&low==old&&
        LibKernel::Memory::TryReadBacking(b+4u,&high,4u)&&high==old;
      scheduler.Finish();scheduler.DrainPriorityOperations();
    });
    // Any unexpected control admission is published only from the foreign
    // thread after all real producers completed, preserving safe cleanup.
    if(same_page)PerformanceMemoryTestAccess::Publish(renderer.GetBufferCache(),same_page);
    if(fresh)PerformanceMemoryTestAccess::Publish(renderer.GetBufferCache(),fresh);
    renderer.UnmapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    Require(name,"context mapping cleanup",!renderer.IsMapped(FetchGpuGuest::Base,FetchGpuGuest::Bytes),
      "fixture range still mapped before final cost oracle");
    renderer.ShutdownGpu();LibKernel::Memory::InstallGpuResources(nullptr);
  } // actual native buffers, producers and synthetic guest allocation released
  std::printf("[readback-prepared-rollover] setup=%u warm=%u preparedReject=%u Tprepare=%llu Trecord=%llu "
    "rollover=%u oldBefore=%u nativeA=%08x finalA=%08x finalB=%08x guards=%u "
    "openReject=%u samePageReject=%u newAReject=%u cleanup=PASS\n",setup,warm_correct,prepared_rejected,
    static_cast<unsigned long long>(prepared_tick),static_cast<unsigned long long>(recorded_tick),
    rollover,old_before,native_a,final_a,final_b,guards_correct,open_rejected,same_page_rejected,new_a_rejected);
  Require(name,"finite native producers, real rollover, all values/guards/cleanup",
    setup&&warm_correct&&prepared_rejected&&rollover&&old_before&&native_correct&&native_guards&&final_correct&&guards_correct,
    "native/setup/control failed; not intended pending-query rollover RED");
  Require(name,"same-page and fresh-open writer admission controls",same_page_rejected&&new_a_rejected,
    "existing actual same-page/open-writer negative failed; not isolated rollover RED");
  Require(name,"prepared-query cannot hide the actual later OPEN producer",open_rejected,
    "query permanently dated prepared A before its real recorded T3 writer, admitting a transfer while T3 remained open");
}
} // namespace
} // namespace Libs::Graphics
