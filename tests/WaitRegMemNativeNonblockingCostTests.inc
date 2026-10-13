// Include AFTER LiteralEopPublicationCostTests.inc: existing real GPU producer and PM4 helper.
#include <condition_variable>
#include <mutex>
#include <thread>
namespace Libs::Graphics {
namespace {
struct WaitRegMemWaitBudget {
  uint32_t full=0,tick=0,priority=0;
  static inline thread_local WaitRegMemWaitBudget* active=nullptr;
  static void Observe(DrainStats::Kind kind) {
    if(active==nullptr)return;
    if(kind==DrainStats::Kind::FullDrain)++active->full;
    if(kind==DrainStats::Kind::TickWait)++active->tick;
    if(kind==DrainStats::Kind::PriorityWait)++active->priority;
  }
};

void CheckWaitRegMemNativeNonblockingCost(VulkanHarness& vulkan) {
  constexpr const char* name="WaitRegMemNativeNonblockingCost";
  constexpr uint64_t base=0x00000002c2400000ull,bytes=0x40000,flag_address=base+0x10000;
  constexpr uint64_t done_a=base+0x20000,done_b=base+0x20004;
  constexpr uint32_t value=0x5145a7c3u,guard=0x5a5a5a5au;
  Require(name,"real GPU dispatcher setup",GuestGpu::IsGpuThread() && !CommandRecordingDeferred(),
      "execute on the existing GPU worker with direct recording; setup failure is not RED");
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
  std::memset(mapped,0x5a,bytes);
  *reinterpret_cast<uint32_t*>(flag_address)=0;
  *reinterpret_cast<uint32_t*>(done_a)=*reinterpret_cast<uint32_t*>(done_b)=0;
  auto& renderer=vulkan.RuntimeRenderer();auto& scheduler=renderer.GetCommandScheduler();
  auto& cache=renderer.GetBufferCache();
  HW::Context registers{};HW::UserConfig user_config{};HW::Shader shaders{};
  scheduler.Begin(registers,user_config,shaders);
  LibKernel::Memory::InstallGpuResources(&renderer);renderer.MapMemory(base,bytes);
  // A written binding must be the registered owner; clean reads may use the upload stream.
  auto [owner,offset]=cache.ObtainBuffer(flag_address,4,true,true);
  Require(name,"real native scalar owner setup",owner!=nullptr && offset==0,
      "existing finite producer uses the first real owner word; unexpected fixture offset is not RED");
  EopComputeProducer producer(vulkan.Device(),owner->Handle(),value);
  auto command=scheduler.Current().Handle();
  vk::BufferMemoryBarrier2 before{};
  before.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands|vk::PipelineStageFlagBits2::eHost;
  before.srcAccessMask=vk::AccessFlagBits2::eMemoryWrite|vk::AccessFlagBits2::eHostWrite;
  before.dstStageMask=vk::PipelineStageFlagBits2::eComputeShader;
  before.dstAccessMask=vk::AccessFlagBits2::eShaderWrite;
  before.srcQueueFamilyIndex=before.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
  before.buffer=owner->Handle();before.offset=0;before.size=4;
  vk::DependencyInfo dependency{};dependency.bufferMemoryBarrierCount=1;dependency.pBufferMemoryBarriers=&before;
  command.pipelineBarrier2(dependency);producer.Record(command);cache.OnCommandRecorded();
  uint32_t cpu_before=UINT32_MAX;
  Require(name,"GPU owned and CPU value still old setup",cache.IsRegionGpuModified(flag_address,4) &&
      LibKernel::Memory::TryReadBacking(flag_address,&cpu_before,4) && cpu_before==0,
      "producer did not write a protected GPU-owned word independently of CPU backing");

  // A legal bounded timeline dependency, not a shader polling loop. The helper
  // always signals within1second, even when the OLD GPU dispatcher synchronously waits.
  vk::SemaphoreTypeCreateInfo type{};type.semaphoreType=vk::SemaphoreType::eTimeline;
  vk::SemaphoreCreateInfo create{};create.pNext=&type;vk::Semaphore gate=nullptr;
  RequireVulkanSuccess(vulkan.Device().createSemaphore(&create,nullptr,&gate),"bounded native producer gate");
  SubmitInfo submission{};submission.AddWait(gate,1);scheduler.Flush(submission);
  std::mutex mutex;std::condition_variable available;
  bool first_calls_returned=false,signal_failed=false;
  std::jthread release_gate([&] {
    {std::unique_lock lock(mutex);available.wait_for(lock,std::chrono::seconds(1),[&]{return first_calls_returned;});}
    vk::SemaphoreSignalInfo signal{};signal.semaphore=gate;signal.value=1;
    signal_failed=vulkan.Device().signalSemaphore(&signal)!=vk::Result::eSuccess;
  });
  GraphicsInitJmpTables();
  CommandProcessor a(renderer,0),b(renderer,0);a.Reset();b.Reset();a.BufferInit();
  const auto commands_a=LiteralCostWaitThenDone(flag_address,4,value,UINT32_MAX,done_a,11);
  const auto commands_b=LiteralCostWaitThenDone(flag_address,4,value,UINT32_MAX,done_b,22);
  Pm4Execution execution_a,execution_b;
  WaitRegMemWaitBudget budget;
  WaitRegMemWaitBudget::active=&budget;
  auto previous=DrainStats::g_wait_timer_test_observer;
  DrainStats::g_wait_timer_test_observer=WaitRegMemWaitBudget::Observe;
  auto result_a=a.Process(execution_a,commands_a);
  auto result_b=b.Process(execution_b,commands_b);
  const bool both_pending=result_a==Pm4ProcessResult::Blocked && result_b==Pm4ProcessResult::Blocked;
  {std::lock_guard lock(mutex);first_calls_returned=true;}available.notify_one();
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  while((result_a!=Pm4ProcessResult::Complete || result_b!=Pm4ProcessResult::Complete) &&
        std::chrono::steady_clock::now()<deadline) {
    // No draw/new submission is needed for priority publication progress when
    // BOTH parser executions are blocked. The production publisher owns its wait.
    if(result_a!=Pm4ProcessResult::Complete)result_a=a.Process(execution_a,commands_a);
    if(result_b!=Pm4ProcessResult::Complete)result_b=b.Process(execution_b,commands_b);
    std::this_thread::yield();
  }
  DrainStats::g_wait_timer_test_observer=previous;WaitRegMemWaitBudget::active=nullptr;
  release_gate.join();
  scheduler.Finish();
  const uint32_t actual=*reinterpret_cast<const uint32_t*>(flag_address);
  const uint32_t marker_a=*reinterpret_cast<const uint32_t*>(done_a),marker_b=*reinterpret_cast<const uint32_t*>(done_b);
  const bool guards=*reinterpret_cast<const uint32_t*>(flag_address-4)==guard &&
                    *reinterpret_cast<const uint32_t*>(flag_address+4)==guard;
  const bool still_gpu_dirty=cache.IsRegionGpuModified(flag_address,4);
  vulkan.Device().destroySemaphore(gate,nullptr);
  renderer.UnmapMemory(base,bytes);LibKernel::Memory::InstallGpuResources(nullptr);
  Require(name,"owned mapping cleanup",LibKernel::Memory::KernelMunmap(base,bytes)==0 &&
      LibKernel::Memory::KernelReleaseDirectMemory(direct,bytes)==0,"native fixture mapping was not retired");
  std::printf("[wait-setup] signalFailed=%u resultA=%u resultB=%u actual=%08x expected=%08x markerA=%u markerB=%u guards=%u dirty=%u budget=%u/%u/%u\n",
      signal_failed?1u:0u,static_cast<unsigned>(result_a),static_cast<unsigned>(result_b),
      actual,value,marker_a,marker_b,guards?1u:0u,still_gpu_dirty?1u:0u,
      budget.full,budget.tick,budget.priority);
  Require(name,"actual native scalar and masked comparison",!signal_failed &&
      result_a==Pm4ProcessResult::Complete && result_b==Pm4ProcessResult::Complete &&
      actual==value && marker_a==11 && marker_b==22 && guards && !still_gpu_dirty,
      "native publication/comparison/markers/guard controls failed BEFORE intended wait cost RED");
  std::printf("[wait-cost] nativeWord=PASS markers=11/22 guards=PASS publication=PASS bothPending=%u dispatcher full/tick/priority=%u/%u/%u cleanup=PASS\n",
      both_pending?1u:0u,budget.full,budget.tick,budget.priority);
  Require(name,"avoidable GPU dispatcher host synchronization",budget.full+budget.tick+budget.priority==0,
      "protected WAIT_REG_MEM entered a synchronous GPU-dispatcher wait instead of yielding until actual publication");
  Require(name,"both pending parsers complete without new draw",both_pending,
      "bounded producer gate did not exercise both WAIT executions pending together");
}
} // namespace
} // namespace Libs::Graphics
