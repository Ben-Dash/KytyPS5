// Include AFTER ReadbackQueueBootstrapTests.inc and LiteralEopPublicationCostTests.inc.
// Actual public GPU-thread ReadMemory, one graphics queue, finite shaders/copies.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
namespace Libs::Graphics {
namespace {
void ConfigureArmedReadbackWindowCostTests() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  _putenv_s("KYTY_READBACK_QUEUE","0");_putenv_s("KYTY_DEBUG_READBACK_AGE","");
  _putenv_s("KYTY_RECORDING_AGE_US","0");
#else
  setenv("KYTY_READBACK_QUEUE","0",1);unsetenv("KYTY_DEBUG_READBACK_AGE");
  setenv("KYTY_RECORDING_AGE_US","0",1);
#endif
}
struct ArmedWindowGate {
  std::mutex mutex;std::condition_variable cv;
  bool release=false,read_start=false,callback_entered=false,timeout=false;
  void Hold() {
    std::unique_lock lock(mutex);callback_entered=true;cv.notify_all();
    if(!cv.wait_for(lock,std::chrono::seconds(5),[&]{return release;}))timeout=true;
  }
  void ReleaseAfterReadStarts() {
    std::unique_lock lock(mutex);
    if(!cv.wait_for(lock,std::chrono::seconds(5),[&]{return read_start;}))timeout=true;
    lock.unlock();
    // A finite CPU stimulus only. Either still-armed or just-published A is
    // semantically ready; BOTH states must avoid widening A's request onto B.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    lock.lock();release=true;lock.unlock();cv.notify_all();
  }
  void StartRead() {
    {std::lock_guard lock(mutex);read_start=true;}cv.notify_all();
  }
};
struct ArmedWindowResult {
  bool setup=false,same_owner=false,eop_pending=false,native_eop=false;
  bool before_state=false,gate_entered=true,gate_ok=true,read_correct=false;
  bool b_pending=true,b_unarmed=true,no_submit=true,new_writer_wait=true;
  bool final_native=false,final_cpu=false,final_b=false,cpu_overwrite=true;
  uint64_t before=0u,after=0u,tick_before=0u,tick_after=0u;
  uint32_t eop_native=0u,cpu_after_read=0u,native_final=0u,cpu_final=0u,b_final=0u;
};
void CheckArmedReadbackWindowCost(VulkanHarness& vulkan) {
  constexpr const char* name="ArmedReadbackWindowCost";
  constexpr uint64_t a=FetchGpuGuest::Base+0x5000u,b=FetchGpuGuest::Base+0x6000u;
  constexpr uint32_t old=0x5a5a5a5au,literal=0x12345678u,gpu_new=0xabcdef09u,cpu_new=0x13579bdfu;
  constexpr uint32_t b_marker=0x76543210u;
  constexpr std::array labels{"old-armed-A","published-clean-A","new-unarmed-A","CPU-overwritten-A"};
  std::array<ArmedWindowResult,4> results{};
  namespace Exception=Common::HostException;
  Require(name,"real fault route",Exception::InstallHandler([](const Exception::ExceptionInfo& i) {
    if(i.type!=Exception::ExceptionType::AccessViolation||
       (i.access_violation_type!=Exception::AccessViolationType::Read&&
        i.access_violation_type!=Exception::AccessViolationType::Write))return false;
    return LibKernel::Memory::HandleGpuFault(
        i.access_violation_type==Exception::AccessViolationType::Write?PageFaultAccess::Write:PageFaultAccess::Read,
        i.access_violation_vaddr);
  }),"fault route bootstrap failed; not RED");
  {
    FetchGpuGuest guest;auto& graphics=vulkan.RuntimeContext();auto& renderer=vulkan.RuntimeRenderer();
    renderer.InitializeGpu(nullptr);LibKernel::Memory::InstallGpuResources(&renderer);
    renderer.MapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    FetchGpuBuffer b_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer a_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer proof(graphics,8u,vk::BufferUsageFlagBits::eTransferDst);
    b_source.Flush();a_source.Flush();proof.Flush();
    EopComputeProducer b_compute(vulkan.Device(),b_source.buffer,b_marker);
    EopComputeProducer a_compute(vulkan.Device(),a_source.buffer,gpu_new);
    std::unique_ptr<CommandProcessor> cp;
    renderer.GetGpu().SendCommandSync([&] {
      cp=std::make_unique<CommandProcessor>(renderer,0x31);cp->Reset();cp->BufferInit();
    });
    for(uint32_t test=0u;test<results.size();++test) {
      auto& out=results[test];ArmedWindowGate gate;
      std::unique_ptr<std::jthread> release_worker;
      if(test==0u)release_worker=std::make_unique<std::jthread>([&]{gate.ReleaseAfterReadStarts();});
      std::memcpy(reinterpret_cast<void*>(a),&old,4u);std::memcpy(reinterpret_cast<void*>(b),&old,4u);
      renderer.GetGpu().SendCommandSync([&] {
        auto& cache=renderer.GetBufferCache();auto& scheduler=renderer.GetCommandScheduler();
        out.setup=GuestGpu::IsGpuThread()&&!PerformanceMemoryTestAccess::HasQueue(cache)&&
            graphics.readback_queue==nullptr&&!ReadbackAgeEnabled();
        const auto producer=scheduler.CurrentTick();
        if(test==0u)scheduler.DeferPriorityOperation([&]{gate.Hold();});
        cp->WriteAtEndOfPipe32(0u,0u,0x2fu,0u,6u,2u,reinterpret_cast<void*>(a),literal,0u);
        auto [owner,offset]=cache.ObtainBuffer(a,4u,false,true);
        CopyEopNativeLabel(scheduler,*owner,offset,proof,0u,4u);
        uint32_t pending=0u;
        out.eop_pending=LibKernel::Memory::TryReadBacking(a,&pending,4u)&&pending==old&&
            PerformanceMemoryTestAccess::Readback(cache,a,4u).gpu_dirty&&
            !scheduler.IsFree(producer);
        scheduler.Flush();scheduler.GetMasterSemaphore().Wait(producer);
        proof.Invalidate();std::memcpy(&out.eop_native,proof.data,4u);out.native_eop=out.eop_native==literal;
        if(test==0u) {
          std::unique_lock lock(gate.mutex);
          out.gate_entered=gate.cv.wait_for(lock,std::chrono::seconds(5),[&]{return gate.callback_entered;});
          const auto state=PerformanceMemoryTestAccess::Readback(cache,a,4u);
          out.before_state=state.gpu_dirty&&!state.unarmed&&state.tick==producer&&
              scheduler.GetMasterSemaphore().IsFree(producer);
        } else {
          scheduler.WaitPriorityOperations(producer);
          out.before_state=!PerformanceMemoryTestAccess::Readback(cache,a,4u).gpu_dirty;
        }
        ReadbackBootstrapWrite(scheduler,cache,b_compute,b_source,b);
        out.same_owner=owner->Handle()==cache.GetBuffer(cache.FindBuffer(b,4u)).Handle();
        if(test==2u) {
          ReadbackBootstrapWrite(scheduler,cache,a_compute,a_source,a);
          const auto state=PerformanceMemoryTestAccess::Readback(cache,a,4u);
          out.before_state=out.before_state&&state.gpu_dirty&&state.unarmed;
        } else if(test==3u) {
          *reinterpret_cast<volatile uint32_t*>(a)=cpu_new;
          uint32_t value=0u;out.cpu_overwrite=LibKernel::Memory::TryReadBacking(a,&value,4u)&&value==cpu_new;
          out.before_state=out.before_state&&!PerformanceMemoryTestAccess::Readback(cache,a,4u).gpu_dirty;
        }
        auto& ring=cache.GetUtilityBuffer(MemoryUsage::Download);
        out.before=StreamBufferTestAccess::Offset(ring);out.tick_before=scheduler.CurrentTick();
        if(test==0u)gate.StartRead();
        cache.ReadMemory(a,4u);
        out.after=StreamBufferTestAccess::Offset(ring);out.tick_after=scheduler.CurrentTick();
        const uint32_t expected=test==2u?gpu_new:test==3u?cpu_new:literal;
        out.read_correct=LibKernel::Memory::TryReadBacking(a,&out.cpu_after_read,4u)&&out.cpu_after_read==expected;
        const auto b_state=PerformanceMemoryTestAccess::Readback(cache,b,4u);uint32_t b_before=0u;
        if(test!=2u) {
          out.b_pending=LibKernel::Memory::TryReadBacking(b,&b_before,4u)&&b_before==old&&b_state.gpu_dirty;
          out.b_unarmed=b_state.unarmed;
          out.no_submit=out.tick_after==out.tick_before&&!scheduler.IsFree(out.tick_before);
        } else {
          out.new_writer_wait=scheduler.IsFree(out.tick_before)&&out.tick_after>out.tick_before;
        }
        auto [final_owner,final_offset]=cache.ObtainBuffer(a,4u,false,true);
        CopyEopNativeLabel(scheduler,*final_owner,final_offset,proof,4u,4u);
        scheduler.Finish();scheduler.WaitPriorityOperations(scheduler.CurrentTick()-1u);
        cache.ReadMemory(a,4u);cache.ReadMemory(b,4u);proof.Invalidate();
        std::memcpy(&out.native_final,static_cast<uint8_t*>(proof.data)+4u,4u);
        out.final_native=out.native_final==expected;
        out.final_cpu=LibKernel::Memory::TryReadBacking(a,&out.cpu_final,4u)&&out.cpu_final==expected;
        out.final_b=LibKernel::Memory::TryReadBacking(b,&out.b_final,4u)&&out.b_final==b_marker;
        scheduler.Finish();scheduler.DrainPriorityOperations();
      });
      if(release_worker)release_worker->join();
      {std::lock_guard lock(gate.mutex);out.gate_ok=!gate.timeout;}
    }
    renderer.UnmapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    renderer.ShutdownGpu();LibKernel::Memory::InstallGpuResources(nullptr);cp.reset();
  }
  for(uint32_t test=0u;test<results.size();++test) {
    const auto& out=results[test];
    std::printf("[gpu] ArmedWindow %s setup=%u sameOwner=%u eop=%08x beforeState=%u gate=%u "
        "ring=%llu->%llu tick=%llu->%llu Bpending=%u Bunarmed=%u newWait=%u read=%08x native=%08x final=%08x B=%08x\n",
        labels[test],out.setup,out.same_owner,out.eop_native,out.before_state,out.gate_entered&&out.gate_ok,
        static_cast<unsigned long long>(out.before),static_cast<unsigned long long>(out.after),
        static_cast<unsigned long long>(out.tick_before),static_cast<unsigned long long>(out.tick_after),
        out.b_pending,out.b_unarmed,out.new_writer_wait,out.cpu_after_read,out.native_final,out.cpu_final,out.b_final);
    Require(labels[test],"real producer/old-armed or clean state/new-writer/CPU overwrite and native cleanup controls",
        out.setup&&out.same_owner&&out.eop_pending&&out.native_eop&&out.before_state&&
        out.gate_entered&&out.gate_ok&&out.read_correct&&out.cpu_overwrite&&out.new_writer_wait&&
        out.final_native&&out.final_cpu&&out.final_b&&out.b_pending&&out.no_submit,
        "setup/fence/value/ownership controls failed; not the redundant-readback RED");
  }
  for(uint32_t test=0u;test<results.size();++test)if(test!=2u)
    Require(labels[test],"ready A causes no download of unrelated dirty B",
        results[test].before==results[test].after&&results[test].b_unarmed,
        "already armed/published/CPU-known A widened onto unrelated B and consumed native download storage");
}
} // namespace
} // namespace Libs::Graphics
