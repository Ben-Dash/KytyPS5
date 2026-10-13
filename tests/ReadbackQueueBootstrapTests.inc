// Test-only proposal; include after EndOfPipeLiteralNeighborTests.inc.
// Requires a real distinct transfer-family queue created before RenderContext.
#include <new>
#include "common/hostException.h"
#include "graphics/host_gpu/renderer/debug.h"
namespace Libs::Graphics {
struct PerformanceMemoryTestAccess {
  static void RecreateUploadRing(BufferCache& cache,uint64_t bytes) {
    // Caller has completed actual native work and drained all callbacks.
    cache.m_staging_buffer.~StreamBuffer();
    new (&cache.m_staging_buffer) StreamBuffer(cache.m_graphics,cache.m_scheduler,MemoryUsage::Upload,bytes);
  }
  static bool CpuDirty(BufferCache& cache,uint64_t address,uint64_t bytes) {
    return cache.m_memory_tracker.IsRegionCpuModified(address,bytes);
  }
  static ReadbackState Readback(BufferCache& cache,uint64_t address,uint64_t bytes) {
    return cache.m_memory_tracker.QueryReadback(address,bytes);
  }
  using Request = std::shared_ptr<BufferCache::QueuedReadback>;
  static Request Plan(BufferCache& cache,uint64_t address,uint64_t bytes) {
    return cache.QueueReadback(address,bytes,false);
  }
  static void Publish(BufferCache& cache,const Request& request) {
    cache.PublishQueuedReadback(*request);
  }
  static bool HasQueue(const BufferCache& cache) {return cache.m_readback_queue!=nullptr;}
  static void MarkHot(BufferCache& cache,uint64_t page) {cache.MarkReadbackHot(page);}
  static uint64_t ReadbackToken(const BufferCache& cache) {return cache.m_readback_token;}
  static bool EagerReadyContains(const BufferCache& cache,uint64_t page) {
    return std::find(cache.m_eager_ready.begin(),cache.m_eager_ready.end(),page)!=cache.m_eager_ready.end();
  }
  static void ResetEagerHotPolicy(BufferCache& cache) {
    for(const auto& [page,tick]:cache.m_hot_pages) {
      (void)tick;cache.m_memory_tracker.SetReadbackHot(page,TRACKER_PAGE_SIZE,false);
    }
    cache.m_hot_pages.clear();cache.m_eager_pending.clear();cache.m_eager_ready.clear();
  }
  static bool EagerReadyContainsOnly(const BufferCache& cache,uint64_t a,uint64_t b) {
    bool has_a=false,has_b=false;
    for(const auto p:cache.m_eager_ready) {
      if(p==a)has_a=true;else if(p==b)has_b=true;else return false;
    }
    return has_a&&has_b&&cache.m_eager_pending.empty();
  }
};
namespace {
void ConfigureReadbackQueueBootstrapTests() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  _putenv_s("KYTY_READBACK_QUEUE","1");
  _putenv_s("KYTY_DEBUG_READBACK_AGE","");
#else
  setenv("KYTY_READBACK_QUEUE","1",1);unsetenv("KYTY_DEBUG_READBACK_AGE");
#endif
}
void ReadbackBootstrapWrite(CommandScheduler& scheduler,BufferCache& cache,
                            EopComputeProducer& compute,FetchGpuBuffer& source,uint64_t address) {
  auto [owner,offset]=cache.ObtainBuffer(address,4u,true,true);
  const auto command=scheduler.Current().Handle();
  vk::MemoryBarrier ready{};ready.srcAccessMask=vk::AccessFlagBits::eHostWrite;
  ready.dstAccessMask=vk::AccessFlagBits::eShaderWrite;
  command.pipelineBarrier(vk::PipelineStageFlagBits::eHost,vk::PipelineStageFlagBits::eComputeShader,
      {},1u,&ready,0u,nullptr,0u,nullptr);
  compute.Record(command);
  vk::MemoryBarrier before{};before.srcAccessMask=vk::AccessFlagBits::eShaderWrite;
  before.dstAccessMask=vk::AccessFlagBits::eTransferRead|vk::AccessFlagBits::eTransferWrite;
  command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,vk::PipelineStageFlagBits::eTransfer,
      {},1u,&before,0u,nullptr,0u,nullptr);
  const vk::BufferCopy copy{0u,offset,4u};
  command.copyBuffer(source.buffer,owner->Handle(),1u,&copy);cache.OnCommandRecorded();
}
void CheckReadbackQueueBootstrap(VulkanHarness& vulkan) {
  constexpr const char* name="ReadbackQueueBootstrap";
  constexpr uint32_t old=0x5a5a5a5au,warm_value=0x11223344u,a_value=0xabcdef09u,b_value=0x76543210u;
  constexpr uint64_t warm=FetchGpuGuest::Base+0x1000u;
  constexpr uint64_t a=FetchGpuGuest::Base+0x5000u,b=FetchGpuGuest::Base+0x6000u;
  bool setup=false,warm_rejected=false,warm_pending=false,warm_correct=false;
  bool before_request=false,same_owner=false,queued=false,after_request=false,no_unrelated_submit=false;
  bool a_read=false,b_pending_before_cleanup=false,b_correct=false,a_correct=false;
  uint32_t native_a=0u,backing_a=0u,backing_b=0u;
  uint64_t expected_open_tick=0u;
  namespace Exception=Common::HostException;
  Require(name,"real fault route",Exception::InstallHandler([](const Exception::ExceptionInfo& i) {
    if(i.type!=Exception::ExceptionType::AccessViolation||
       (i.access_violation_type!=Exception::AccessViolationType::Read&&
        i.access_violation_type!=Exception::AccessViolationType::Write))return false;
    return LibKernel::Memory::HandleGpuFault(
        i.access_violation_type==Exception::AccessViolationType::Write?PageFaultAccess::Write:PageFaultAccess::Read,
        i.access_violation_vaddr);
  }),"production fault handler setup failed; not RED");
  {
    FetchGpuGuest guest;
    auto& graphics=vulkan.RuntimeContext();auto& renderer=vulkan.RuntimeRenderer();
    renderer.InitializeGpu(nullptr);LibKernel::Memory::InstallGpuResources(&renderer);
    renderer.MapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    std::memcpy(reinterpret_cast<void*>(warm),&old,4u);
    std::memcpy(reinterpret_cast<void*>(a),&old,4u);std::memcpy(reinterpret_cast<void*>(b),&old,4u);
    FetchGpuBuffer warm_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer a_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer b_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer a_proof(graphics,4u,vk::BufferUsageFlagBits::eTransferDst);
    warm_source.Flush();a_source.Flush();b_source.Flush();a_proof.Flush();
    EopComputeProducer warm_compute(vulkan.Device(),warm_source.buffer,warm_value);
    EopComputeProducer a_compute(vulkan.Device(),a_source.buffer,a_value);
    EopComputeProducer b_compute(vulkan.Device(),b_source.buffer,b_value);
    PerformanceMemoryTestAccess::Request request;
    renderer.GetGpu().SendCommandSync([&] {
      auto& cache=renderer.GetBufferCache();auto& scheduler=renderer.GetCommandScheduler();
      CommandProcessor cp(renderer,0x31);cp.Reset();cp.BufferInit();
      setup=GuestGpu::IsGpuThread()&&!ReadbackAgeEnabled()&&PerformanceMemoryTestAccess::HasQueue(cache)&&
          graphics.readback_queue!=nullptr&&graphics.readback_queue!=graphics.queue&&
          graphics.readback_family!=graphics.queue_family&&
          graphics.shared_queue_families[0]==graphics.queue_family&&
          graphics.shared_queue_families[1]==graphics.readback_family;
      ReadbackBootstrapWrite(scheduler,cache,warm_compute,warm_source,warm);
      const auto warm_tick=scheduler.CurrentTick();uint32_t warm_old=0u;
      warm_pending=LibKernel::Memory::TryReadBacking(warm,&warm_old,4u)&&warm_old==old&&
          cache.HasGpuDirtyBytes(warm,4u)&&!scheduler.IsFree(warm_tick);
      const auto first=PerformanceMemoryTestAccess::Plan(cache,warm,4u);
      warm_rejected=!first; // An open/untrusted initial producer cannot use the side queue.
      cache.ReadMemory(warm,4u);uint32_t value=0u;
      warm_correct=LibKernel::Memory::TryReadBacking(warm,&value,4u)&&value==warm_value;
      // A is genuinely recorded on this tick, then submitted/completed. No artificial
      // pre-submission Plan/InflightWriteTick call may hide the late-stamping defect.
      ReadbackBootstrapWrite(scheduler,cache,a_compute,a_source,a);
      auto [a_owner,a_offset]=cache.ObtainBuffer(a,4u,false,true);
      CopyEopNativeLabel(scheduler,*a_owner,a_offset,a_proof,0u,4u);
      const auto a_tick=scheduler.CurrentTick();scheduler.Flush();
      scheduler.GetMasterSemaphore().Wait(a_tick);scheduler.WaitPriorityOperations(a_tick);
      a_proof.Invalidate();std::memcpy(&native_a,a_proof.data,4u);
      // B is real unknown GPU work left OPEN on another4KiB page of the SAME16KiB cache owner.
      ReadbackBootstrapWrite(scheduler,cache,b_compute,b_source,b);
      same_owner=a_owner->Handle()==cache.GetBuffer(cache.FindBuffer(b,4u)).Handle();
      expected_open_tick=scheduler.CurrentTick();uint32_t av=0u,bv=0u;
      before_request=native_a==a_value&&LibKernel::Memory::TryReadBacking(a,&av,4u)&&av==old&&
          LibKernel::Memory::TryReadBacking(b,&bv,4u)&&bv==old&&cache.HasGpuDirtyBytes(a,4u)&&
          cache.HasGpuDirtyBytes(b,4u)&&!scheduler.IsFree(expected_open_tick);
      request=PerformanceMemoryTestAccess::Plan(cache,a,4u);queued=bool(request);
      after_request=scheduler.CurrentTick()==expected_open_tick&&!scheduler.IsFree(expected_open_tick)&&
          cache.HasGpuDirtyBytes(b,4u);
    });
    // Foreign reader performs actual transfer submission/publication; never invoke the
    // transfer Vulkan calls on Thread_Gpu's deferred-recording thread.
    if(request)PerformanceMemoryTestAccess::Publish(renderer.GetBufferCache(),request);
    renderer.GetGpu().SendCommandSync([&] {
      auto& cache=renderer.GetBufferCache();auto& scheduler=renderer.GetCommandScheduler();uint32_t bv=0u;
      no_unrelated_submit=scheduler.CurrentTick()==expected_open_tick&&!scheduler.IsFree(expected_open_tick);
      a_read=LibKernel::Memory::TryReadBacking(a,&backing_a,4u);
      b_pending_before_cleanup=LibKernel::Memory::TryReadBacking(b,&bv,4u)&&bv==old&&
          cache.HasGpuDirtyBytes(b,4u);
      // On RED a rejected queue plan leaves A old. Normal fallback is required for
      // cleanup and value controls, but its extra submission is not the cost oracle.
      if(!request)cache.ReadMemory(a,4u);
      scheduler.Finish();scheduler.WaitPriorityOperations(scheduler.CurrentTick()-1u);
      cache.ReadMemory(a,4u);cache.ReadMemory(b,4u);
      uint32_t av=0u;
      a_correct=LibKernel::Memory::TryReadBacking(a,&av,4u)&&av==a_value;
      b_correct=LibKernel::Memory::TryReadBacking(b,&backing_b,4u)&&backing_b==b_value;
      scheduler.Finish();scheduler.DrainPriorityOperations();
    });
    renderer.UnmapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    renderer.ShutdownGpu();LibKernel::Memory::InstallGpuResources(nullptr);
  }
  std::printf("[gpu] ReadbackQueueBootstrap setup=%u warmRejected=%u warmCorrect=%u "
      "nativeA=%08x before=%u queued=%u openTick=%llu noSubmit=%u backingA=%08x Bpending=%u finalA=%u finalB=%08x\n",
      setup,warm_rejected,warm_correct,native_a,before_request,queued,
      static_cast<unsigned long long>(expected_open_tick),no_unrelated_submit,backing_a,
      b_pending_before_cleanup,a_correct,backing_b);
  Require(name,"actual2queues, conservative warm fallback and all native numerical controls",
      setup&&warm_rejected&&warm_pending&&warm_correct&&same_owner&&before_request&&after_request&&
      a_correct&&b_correct&&native_a==a_value,
      "native/bootstrap/unknown-writer controls failed; not the queue-eligibility RED");
  Require(name,"old writer uses actual transfer without draining unrelated open work",
      queued&&no_unrelated_submit&&a_read&&backing_a==a_value&&b_pending_before_cleanup,
      "AGE-off side queue never bootstrapped or assigned an already completed writer the new open tick");
}
} // namespace
} // namespace Libs::Graphics
