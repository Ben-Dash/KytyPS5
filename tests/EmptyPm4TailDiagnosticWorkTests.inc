// Include after EmptyPm4TailSubmitCostTests.inc. Actual automatic Begin timestamps, not injected work flags.
namespace Libs::Graphics {
namespace {
void ConfigureEmptyPm4TailDiagnosticWorkTests() {
  ConfigureEmptyPm4TailSubmitCostTests();
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  _putenv_s("KYTY_GPU_ZONES","0");
#else
  setenv("KYTY_GPU_ZONES","0",1);
#endif
}
struct EmptyTailQueryBudget {
  VkQueryPool pool=VK_NULL_HANDLE;uint32_t first=UINT32_MAX;
  uint32_t resets=0,top=0,bottom=0,retired=0;uint64_t start=0,end=0;
  static inline EmptyTailQueryBudget* active=nullptr;
  static inline PFN_vkCmdResetQueryPool original_reset=nullptr;
  static inline PFN_vkCmdWriteTimestamp original_write=nullptr;
  static inline PFN_vkGetQueryPoolResults original_results=nullptr;
  static void VKAPI_PTR Reset(VkCommandBuffer command,VkQueryPool pool,uint32_t first,uint32_t count) {
    auto* b=active;
    if(b!=nullptr&&count==2u) {
      if(b->pool==VK_NULL_HANDLE){b->pool=pool;b->first=first;}
      if(pool==b->pool&&first==b->first)++b->resets;
    }
    original_reset(command,pool,first,count);
  }
  static void VKAPI_PTR Write(VkCommandBuffer command,VkPipelineStageFlagBits stage,VkQueryPool pool,uint32_t query) {
    auto* b=active;
    if(b!=nullptr&&pool==b->pool) {
      if(query==b->first&&stage==VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT)++b->top;
      if(query==b->first+1u&&stage==VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT)++b->bottom;
    }
    original_write(command,stage,pool,query);
  }
  static VkResult VKAPI_PTR Results(VkDevice device,VkQueryPool pool,uint32_t first,uint32_t count,
      size_t bytes,void* data,VkDeviceSize stride,VkQueryResultFlags flags) {
    const auto result=original_results(device,pool,first,count,bytes,data,stride,flags);
    auto* b=active;
    if(b!=nullptr&&pool==b->pool&&first==b->first&&count==2u&&
       result==VK_SUCCESS&&(flags&VK_QUERY_RESULT_64_BIT)!=0&&stride==8u&&bytes>=16u&&data!=nullptr) {
      std::memcpy(&b->start,data,8u);std::memcpy(&b->end,static_cast<const uint8_t*>(data)+8u,8u);++b->retired;
    }
    return result;
  }
  struct Scope {
    explicit Scope(EmptyTailQueryBudget& b) {
      EXIT_IF(active!=nullptr||original_reset!=nullptr||original_write!=nullptr||original_results!=nullptr);
      original_reset=VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdResetQueryPool;
      original_write=VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdWriteTimestamp;
      original_results=VULKAN_HPP_DEFAULT_DISPATCHER.vkGetQueryPoolResults;
      EXIT_IF(original_reset==nullptr||original_write==nullptr||original_results==nullptr);
      VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdResetQueryPool=Reset;
      VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdWriteTimestamp=Write;
      VULKAN_HPP_DEFAULT_DISPATCHER.vkGetQueryPoolResults=Results;active=&b;
    }
    ~Scope() {
      active=nullptr;VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdResetQueryPool=original_reset;
      VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdWriteTimestamp=original_write;
      VULKAN_HPP_DEFAULT_DISPATCHER.vkGetQueryPoolResults=original_results;
      original_reset=nullptr;original_write=nullptr;original_results=nullptr;
    }
  };
};
void CheckEmptyPm4TailDiagnosticWork(VulkanHarness& vulkan) {
  constexpr const char* name="EmptyPm4TailDiagnosticWork";
  auto& graphics=vulkan.RuntimeContext();auto& renderer=vulkan.RuntimeRenderer();
  const auto families=graphics.physical_device.getQueueFamilyProperties();
  Require(name,"actual graphics queue supports timestamps",graphics.queue_family<families.size()&&
    families[graphics.queue_family].timestampValidBits!=0u&&graphics.GetPhysicalDeviceProperties().limits.timestampPeriod>0.0f,
    "native timestamp precondition unavailable; not optional-submit reproduction");
  const bool enabled_before=DrainStats::g_enabled.exchange(true,std::memory_order_relaxed);
  renderer.InitializeGpu(nullptr);auto& gpu=renderer.GetGpu();auto& scheduler=renderer.GetCommandScheduler();
  std::unique_ptr<CommandProcessor> cp;bool setup=false,automatic=false,submitted=false,free=false;
  uint64_t before=0,after=0;uint32_t calls=0,buffers=0,failed=0;
  EmptyTailSubmitBudget submits;submits.queue=static_cast<VkQueue>(graphics.queue);EmptyTailQueryBudget queries;
  {
    EmptyTailQueryBudget::Scope query_observe(queries);EmptyTailSubmitBudget::Scope submit_observe(submits);
    gpu.SendCommandSync([&] {
      cp=std::make_unique<CommandProcessor>(renderer,0x31);cp->Reset();cp->BufferInit();
      // No public CommandBuffer handle, fill/copy/draw/dispatch or user callback has been requested.
      setup=GuestGpu::IsGpuThread()&&!CommandRecordingDeferred()&&Config::AsyncSubmitEnabled()&&DrainStats::Enabled();
      automatic=queries.pool!=VK_NULL_HANDLE&&queries.first!=UINT32_MAX&&queries.resets==1u&&queries.top==1u&&queries.bottom==0u;
      before=scheduler.CurrentTick();cp->BufferFlush();after=scheduler.CurrentTick();submitted=after==before+1u;
      if(!submitted) {
        // Explicit mandatory cleanup only: losing an automatic query must fail, not hang.
        EmptyTailSubmitBudget::active.store(nullptr,std::memory_order_release);scheduler.Flush();
      }
      scheduler.GetMasterSemaphore().Wait(scheduler.CurrentTick()-1u);
      scheduler.WaitPriorityOperations(scheduler.CurrentTick()-1u);scheduler.PopPendingOperations();
      free=scheduler.GetMasterSemaphore().IsFree(before);
    });
    calls=submits.calls.load();buffers=submits.buffers.load();failed=submits.failed.load();
  }
  renderer.ShutdownGpu();cp.reset();DrainStats::g_enabled.store(enabled_before,std::memory_order_relaxed);
  std::printf("[empty-tail-diagnostic] setup=%u automaticBegin=%u submitted=%u tick=%llu->%llu nativeSubmits=%u buffers=%u failures=%u reset=%u top=%u bottom=%u retired=%u free=%u timestamps=%llu/%llu\n",
    setup,automatic,submitted,static_cast<unsigned long long>(before),static_cast<unsigned long long>(after),calls,buffers,failed,
    queries.resets,queries.top,queries.bottom,queries.retired,free,static_cast<unsigned long long>(queries.start),static_cast<unsigned long long>(queries.end));
  Require(name,"automatic native Begin query, real retirement and cleanup BEFORE required-submit invariant",
    setup&&automatic&&failed==0u&&free&&queries.resets==1u&&queries.top==1u&&queries.bottom==1u&&queries.retired==1u&&queries.end>=queries.start,
    "automatic native reset/start/end query or native retirement failed");
  Require(name,"diagnostic-only current buffer remains submitted at CP boundary",
    submitted&&after==before+1u&&calls==1u&&buffers==1u,
    "optional empty classification skipped actual Begin timestamp commands; explicit fallback is not success");
}
} // namespace
} // namespace Libs::Graphics
