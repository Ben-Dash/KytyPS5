// Include AFTER EndOfPipeLiteralNeighborTests.inc in
// BindlessLifecycleTests.cpp (not ShaderRecompilerComputeTests.cpp).
// All native work is finite; every assertion runs after normal cleanup.
#include "graphics/guest_gpu/command_processor/pm4Dispatch.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "common/hostException.h"

namespace Libs::Graphics {
struct StreamBufferTestAccess {
  static uint64_t Offset(const StreamBuffer& buffer) { return buffer.m_offset; }
};
namespace {

void LiteralCostCopyUnknown(CommandScheduler& scheduler, BufferCache& cache,
                            FetchGpuBuffer& producer, uint64_t destination) {
  auto [owner, offset] = cache.ObtainBuffer(destination, 4u, true, true);
  const auto command = scheduler.Current().Handle();
  vk::MemoryBarrier before{};
  before.srcAccessMask = vk::AccessFlagBits::eMemoryWrite;
  before.dstAccessMask = vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite;
  command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
      vk::PipelineStageFlagBits::eTransfer, {}, 1u, &before, 0u, nullptr, 0u, nullptr);
  const vk::BufferCopy copy{0u, offset, 4u};
  command.copyBuffer(producer.buffer, owner->Handle(), 1u, &copy);
  cache.OnCommandRecorded();
}

std::vector<uint32_t> LiteralCostWaitThenDone(uint64_t label, uint64_t width,
                                            uint64_t reference, uint64_t mask,
                                            uint64_t done, uint32_t marker) {
  std::vector<uint32_t> commands;
  commands.push_back(KYTY_PM4(width == 4u ? 7u : 9u,
      width == 4u ? Pm4::IT_WAIT_REG_MEM : Pm4::IT_WAIT_REG_MEM_64, 0));
  commands.push_back(0x10u | 3u); // Memory-space equality, ordinary wait operation.
  commands.push_back(static_cast<uint32_t>(label));
  commands.push_back(static_cast<uint32_t>(label >> 32u));
  commands.push_back(static_cast<uint32_t>(reference));
  if (width == 8u) commands.push_back(static_cast<uint32_t>(reference >> 32u));
  commands.push_back(static_cast<uint32_t>(mask));
  if (width == 8u) commands.push_back(static_cast<uint32_t>(mask >> 32u));
  commands.push_back(0u);
  const std::array<uint32_t, 5> store{KYTY_PM4(5, Pm4::IT_WRITE_DATA, 0), 0u,
      static_cast<uint32_t>(done), static_cast<uint32_t>(done >> 32u), marker};
  commands.insert(commands.end(), store.begin(), store.end());
  return commands;
}

void CheckLiteralEopPublicationCost(VulkanHarness& vulkan) {
  constexpr const char* name = "LiteralEopPublicationCost";
  namespace Exception = Common::HostException;
  Require(name, "production guest-memory fault route",
      Exception::InstallHandler([](const Exception::ExceptionInfo& info) {
        if (info.type != Exception::ExceptionType::AccessViolation ||
            (info.access_violation_type != Exception::AccessViolationType::Read &&
             info.access_violation_type != Exception::AccessViolationType::Write)) return false;
        const auto access = info.access_violation_type == Exception::AccessViolationType::Write
            ? PageFaultAccess::Write : PageFaultAccess::Read;
        return LibKernel::Memory::HandleGpuFault(access, info.access_violation_vaddr);
      }), "could not install the existing protected-memory fault handler");
  constexpr uint64_t sentinel = 0x8899aabbccddeef0ull;
  constexpr uint64_t literal = 0x1122334455667788ull;
  constexpr uint64_t repeated = 0xa5b6c7d8e9fa0b1cull;
  constexpr uint64_t cpu_value = 0x76543210abcdef09ull;
  constexpr uint32_t sibling_sentinel = 0xd00df00du;
  constexpr uint32_t done_marker = 0x7a6b5c4du;
  struct Result {
    bool actual_gpu_thread = false, setup = false, pending = false;
    bool byte_pending = false, clean_read_refused = false;
    bool before_callback = false, label_old_at_callback = false, done_old_at_callback = false;
    bool older_kept_new_dirty = true, cpu_overwrite_after_fence = true;
    bool producer_complete = false, final_read = false, sibling_read = false;
    bool wait_complete = false, wait_blocked = false, consumer_after_fence = false;
    bool done_read = false, clean = false, irq_setup = true;
    uint64_t reservation_before = 0u, reservation_after = 0u;
    uint64_t first_native = 0u, second_native = 0u, final_backing = 0u;
    uint32_t sibling = 0u, done = 0u, producer = 0u;
    EopIrqObserver irq{};
    int irq_status = 0, irq_count = 0;
  };
  std::array<Result, 8> results{};
  GraphicsInitJmpTables();
  {
    FetchGpuGuest guest;
    auto& renderer = vulkan.RuntimeRenderer();
    renderer.InitializeGpu(nullptr);
    LibKernel::Memory::InstallGpuResources(&renderer);
    renderer.MapMemory(FetchGpuGuest::Base, FetchGpuGuest::Bytes);
    for (uint32_t test = 0u; test < results.size(); ++test) {
      const bool mixed = test == 2u, rewrite = test == 3u, cpu_overwrite = test == 4u;
      const bool repeated_store = test == 5u, false_wait = test == 6u, partial_mask = test == 7u;
      const uint64_t width = test == 1u || false_wait ? 4u : 8u;
      const uint64_t address = FetchGpuGuest::Base + test * 0x1000u;
      const uint64_t sibling_address = address + 0x40u;
      const uint64_t done_address = FetchGpuGuest::Base + 0xe000u;
      const uint32_t gpu_marker = 0xcdef4000u + test;
      std::memcpy(reinterpret_cast<void*>(address), &sentinel, 8u);
      std::memcpy(reinterpret_cast<void*>(sibling_address), &sibling_sentinel, 4u);
      const uint32_t zero = 0u;
      std::memcpy(reinterpret_cast<void*>(done_address), &zero, 4u);
      FetchGpuBuffer producer(vulkan.RuntimeContext(), 4u,
          vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc);
      FetchGpuBuffer readback(vulkan.RuntimeContext(), 16u, vk::BufferUsageFlagBits::eTransferDst);
      producer.Flush(); readback.Flush();
      EopComputeProducer compute(vulkan.Device(), producer.buffer, gpu_marker);
      renderer.GetGpu().SendCommandSync([&] {
        auto& result = results[test];
        auto& cache = renderer.GetBufferCache();
        auto& scheduler = renderer.GetCommandScheduler();
        CommandProcessor cp(renderer, 0x31); cp.Reset(); cp.BufferInit();
        result.actual_gpu_thread = GuestGpu::IsGpuThread();
        const auto registered = cache.FindBuffer(address, 8u);
        cache.SynchronizeBuffersInRange(address, 0x44u);
        result.setup = cache.IsRegionRegistered(address, width) &&
            !cache.HasGpuDirtyBytes(address, width);
        (void)registered;
        const auto command = scheduler.Current().Handle();
        vk::MemoryBarrier ready{};
        ready.srcAccessMask = vk::AccessFlagBits::eHostWrite;
        ready.dstAccessMask = vk::AccessFlagBits::eShaderWrite;
        command.pipelineBarrier(vk::PipelineStageFlagBits::eHost,
            vk::PipelineStageFlagBits::eComputeShader, {}, 1u, &ready, 0u, nullptr, 0u, nullptr);
        compute.Record(command); // Actual finite shader, never a CPU-produced witness.
        if (mixed) LiteralCostCopyUnknown(scheduler, cache, producer, sibling_address);
        const auto tick = scheduler.CurrentTick();
        // Observe the consumer marker before actual EOP backing publication.
        // A future fake known-value WAIT that runs host WRITE_DATA early fails this.
        scheduler.DeferPriorityOperation([&] {
          uint64_t label_before = 0u; uint32_t done_before = 0u;
          result.before_callback = true;
          result.label_old_at_callback = LibKernel::Memory::TryReadBacking(
              address, &label_before, width) && label_before ==
              (width == 4u ? static_cast<uint32_t>(sentinel) : sentinel);
          result.done_old_at_callback = LibKernel::Memory::TryReadBacking(
              done_address, &done_before, 4u) && done_before == 0u;
        });
        LibKernel::EventQueue::KernelEqueue queue = LibKernel::EventQueue::KERNEL_EQUEUE_INVALID;
        const bool interrupt = mixed || repeated_store;
        result.irq = EopIrqObserver{address, literal};
        if (interrupt) {
          result.irq_setup = LibKernel::EventQueue::KernelCreateEqueue(&queue, "literal-cost") == 0;
          LibKernel::EventQueue::KernelEqueueEvent event{};
          event.event.ident = 0x31u; event.event.filter = LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS;
          event.event.udata = &result.irq;
          event.filter.trigger_func = ObserveEopIrq; event.filter.reset_func = ResetEopIrq;
          result.irq_setup = result.irq_setup && LibKernel::EventQueue::KernelAddEvent(queue, event) == 0;
          renderer.AddInterruptEq(queue, 0x31);
        }
        auto& ring = cache.GetUtilityBuffer(MemoryUsage::Download);
        result.reservation_before = StreamBufferTestAccess::Offset(ring);
        if (width == 4u) cp.WriteAtEndOfPipe32(0u, 0u, 0x2fu, 0u, 6u, 2u,
            reinterpret_cast<void*>(address), static_cast<uint32_t>(literal), 0u);
        else cp.WriteAtEndOfPipe64(0u, 0u, 0x28u, 0u, 0u, 2u,
            reinterpret_cast<void*>(address), literal, interrupt ? 2u : 0u);
        auto [first, first_offset] = cache.ObtainBuffer(address, width, false, true);
        CopyEopNativeLabel(scheduler, *first, first_offset, readback, 0u, width);
        uint64_t before = 0u; uint32_t done_before = 0u; uint64_t clean_value = 0u;
        result.pending = LibKernel::Memory::TryReadBacking(address, &before, width) &&
            before == (width == 4u ? static_cast<uint32_t>(sentinel) : sentinel) &&
            LibKernel::Memory::TryReadBacking(done_address, &done_before, 4u) && done_before == 0u &&
            scheduler.CurrentTick() == tick && !scheduler.IsFree(tick) && !result.irq.called;
        result.byte_pending = cache.HasGpuDirtyBytes(address, width);
        result.clean_read_refused = !LibKernel::Memory::TryReadGpuCleanBacking(
            address, &clean_value, width);
        uint64_t final_expected = width == 4u ? static_cast<uint32_t>(literal) : literal;
        if (rewrite || repeated_store) {
          // This is thread-safe page state, not GPU-thread-only byte-set pruning.
          scheduler.DeferPriorityOperation([&] {
            result.older_kept_new_dirty = cache.IsRegionGpuModified(address, width);
          });
          if (rewrite) {
            LiteralCostCopyUnknown(scheduler, cache, producer, address);
            final_expected = (literal & 0xffffffff00000000ull) | gpu_marker;
          } else {
            cp.WriteAtEndOfPipe64(0u, 0u, 0x28u, 0u, 0u, 2u,
                reinterpret_cast<void*>(address), repeated, 0u);
            final_expected = repeated;
          }
          auto [second, second_offset] = cache.ObtainBuffer(address, width, false, true);
          CopyEopNativeLabel(scheduler, *second, second_offset, readback, 8u, width);
        }
        // Capture cost before any required WAIT/CPU-overwrite readback. Only the
        // known literal store(s) are under the zero-download reservation budget.
        result.reservation_after = StreamBufferTestAccess::Offset(ring);
        if (cpu_overwrite) {
          *reinterpret_cast<volatile uint64_t*>(address) = cpu_value;
          result.cpu_overwrite_after_fence = scheduler.IsFree(tick);
          final_expected = cpu_value;
          auto [second, second_offset] = cache.ObtainBuffer(address, 8u, false, true);
          CopyEopNativeLabel(scheduler, *second, second_offset, readback, 8u, 8u);
        }
        const uint64_t mask = partial_mask ? 0x0ff00000ff00f00full :
            width == 4u ? uint64_t{UINT32_MAX} : UINT64_MAX;
        const uint64_t reference = (final_expected & mask) ^ (false_wait ? 1u : 0u);
        const auto commands = LiteralCostWaitThenDone(address, width, reference, mask,
            done_address, done_marker);
        Pm4Execution execution;
        auto outcome = cp.Process(execution, commands);
        const auto publication_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (outcome == Pm4ProcessResult::Blocked && cache.IsRegionGpuModified(address, width) &&
               std::chrono::steady_clock::now() < publication_deadline) {
          std::this_thread::yield();
          outcome = cp.Process(execution, commands);
        }
        Require(name, "bounded actual publication before final predicate retry",
                !cache.IsRegionGpuModified(address, width),
                "resumable WAIT did not publish its native producer within the bounded fixture");
        if (outcome == Pm4ProcessResult::Blocked) outcome = cp.Process(execution, commands);
        result.wait_complete = outcome == Pm4ProcessResult::Complete;
        result.wait_blocked = outcome == Pm4ProcessResult::Blocked;
        result.consumer_after_fence = scheduler.IsFree(tick);
        if (mixed) {
          auto [sibling, sibling_offset] = cache.ObtainBuffer(sibling_address, 4u, false, true);
          CopyEopNativeLabel(scheduler, *sibling, sibling_offset, readback, 8u, 4u);
        }
        scheduler.Finish(); scheduler.WaitPriorityOperations(scheduler.CurrentTick() - 1u);
        // A post-EOP unknown writer must remain owned until its actual download.
        cache.ReadMemory(address, width);
        if (mixed) cache.ReadMemory(sibling_address, 4u);
        producer.Invalidate(); readback.Invalidate();
        std::memcpy(&result.producer, producer.data, 4u);
        std::memcpy(&result.first_native, readback.data, width);
        std::memcpy(&result.second_native, static_cast<uint8_t*>(readback.data) + 8u,
            mixed ? 4u : width);
        result.producer_complete = result.producer == gpu_marker;
        result.final_read = LibKernel::Memory::TryReadBacking(address, &result.final_backing, width);
        result.sibling_read = LibKernel::Memory::TryReadBacking(sibling_address, &result.sibling, 4u);
        result.done_read = LibKernel::Memory::TryReadBacking(done_address, &result.done, 4u);
        result.clean = !cache.HasGpuDirtyBytes(address, width);
        if (interrupt) {
          LibKernel::EventQueue::KernelEvent event{}; int count = 0;
          LibKernel::KernelUseconds timeout = 0;
          result.irq_status = LibKernel::EventQueue::KernelWaitEqueue(queue, &event, 1, &count, &timeout);
          result.irq_count = count;
          renderer.DeleteInterruptEq(queue, 0x31); LibKernel::EventQueue::KernelDeleteEqueue(queue);
        }
      });
    }
    renderer.GetGpu().SendCommandSync([&] {
      auto& scheduler = renderer.GetCommandScheduler();
      scheduler.Finish(); scheduler.DrainPriorityOperations();
    });
    renderer.UnmapMemory(FetchGpuGuest::Base, FetchGpuGuest::Bytes);
    renderer.ShutdownGpu();
    LibKernel::Memory::InstallGpuResources(nullptr);
  } // All native descriptors/buffers, callbacks, guest memory and worker disposed.
  for (uint32_t test = 0u; test < results.size(); ++test) {
    const auto& r = results[test];
    const bool mixed = test == 2u, rewrite = test == 3u, cpu_overwrite = test == 4u;
    const bool repeated_store = test == 5u, false_wait = test == 6u;
    const uint64_t width = test == 1u || false_wait ? 4u : 8u;
    const uint32_t gpu_marker = 0xcdef4000u + test;
    const uint64_t first_expected = width == 4u ? static_cast<uint32_t>(literal) : literal;
    const uint64_t final_expected = rewrite ? (literal & 0xffffffff00000000ull) | gpu_marker :
        cpu_overwrite ? cpu_value : repeated_store ? repeated : first_expected;
    std::printf("[gpu] LiteralEopCost%u setup=%u pending=%u inflight=%u cleanRefused=%u "
        "reserved=%llu->%llu first=%016llx final=%016llx done=%08x oldPubNewDirty=%u\n",
        test, r.setup, r.pending, r.byte_pending, r.clean_read_refused,
        static_cast<unsigned long long>(r.reservation_before),
        static_cast<unsigned long long>(r.reservation_after),
        static_cast<unsigned long long>(r.first_native),
        static_cast<unsigned long long>(r.final_backing), r.done, r.older_kept_new_dirty);
    Require(name, "actual native producer and real Thread_Gpu setup",
        r.actual_gpu_thread && r.setup && r.pending && r.producer_complete && r.irq_setup,
        "fixture/native setup failed; not the intended readback-cost RED");
    Require(name, "literal byte ownership survives until its true completion",
        r.byte_pending && r.clean_read_refused && r.before_callback && r.label_old_at_callback &&
        r.done_old_at_callback && r.consumer_after_fence && r.cpu_overwrite_after_fence,
        "literal publication or host WAIT consumer became visible ahead of its producer fence");
    Require(name, "native store and independent native consumers",
        r.first_native == first_expected &&
        (!(rewrite || cpu_overwrite || repeated_store) || r.second_native == final_expected) &&
        (!mixed || r.second_native == gpu_marker),
        "actual native GPU store/read was skipped or unknown neighbor/rewrite bytes were guessed");
    Require(name, "backing/generation/CPU overwrite and comparator controls",
        r.final_read && r.final_backing == final_expected && r.sibling_read &&
        r.sibling == (mixed ? gpu_marker : sibling_sentinel) && r.clean && r.older_kept_new_dirty &&
        r.done_read && r.done == (false_wait ? 0u : done_marker) &&
        (false_wait ? r.wait_blocked : r.wait_complete),
        "latest owner, false/masked wait, CPU overwrite or mixed-page bytes were corrupted");
    Require(name, "IRQ occurs after its own literal backing publication",
        !(mixed || repeated_store) || (r.irq.called && r.irq.readable && r.irq.observed == literal &&
            r.irq_status == 0 && r.irq_count == 1),
        "IRQ ran before its own literal or used a later store's value");
  }
  // Preserve every numerical/setup/lifetime control above before the cost RED.
  for (uint32_t test = 0u; test < results.size(); ++test) {
    const auto& r = results[test];
    Require(name, "known-only ordered stores need zero GPU-download reservation",
        test == 2u ? r.reservation_after > r.reservation_before :
                    r.reservation_after == r.reservation_before,
        "known literal bytes redundantly consumed the GPU-download ring");
  }
}

} // namespace
} // namespace Libs::Graphics
