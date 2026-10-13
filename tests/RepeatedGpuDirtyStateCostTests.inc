// Proposal only: include after existing readback/cost helpers in BindlessLifecycleTests.cpp.
// Requires diagnostic g_region_protection_mask_evaluations described in interfaces.md.
namespace Libs::Graphics {
namespace {
void ConfigureRepeatedGpuDirtyStateCostTests() {
  ConfigureEagerReadbackAdjacentPagesCostTests();
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  _putenv_s("KYTY_HOT_WRITES", "0");
  _putenv_s("KYTY_GPU_READ_EXACT_PAGES", "0");
  _putenv_s("KYTY_CPU_WRITE_FAULT_WINDOW", "0");
#else
  setenv("KYTY_HOT_WRITES", "0", 1);
  setenv("KYTY_GPU_READ_EXACT_PAGES", "0", 1);
  setenv("KYTY_CPU_WRITE_FAULT_WINDOW", "0", 1);
#endif
}
struct RepeatedGpuDirtyGuest {
  static constexpr uint64_t Owner = 0x0000000201000000ull;
  static constexpr uint64_t Guard = 0x10000u;
  static constexpr uint64_t Base = Owner - Guard;
  static constexpr uint64_t Bytes = TRACKER_REGION_SIZE + 2u * Guard;
  static constexpr uint32_t Old = 0x5a5a5a5au;
  int64_t direct = -1;
  RepeatedGpuDirtyGuest() {
    Require("RepeatedGpuDirtyStateCost", "real guest allocation",
        LibKernel::Memory::KernelAllocateDirectMemory(0,
            LibKernel::Memory::KernelGetDirectMemorySize(), Bytes, Guard, 0, &direct) == 0,
        "real allocation setup failed; NOT cost RED");
    void* mapped = reinterpret_cast<void*>(Base);
    Require("RepeatedGpuDirtyStateCost", "real fixed guest mapping",
        LibKernel::Memory::KernelMapDirectMemory(&mapped, Bytes, 0x3, 0x10, direct, Guard) == 0 &&
            mapped == reinterpret_cast<void*>(Base),
        "real mapping setup failed; NOT cost RED");
    std::memset(mapped, 0x5a, Bytes);
  }
  ~RepeatedGpuDirtyGuest() {
    Require("RepeatedGpuDirtyStateCost", "real guest cleanup",
        LibKernel::Memory::KernelMunmap(Base, Bytes) == 0 &&
            LibKernel::Memory::KernelReleaseDirectMemory(direct, Bytes) == 0,
        "synthetic guest mapping or allocation was not released");
  }
};
struct RegionMaskEvaluationScope {
  explicit RegionMaskEvaluationScope(uint64_t& count) {
    EXIT_IF(g_region_protection_mask_evaluations != nullptr);
    g_region_protection_mask_evaluations = &count;
  }
  ~RegionMaskEvaluationScope() { g_region_protection_mask_evaluations = nullptr; }
};
void CheckRepeatedGpuDirtyStateCost(VulkanHarness& vulkan) {
  constexpr const char* name = "RepeatedGpuDirtyStateCost";
  constexpr auto base = RepeatedGpuDirtyGuest::Owner;
  constexpr auto bytes = TRACKER_REGION_SIZE;
  constexpr uint32_t first_value = 0x12345678u, second_value = 0xabcdef09u;
  struct Result {
    bool setup = false, owner = false, initial_native = false, repeated_owner = false;
    bool pending_old_backing = false, all_gpu_dirty = false, native_rewrite = false;
    bool backing_rewrite = false, clean = false, guards = false;
    uint64_t masks = 0;
  } result;
  Require(name, "existing production guest fault route",
      Common::HostException::InstallHandler([](const Common::HostException::ExceptionInfo& info) {
        namespace E = Common::HostException;
        if (info.type != E::ExceptionType::AccessViolation ||
            (info.access_violation_type != E::AccessViolationType::Read &&
             info.access_violation_type != E::AccessViolationType::Write)) return false;
        return LibKernel::Memory::HandleGpuFault(
            info.access_violation_type == E::AccessViolationType::Write ?
                PageFaultAccess::Write : PageFaultAccess::Read, info.access_violation_vaddr);
      }), "fault-handler bootstrap failed; NOT cost RED");
  {
    RepeatedGpuDirtyGuest guest;
    auto& graphics = vulkan.RuntimeContext();
    auto& renderer = vulkan.RuntimeRenderer();
    renderer.InitializeGpu(nullptr);
    LibKernel::Memory::InstallGpuResources(&renderer);
    renderer.MapMemory(RepeatedGpuDirtyGuest::Base, RepeatedGpuDirtyGuest::Bytes);
    auto& gpu = renderer.GetGpu();
    auto& cache = renderer.GetBufferCache();
    auto& scheduler = renderer.GetCommandScheduler();
    std::unique_ptr<CommandProcessor> cp;
    FetchGpuBuffer proof(graphics, bytes, vk::BufferUsageFlagBits::eTransferDst);
    proof.Flush();
    gpu.SendCommandSync([&] {
      cp = std::make_unique<CommandProcessor>(renderer, 0x31);
      cp->Reset(); cp->BufferInit();
      auto [owner, offset] = cache.ObtainBuffer(base, bytes, true, true);
      const auto original_handle = owner->Handle();
      result.setup = GuestGpu::IsGpuThread() && !CommandRecordingDeferred() &&
          !DrainStats::Enabled() && !ReadbackAgeEnabled() &&
          !PerformanceMemoryTestAccess::HasQueue(cache) && graphics.readback_queue == nullptr;
      result.owner = owner->CpuAddress() == base && owner->Size() == bytes && offset == 0;
      owner->Fill(offset, bytes, first_value);
      cache.OnCommandRecorded();
      CopyEopNativeLabel(scheduler, *owner, offset, proof, 0, bytes);
      scheduler.Finish(); scheduler.DrainPriorityOperations(); proof.Invalidate();
      const auto all_words = [&](uint32_t expected) {
        const auto* words = static_cast<const uint32_t*>(proof.data);
        return std::all_of(words, words + bytes / 4, [expected](uint32_t word) {
          return word == expected;
        });
      };
      result.initial_native = all_words(first_value);
      uint32_t backing_first = 0, backing_last = 0;
      result.pending_old_backing = LibKernel::Memory::TryReadBacking(base, &backing_first, 4) &&
          LibKernel::Memory::TryReadBacking(base + bytes - 4, &backing_last, 4) &&
          backing_first == RepeatedGpuDirtyGuest::Old && backing_last == RepeatedGpuDirtyGuest::Old;
      result.all_gpu_dirty = true;
      for (uint64_t page = base; page < base + bytes; page += TRACKER_PAGE_SIZE) {
        const auto state = PerformanceMemoryTestAccess::Readback(cache, page, TRACKER_PAGE_SIZE);
        result.all_gpu_dirty &= state.gpu_dirty && state.unarmed && state.tick == 0 &&
            !cache.IsRegionCpuModified(page, TRACKER_PAGE_SIZE);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
        MEMORY_BASIC_INFORMATION memory{};
        result.all_gpu_dirty &= VirtualQuery(reinterpret_cast<void*>(page), &memory, sizeof(memory)) == sizeof(memory) &&
            memory.State == MEM_COMMIT && (memory.Protect & 0xff) == PAGE_NOACCESS;
#endif
      }
      Buffer* repeated = nullptr; uint64_t repeated_offset = 0;
      {
        RegionMaskEvaluationScope observe(result.masks);
        const auto binding = cache.ObtainBuffer(base, bytes, true, true);
        repeated = binding.first; repeated_offset = binding.second;
      }
      result.repeated_owner = repeated == owner && repeated->Handle() == original_handle &&
          repeated_offset == offset;
      // A genuinely later finite GPU write, not a fabricated tracker transition.
      repeated->Fill(repeated_offset, bytes, second_value);
      cache.OnCommandRecorded();
      CopyEopNativeLabel(scheduler, *repeated, repeated_offset, proof, 0, bytes);
      scheduler.Finish(); scheduler.DrainPriorityOperations(); proof.Invalidate();
      result.native_rewrite = all_words(second_value);
      cache.ReadMemory(base, bytes); scheduler.Finish(); scheduler.DrainPriorityOperations();
      std::vector<uint32_t> backing(bytes / 4, 0);
      result.backing_rewrite = LibKernel::Memory::TryReadBacking(base, backing.data(), bytes) &&
          std::all_of(backing.begin(), backing.end(), [](uint32_t word) { return word == second_value; });
      result.clean = !cache.IsRegionGpuModified(base, bytes);
      uint32_t left = 0, right = 0;
      result.guards = LibKernel::Memory::TryReadBacking(base - 4, &left, 4) &&
          LibKernel::Memory::TryReadBacking(base + bytes, &right, 4) &&
          left == RepeatedGpuDirtyGuest::Old && right == RepeatedGpuDirtyGuest::Old;
    });
    renderer.UnmapMemory(RepeatedGpuDirtyGuest::Base, RepeatedGpuDirtyGuest::Bytes);
    renderer.ShutdownGpu(); LibKernel::Memory::InstallGpuResources(nullptr); cp.reset();
  }
  std::printf("[repeated-gpu-state] setup=%u owner=%u nativeInitialAllWords=%u pendingOldBacking=%u "
      "allPagesGpuDirtyUnarmedCpuClean=%u sameOwner=%u nativeRewriteAllWords=%u backingRewriteAllWords=%u "
      "clean=%u guards=%u protectionMaskEvaluations=%llu\n", result.setup, result.owner,
      result.initial_native, result.pending_old_backing, result.all_gpu_dirty, result.repeated_owner,
      result.native_rewrite, result.backing_rewrite, result.clean, result.guards,
      static_cast<unsigned long long>(result.masks));
  Require(name, "ALL actual native producers, every owner word/page, backing, guards and cleanup BEFORE cost",
      result.setup && result.owner && result.initial_native && result.pending_old_backing &&
          result.all_gpu_dirty && result.repeated_owner && result.native_rewrite &&
          result.backing_rewrite && result.clean && result.guards,
      "semantic/native/backing/lifetime setup failed; NOT intended cost RED");
  Require(name, "repeated already-GPU-dirty unarmed unstale write skips whole-region protection mask",
      result.masks == 0, "redundant whole-region protection-mask evaluation remains");
}
} // namespace
} // namespace Libs::Graphics
