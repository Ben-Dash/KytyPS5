// Ignored proposal; include after RepeatedGpuDirtyStateCostTests.inc.
// Requires PerformanceMemoryTestAccess::Tracker(BufferCache&) getter (public API only).
namespace Libs::Graphics {
namespace {
void CheckRepeatedGpuDirtyStateNeighbors(VulkanHarness& vulkan) {
  constexpr const char* name = "RepeatedGpuDirtyStateNeighbors";
  constexpr uint64_t base = RepeatedGpuDirtyGuest::Owner, bytes = TRACKER_REGION_SIZE;
  constexpr uint64_t first_page = base + 63u * TRACKER_PAGE_SIZE;
  constexpr uint64_t armed_page = base + 65u * TRACKER_PAGE_SIZE;
  constexpr uint32_t first_value = 0x12345678u, second_value = 0xabcdef09u, literal = 0x2468ace0u;
  constexpr std::array labels{"partial-dirty-rewrite-crosses-word", "previously-clean-range-becomes-gpu-dirty",
      "actual-armed-overlap-new-writer", "actual-armed-stale-overlap-new-writer", "actual-armed-outside-write"};
  struct Result {
    bool setup = false, before = false, after = false, stale = true, old_publication = true;
    bool native = false, backing = false, guards = false, clean = false, gate = true;
    uint64_t masks = 0;
  };
  std::array<Result, labels.size()> results{};
  Require(name, "existing production guest fault route",
      Common::HostException::InstallHandler([](const Common::HostException::ExceptionInfo& info) {
        namespace E = Common::HostException;
        if (info.type != E::ExceptionType::AccessViolation ||
            (info.access_violation_type != E::AccessViolationType::Read &&
             info.access_violation_type != E::AccessViolationType::Write)) return false;
        return LibKernel::Memory::HandleGpuFault(
            info.access_violation_type == E::AccessViolationType::Write ?
                PageFaultAccess::Write : PageFaultAccess::Read, info.access_violation_vaddr);
      }), "fault-handler bootstrap failed; NOT semantic RED");
  {
    RepeatedGpuDirtyGuest guest;
    auto& graphics = vulkan.RuntimeContext(); auto& renderer = vulkan.RuntimeRenderer();
    renderer.InitializeGpu(nullptr); LibKernel::Memory::InstallGpuResources(&renderer);
    renderer.MapMemory(RepeatedGpuDirtyGuest::Base, RepeatedGpuDirtyGuest::Bytes);
    auto& gpu = renderer.GetGpu(); auto& cache = renderer.GetBufferCache();
    auto& scheduler = renderer.GetCommandScheduler();
    auto& tracker = PerformanceMemoryTestAccess::Tracker(cache);
    FetchGpuBuffer proof(graphics, bytes, vk::BufferUsageFlagBits::eTransferDst); proof.Flush();
    std::unique_ptr<CommandProcessor> cp; Buffer* owner = nullptr;
    gpu.SendCommandSync([&] { cp = std::make_unique<CommandProcessor>(renderer, 0x31);
      cp->Reset(); cp->BufferInit(); owner = cache.ObtainBuffer(base, bytes, true, true).first; });
    for (uint32_t test = 0; test < results.size(); ++test) {
      auto& result = results[test]; ArmedWindowGate gate;
      gpu.SendCommandSync([&] {
        // Each scenario starts from a genuine fully-published CPU/GPU generation.
        (void)cache.ObtainBuffer(base, bytes, true, true);
        owner->Fill(owner->Offset(base), bytes, RepeatedGpuDirtyGuest::Old); cache.OnCommandRecorded();
        scheduler.Finish(); scheduler.DrainPriorityOperations(); cache.ReadMemory(base, bytes);
        scheduler.Finish(); scheduler.DrainPriorityOperations();
        PerformanceMemoryTestAccess::ResetEagerHotPolicy(cache);
        tracker.SetReadbackHot(armed_page, TRACKER_PAGE_SIZE, false);
        result.setup = GuestGpu::IsGpuThread() && !CommandRecordingDeferred() &&
            !DrainStats::Enabled() && !ReadbackAgeEnabled() &&
            !PerformanceMemoryTestAccess::HasQueue(cache) && graphics.readback_queue == nullptr &&
            owner->CpuAddress() == base && owner->Size() == bytes;
        if (test != 1) {
          (void)cache.ObtainBuffer(base, bytes, true, true);
          owner->Fill(owner->Offset(base), bytes, first_value); cache.OnCommandRecorded();
          scheduler.Finish(); scheduler.DrainPriorityOperations();
        }
        if (test >= 2) {
          scheduler.DeferPriorityOperation([&] { gate.Hold(); });
          cp->WriteAtEndOfPipe32(0, 0, 0x2f, 0, 6, 2,
              reinterpret_cast<void*>(armed_page), literal, 0);
          const auto producer = scheduler.CurrentTick();
          scheduler.Flush(); scheduler.GetMasterSemaphore().Wait(producer);
          {
            std::unique_lock lock(gate.mutex);
            result.gate = gate.cv.wait_for(lock, std::chrono::seconds(5), [&] { return gate.callback_entered; });
          }
          const auto state = tracker.QueryReadback(armed_page, 4);
          result.before = state.gpu_dirty && !state.unarmed && state.tick == producer;
          if (test == 3) {
            tracker.SetReadbackHot(armed_page, TRACKER_PAGE_SIZE, true);
            result.stale = tracker.GrantStaleRead(armed_page, 4) &&
                *reinterpret_cast<const volatile uint32_t*>(armed_page) == RepeatedGpuDirtyGuest::Old;
          }
        } else {
          const auto state = tracker.QueryReadback(first_page, 2 * TRACKER_PAGE_SIZE);
          result.before = test == 0 ? state.gpu_dirty && state.unarmed : !state.gpu_dirty;
        }
        const auto write_base = test == 2 || test == 3 ? armed_page : first_page;
        const auto write_bytes = test == 2 || test == 3 ? TRACKER_PAGE_SIZE : 2 * TRACKER_PAGE_SIZE;
        {
          RegionMaskEvaluationScope observe(result.masks);
          const auto binding = cache.ObtainBuffer(write_base, write_bytes, true, true);
          result.after = binding.first == owner && binding.second == owner->Offset(write_base);
        }
        const auto after = tracker.QueryReadback(write_base, write_bytes);
        result.after &= after.gpu_dirty && after.unarmed && after.tick == 0;
        if (test == 4) {
          const auto outside = tracker.QueryReadback(armed_page, 4);
          result.after &= outside.gpu_dirty && !outside.unarmed;
        }
        if (test == 3) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
          MEMORY_BASIC_INFORMATION memory{};
          result.stale &= VirtualQuery(reinterpret_cast<void*>(armed_page), &memory, sizeof(memory)) == sizeof(memory) &&
              (memory.Protect & 0xff) == PAGE_NOACCESS;
#else
          // Native Windows is the required fixture platform; no guest-fault probe is needed.
          result.stale = false;
#endif
        }
        owner->Fill(owner->Offset(write_base), write_bytes, second_value); cache.OnCommandRecorded();
        std::vector<uint32_t> expected(bytes / 4, test == 1 ? RepeatedGpuDirtyGuest::Old : first_value);
        if (test == 4) expected[(armed_page - base) / 4] = literal;
        std::fill(expected.begin() + (write_base - base) / 4,
            expected.begin() + (write_base - base + write_bytes) / 4, second_value);
        if (test >= 2) {
          // Release only the owned CPU callback gate; no native GPU work is blocked on the host.
          { std::lock_guard lock(gate.mutex); gate.release = true; } gate.cv.notify_all();
          scheduler.DrainPriorityOperations();
          const auto latest = tracker.QueryReadback(write_base, write_bytes);
          result.old_publication = latest.gpu_dirty && latest.unarmed;
        }
        CopyEopNativeLabel(scheduler, *owner, owner->Offset(base), proof, 0, bytes);
        scheduler.Finish(); scheduler.DrainPriorityOperations(); proof.Invalidate();
        result.native = std::memcmp(proof.data, expected.data(), bytes) == 0;
        cache.ReadMemory(base, bytes); scheduler.Finish(); scheduler.DrainPriorityOperations();
        std::vector<uint32_t> backing(bytes / 4, 0);
        result.backing = LibKernel::Memory::TryReadBacking(base, backing.data(), bytes) && backing == expected;
        result.clean = !tracker.QueryReadback(base, bytes).gpu_dirty;
        uint32_t left = 0, right = 0;
        result.guards = LibKernel::Memory::TryReadBacking(base - 4, &left, 4) &&
            LibKernel::Memory::TryReadBacking(base + bytes, &right, 4) &&
            left == RepeatedGpuDirtyGuest::Old && right == RepeatedGpuDirtyGuest::Old;
      });
      { std::lock_guard lock(gate.mutex); result.gate &= !gate.timeout; }
    }
    renderer.UnmapMemory(RepeatedGpuDirtyGuest::Base, RepeatedGpuDirtyGuest::Bytes);
    renderer.ShutdownGpu(); LibKernel::Memory::InstallGpuResources(nullptr); cp.reset();
  }
  for (uint32_t test = 0; test < results.size(); ++test) {
    const auto& result = results[test];
    std::printf("[repeated-gpu-state-neighbor] %s setup=%u before=%u after=%u stale=%u oldPublication=%u "
        "nativeAllWords=%u backingAllWords=%u guards=%u clean=%u boundedGate=%u masks=%llu\n",
        labels[test], result.setup, result.before, result.after, result.stale, result.old_publication,
        result.native, result.backing, result.guards, result.clean, result.gate,
        static_cast<unsigned long long>(result.masks));
    Require(labels[test], "ALL real producer/current-backing/pages/native/publication/guards/cleanup",
        result.setup && result.before && result.after && result.stale && result.old_publication &&
            result.native && result.backing && result.guards && result.clean && result.gate,
        "a required actual arm/stale/new-writer/page/native invariant failed");
    const auto expected_masks = test == 0 || test == 4 ? 0u : 1u;
    Require(labels[test], "only unchanged unarmed unstale ranges skip mask; real new state recomputes",
        result.masks == expected_masks, "mandatory state transition was lost or unchanged range still recomputes");
  }
}
} // namespace
} // namespace Libs::Graphics
