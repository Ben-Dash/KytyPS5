// Include after ArmedReadbackWindowCostTests.inc; existing native producer/copy/access helpers.
// Test-only Vulkan dispatcher wrappers forward EXACTLY to original driver/hooks; no work skipped.
namespace Libs::Graphics {
namespace {
void ConfigureEagerReadbackAdjacentPagesCostTests() {
  ConfigureArmedReadbackWindowCostTests();
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  _putenv_s("KYTY_RECORD_THREAD","0");_putenv_s("KYTY_DEBUG_VK_TIME","");
#else
  setenv("KYTY_RECORD_THREAD","0",1);unsetenv("KYTY_DEBUG_VK_TIME");
#endif
}
struct EagerNativeBudget {
  uint32_t source_barriers=0,host_barriers=0,copy_calls=0,copy_regions=0;
  uint64_t copied_bytes=0;
  std::array<VkBuffer,2> sources{};VkBuffer download=VK_NULL_HANDLE;
  static inline thread_local EagerNativeBudget* active=nullptr;
  static inline PFN_vkCmdPipelineBarrier original_barrier=nullptr;
  static inline PFN_vkCmdCopyBuffer original_copy=nullptr;
  bool IsSource(VkBuffer handle) const {return handle==sources[0]||handle==sources[1];}
  static void VKAPI_PTR Barrier(VkCommandBuffer command,VkPipelineStageFlags src,VkPipelineStageFlags dst,
      VkDependencyFlags flags,uint32_t memory_count,const VkMemoryBarrier* memory,
      uint32_t buffer_count,const VkBufferMemoryBarrier* buffers,uint32_t image_count,const VkImageMemoryBarrier* images) {
    if(active!=nullptr)for(uint32_t i=0;i<buffer_count;++i) {
      const auto& b=buffers[i];
      if(active->IsSource(b.buffer)&&(b.dstAccessMask&VK_ACCESS_TRANSFER_READ_BIT)!=0&&
         (b.srcAccessMask&VK_ACCESS_MEMORY_WRITE_BIT)!=0)++active->source_barriers;
      if(b.buffer==active->download&&(b.srcAccessMask&VK_ACCESS_TRANSFER_WRITE_BIT)!=0&&
         (b.dstAccessMask&VK_ACCESS_HOST_READ_BIT)!=0)++active->host_barriers;
    }
    original_barrier(command,src,dst,flags,memory_count,memory,buffer_count,buffers,image_count,images);
  }
  static void VKAPI_PTR Copy(VkCommandBuffer command,VkBuffer source,VkBuffer target,uint32_t count,const VkBufferCopy* regions) {
    if(active!=nullptr&&active->IsSource(source)&&target==active->download) {
      ++active->copy_calls;active->copy_regions+=count;
      for(uint32_t i=0;i<count;++i)active->copied_bytes+=regions[i].size;
    }
    original_copy(command,source,target,count,regions);
  }
  struct Scope {
    explicit Scope(EagerNativeBudget& budget) {
      EXIT_IF(active!=nullptr||original_barrier!=nullptr||original_copy!=nullptr);
      original_barrier=VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier;
      original_copy=VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdCopyBuffer;
      EXIT_IF(original_barrier==nullptr||original_copy==nullptr);
      VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier=Barrier;
      VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdCopyBuffer=Copy;active=&budget;
    }
    ~Scope() {
      active=nullptr;VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier=original_barrier;
      VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdCopyBuffer=original_copy;
      original_barrier=nullptr;original_copy=nullptr;
    }
  };
};

struct EagerAdjacentResult {
  bool setup=false,owner_shape=false,old_protected=false,ready_policy=false;
  bool native_values=false,published_values=false,unhot_pending=false,unhot_unarmed=false;
  bool final_unhot=false,guards=false,clean=false,hot_armed_before_fence=false;
  EagerNativeBudget budget;
  uint64_t tokens_before=0,tokens_after=0,ring_before=0,ring_after=0;
  uint32_t native_a=0,native_b=0,native_sibling=0,native_c=0,backing_a=0,backing_b=0,backing_sibling=0,backing_c=0;
};

void CheckEagerReadbackAdjacentPagesCost(VulkanHarness& vulkan) {
  constexpr const char* name="EagerReadbackAdjacentPagesCost";
  constexpr uint32_t old=0x5a5a5a5au,value_a=0x12345678u,value_b=0xabcdef09u,value_c=0x76543210u,sibling=0x2468ace0u;
  constexpr std::array names{"adjacent-same-owner","unhot-gap","adjacent-other-owner"};
  // A/B are selected HOT pages; C is unselected dirty, including the GAP negative.
  constexpr std::array<uint64_t,3> a_offsets{0x5000u,0x5000u,0x7000u};
  constexpr std::array<uint64_t,3> b_offsets{0x6000u,0x7000u,0x8000u};
  constexpr std::array<uint64_t,3> c_offsets{0x7000u,0x6000u,0xa000u};
  std::array<EagerAdjacentResult,3> results{};
  namespace Exception=Common::HostException;
  Require(name,"existing real fault route",Exception::InstallHandler([](const Exception::ExceptionInfo& i) {
    if(i.type!=Exception::ExceptionType::AccessViolation||
       (i.access_violation_type!=Exception::AccessViolationType::Read&&
        i.access_violation_type!=Exception::AccessViolationType::Write))return false;
    return LibKernel::Memory::HandleGpuFault(
      i.access_violation_type==Exception::AccessViolationType::Write?PageFaultAccess::Write:PageFaultAccess::Read,
      i.access_violation_vaddr);
  }),"fault route bootstrap failed; NOT cost RED");
  {
    FetchGpuGuest guest;auto& graphics=vulkan.RuntimeContext();auto& renderer=vulkan.RuntimeRenderer();
    renderer.InitializeGpu(nullptr);LibKernel::Memory::InstallGpuResources(&renderer);
    renderer.MapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    auto& gpu=renderer.GetGpu();auto& cache=renderer.GetBufferCache();auto& scheduler=renderer.GetCommandScheduler();
    FetchGpuBuffer a_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer b_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer c_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer sibling_source(graphics,4u,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer proof(graphics,16u,vk::BufferUsageFlagBits::eTransferDst);
    a_source.Flush();b_source.Flush();c_source.Flush();sibling_source.Flush();proof.Flush();
    EopComputeProducer a_compute(vulkan.Device(),a_source.buffer,value_a);
    EopComputeProducer b_compute(vulkan.Device(),b_source.buffer,value_b);
    EopComputeProducer c_compute(vulkan.Device(),c_source.buffer,value_c);
    EopComputeProducer sibling_compute(vulkan.Device(),sibling_source.buffer,sibling);
    std::unique_ptr<CommandProcessor> cp;
    gpu.SendCommandSync([&]{cp=std::make_unique<CommandProcessor>(renderer,0x31);cp->Reset();cp->BufferInit();});
    for(uint32_t test=0;test<results.size();++test) {
      auto& out=results[test];const auto a=FetchGpuGuest::Base+a_offsets[test];
      const auto b=FetchGpuGuest::Base+b_offsets[test],c=FetchGpuGuest::Base+c_offsets[test],s=a+0x40u;
      for(const auto p:{a-4u,a,a+4u,b-4u,b,b+4u,c-4u,c,c+4u,s-4u,s,s+4u})
        std::memcpy(reinterpret_cast<void*>(p),&old,4u);
      gpu.SendCommandSync([&] {
        // Policy metadata only; all preceding test's producers/publications are retired.
        PerformanceMemoryTestAccess::ResetEagerHotPolicy(cache);
        PerformanceMemoryTestAccess::MarkHot(cache,a);PerformanceMemoryTestAccess::MarkHot(cache,b);
        out.setup=GuestGpu::IsGpuThread()&&!CommandRecordingDeferred()&&
            !PerformanceMemoryTestAccess::HasQueue(cache)&&graphics.readback_queue==nullptr&&!ReadbackAgeEnabled();
        ReadbackBootstrapWrite(scheduler,cache,a_compute,a_source,a);
        ReadbackBootstrapWrite(scheduler,cache,sibling_compute,sibling_source,s);
        ReadbackBootstrapWrite(scheduler,cache,b_compute,b_source,b);
        ReadbackBootstrapWrite(scheduler,cache,c_compute,c_source,c);
        auto [ao,ap]=cache.ObtainBuffer(a,4u,false,true);auto [bo,bp]=cache.ObtainBuffer(b,4u,false,true);
        auto [so,sp]=cache.ObtainBuffer(s,4u,false,true);auto [co,cp_offset]=cache.ObtainBuffer(c,4u,false,true);
        out.owner_shape=(ao->Handle()==bo->Handle())==(test!=2u)&&so->Handle()==ao->Handle();
        uint32_t av=0,bv=0,cv=0,sv=0;
        const auto as=PerformanceMemoryTestAccess::Readback(cache,a,4u),bs=PerformanceMemoryTestAccess::Readback(cache,b,4u);
        out.old_protected=as.gpu_dirty&&as.unarmed&&bs.gpu_dirty&&bs.unarmed&&
          LibKernel::Memory::TryReadBacking(a,&av,4u)&&av==old&&LibKernel::Memory::TryReadBacking(b,&bv,4u)&&bv==old&&
          LibKernel::Memory::TryReadBacking(c,&cv,4u)&&cv==old&&LibKernel::Memory::TryReadBacking(s,&sv,4u)&&sv==old&&
          !LibKernel::Memory::TryReadGpuCleanBacking(a,&av,4u)&&!LibKernel::Memory::TryReadGpuCleanBacking(b,&bv,4u);
        out.ready_policy=PerformanceMemoryTestAccess::EagerReadyContainsOnly(cache,a,b);
        out.budget.sources={static_cast<VkBuffer>(ao->Handle()),static_cast<VkBuffer>(bo->Handle())};
        out.budget.download=static_cast<VkBuffer>(cache.GetUtilityBuffer(MemoryUsage::Download).Handle());
        out.tokens_before=PerformanceMemoryTestAccess::ReadbackToken(cache);
        out.ring_before=StreamBufferTestAccess::Offset(cache.GetUtilityBuffer(MemoryUsage::Download));
        {
          EagerNativeBudget::Scope observe(out.budget);
          // Public boundary used by SuspendPoint; records eager readbacks then real Submit.
          cp->BufferFlush();
        }
        out.tokens_after=PerformanceMemoryTestAccess::ReadbackToken(cache);
        out.ring_after=StreamBufferTestAccess::Offset(cache.GetUtilityBuffer(MemoryUsage::Download));
        const auto ar=PerformanceMemoryTestAccess::Readback(cache,a,4u),br=PerformanceMemoryTestAccess::Readback(cache,b,4u);
        out.hot_armed_before_fence=ar.gpu_dirty&&!ar.unarmed&&br.gpu_dirty&&!br.unarmed;
        // Same real native queue order/fence; publication may already have finished on a fast device.
        // Values and canaries, not that race, are the semantic controls.
        CopyEopNativeLabel(scheduler,*ao,ap,proof,0u,4u);
        CopyEopNativeLabel(scheduler,*bo,bp,proof,4u,4u);
        CopyEopNativeLabel(scheduler,*so,sp,proof,8u,4u);
        CopyEopNativeLabel(scheduler,*co,cp_offset,proof,12u,4u);
        scheduler.Finish();scheduler.WaitPriorityOperations(scheduler.CurrentTick()-1u);proof.Invalidate();
        std::memcpy(&out.native_a,proof.data,4u);std::memcpy(&out.native_b,static_cast<uint8_t*>(proof.data)+4u,4u);
        std::memcpy(&out.native_sibling,static_cast<uint8_t*>(proof.data)+8u,4u);
        std::memcpy(&out.native_c,static_cast<uint8_t*>(proof.data)+12u,4u);
        out.native_values=out.native_a==value_a&&out.native_b==value_b&&out.native_sibling==sibling&&out.native_c==value_c;
        out.published_values=LibKernel::Memory::TryReadBacking(a,&out.backing_a,4u)&&out.backing_a==value_a&&
          LibKernel::Memory::TryReadBacking(b,&out.backing_b,4u)&&out.backing_b==value_b&&
          LibKernel::Memory::TryReadBacking(s,&out.backing_sibling,4u)&&out.backing_sibling==sibling;
        const auto cs=PerformanceMemoryTestAccess::Readback(cache,c,4u);uint32_t c_old=0;
        out.unhot_pending=cs.gpu_dirty&&LibKernel::Memory::TryReadBacking(c,&c_old,4u)&&c_old==old;
        out.unhot_unarmed=cs.unarmed;
        out.guards=true;
        for(const auto p:{a-4u,a+4u,b-4u,b+4u,c-4u,c+4u,s-4u,s+4u}) {
          uint32_t actual=0;out.guards=out.guards&&LibKernel::Memory::TryReadBacking(p,&actual,4u)&&actual==old;
        }
        cache.ReadMemory(c,4u);
        out.final_unhot=LibKernel::Memory::TryReadBacking(c,&out.backing_c,4u)&&out.backing_c==value_c;
        out.clean=!PerformanceMemoryTestAccess::Readback(cache,a,4u).gpu_dirty&&
          !PerformanceMemoryTestAccess::Readback(cache,b,4u).gpu_dirty&&!PerformanceMemoryTestAccess::Readback(cache,c,4u).gpu_dirty;
        scheduler.Finish();scheduler.DrainPriorityOperations();
      });
    }
    renderer.UnmapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    renderer.ShutdownGpu();LibKernel::Memory::InstallGpuResources(nullptr);cp.reset();
  }
  for(uint32_t test=0;test<results.size();++test) {
    const auto& r=results[test];
    std::printf("[eager-adjacent] %s setup=%u owner=%u oldProtected=%u policy=%u ring=%llu->%llu tokens=%llu->%llu src/host/copies/regions=%u/%u/%u/%u bytes=%llu native=%08x/%08x/%08x/%08x CPU=%08x/%08x/%08x unhot=%u/%u finalC=%08x guards=%u clean=%u\n",
      names[test],r.setup,r.owner_shape,r.old_protected,r.ready_policy,
      static_cast<unsigned long long>(r.ring_before),static_cast<unsigned long long>(r.ring_after),
      static_cast<unsigned long long>(r.tokens_before),static_cast<unsigned long long>(r.tokens_after),
      r.budget.source_barriers,r.budget.host_barriers,r.budget.copy_calls,r.budget.copy_regions,
      static_cast<unsigned long long>(r.budget.copied_bytes),r.native_a,r.native_b,r.native_sibling,r.native_c,
      r.backing_a,r.backing_b,r.backing_sibling,r.unhot_pending,r.unhot_unarmed,r.backing_c,r.guards,r.clean);
    Require(names[test],"all real producer/oldprotected/native/publication/gap/guard/cleanup controls BEFORE cost",
      r.setup&&r.owner_shape&&r.old_protected&&r.ready_policy&&r.native_values&&r.published_values&&
      r.unhot_pending&&r.unhot_unarmed&&r.final_unhot&&r.guards&&r.clean&&
      r.budget.copy_regions==3u&&r.budget.copied_bytes==12u,
      "native/owner/policy/value/gap/guard/cleanup control failure; NOT redundant-batch RED");
  }
  for(uint32_t test=0;test<results.size();++test) {
    const auto& r=results[test];const uint32_t expected=test==0u?1u:2u;
    Require(names[test],"one native eager batch ONLY for consecutive selected pages in same complete owner",
      r.tokens_after-r.tokens_before==expected&&r.budget.source_barriers==expected&&
      r.budget.host_barriers==expected&&r.budget.copy_calls==expected,
      "adjacent same-owner pages were individually synchronized/published, or a gap/other owner was incorrectly merged");
  }
}
} // namespace
} // namespace Libs::Graphics
