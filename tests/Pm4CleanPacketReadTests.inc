// Test-only proposal. Include after PerformanceMemoryTests.inc at global scope.
// Reuses its real Kernel backing + RenderContext + Thread_Gpu fixture.
namespace Libs::Graphics {
namespace {

void CheckPm4CleanPacketReads(VulkanHarness& vulkan) {
  constexpr const char* name = "Pm4CleanPacketReads";
  constexpr uint64_t base = 0x00000002d4000000ull;
  constexpr uint64_t allocation_size = 0x10000u;
  constexpr uint32_t old_word = 0x12345678u;
  constexpr uint32_t gpu_word = 0x9abcdef0u;
  constexpr uint32_t sibling_word = 0xa5b6c7d8u;
  struct Result {
    bool gpu_thread = false, lookahead = false, setup = false;
    bool clean_snapshot = false, finished = false, output_read = false;
    bool sibling_pending = false, sibling_read = false;
    uint64_t before_tick = 0, after_tick = 0;
    uint32_t output = 0, sibling = 0;
  };
  std::array<Result, 4> results{};
  // The independent speculative worker is deliberately disabled for this oracle:
  // its page reads are a separate mechanism, whereas main-thread lookahead stays ON.
  const auto previous_speculation = Config::SpeculativeDrawsEnabled();
  Config::SetSpeculativeDrawsEnabled(false);
  GraphicsInitJmpTables();
  for (uint32_t test = 0; test < results.size(); ++test) {
    {
      PerformanceMemoryFixture fixture(vulkan, name, base, allocation_size);
      auto& context = fixture.context;
      LibKernel::Memory::InstallGpuResources(&context);
      context.MapMemory(base, allocation_size);
      // Cross-page case leaves the header clean on one page and its clean payload
      // sharing the next page with the unrelated GPU producer.
      const uint64_t packet_address = base + (test == 2u ? 0xffcu : 0x100u);
      const uint64_t sibling_address = base + (test == 2u ? 0x1300u : 0x300u);
      const uint64_t output_address = base + 0x4000u;
      const bool gpu_packet = test == 1u || test == 3u;
      const std::array<uint32_t, 5> words{
          KYTY_PM4(5, test == 3u ? Pm4::IT_NOP : Pm4::IT_WRITE_DATA, 0), 0,
          static_cast<uint32_t>(output_address),
          static_cast<uint32_t>(output_address >> 32u), old_word};
      std::memcpy(reinterpret_cast<void*>(packet_address), words.data(), sizeof(words));
      const uint32_t initial_output = 0;
      std::memcpy(reinterpret_cast<void*>(output_address), &initial_output, 4u);
      context.GetGpu().SendCommandSync([&] {
        auto& cache = context.GetBufferCache();
        auto& scheduler = context.GetCommandScheduler();
        auto& result = results[test];
        result.gpu_thread = GuestGpu::IsGpuThread();
        result.lookahead = Config::DispatchLookaheadEnabled();
        (void)cache.FindBuffer(base, 0x2000u);
        cache.SynchronizeBuffersInRange(base, 0x2000u);
        (void)cache.ObtainBuffer(sibling_address, 4u, true, false);
        cache.FillBuffer(sibling_address, 4u, sibling_word, false);
        if (gpu_packet) {
          // Positive coherence control: GPU owns the actual command payload.
          // The backing deliberately retains old_word until that producer publishes.
          const auto data_address = packet_address + 4u * sizeof(uint32_t);
          (void)cache.ObtainBuffer(data_address, 4u, true, false);
          cache.FillBuffer(data_address, 4u, gpu_word, false);
          if (test == 3u) {
            (void)cache.ObtainBuffer(packet_address, 4u, true, false);
            cache.FillBuffer(packet_address, 4u,
                             KYTY_PM4(5, Pm4::IT_WRITE_DATA, 0), false);
          }
        }
        std::array<uint32_t, 5> snapshot{};
        result.clean_snapshot = LibKernel::Memory::TryReadGpuCleanBacking(
            packet_address, snapshot.data(), sizeof(snapshot));
        result.setup = cache.HasGpuDirtyBytes(sibling_address, 4u) &&
            cache.IsPageGpuDirtyHint(sibling_address) &&
            (gpu_packet ? cache.HasGpuDirtyBytes(packet_address, sizeof(words))
                        : !cache.HasGpuDirtyBytes(packet_address, sizeof(words))) &&
            (gpu_packet ? !result.clean_snapshot : result.clean_snapshot && snapshot == words);
        result.before_tick = scheduler.CurrentTick();
        Pm4Execution execution;
        result.finished = fixture.processor.Process(
            execution, {reinterpret_cast<const uint32_t*>(packet_address), words.size()}) ==
            Pm4ProcessResult::Complete;
        result.after_tick = scheduler.CurrentTick();
        result.sibling_pending = cache.HasGpuDirtyBytes(sibling_address, 4u);
        // Drain and publish all real native work before semantic assertions.
        scheduler.Finish();
        scheduler.WaitPriorityOperations(scheduler.CurrentTick() - 1u);
        result.output_read = LibKernel::Memory::TryReadBacking(
            output_address, &result.output, sizeof(result.output));
        cache.ReadMemory(sibling_address, sizeof(result.sibling));
        result.sibling_read = LibKernel::Memory::TryReadBacking(
            sibling_address, &result.sibling, sizeof(result.sibling));
      });
    } // Fixture destroys native/guest resources before the intended RED assertion.
    LibKernel::Memory::InstallGpuResources(nullptr);
  }
  Config::SetSpeculativeDrawsEnabled(previous_speculation);
  for (uint32_t test = 0; test < results.size(); ++test) {
    const auto& result = results[test];
    std::printf("[gpu] Pm4CleanPacketRead%u setup=%u lookahead=%u tick=%llu->%llu "
                "output=%08x siblingPending=%u sibling=%08x\n", test, result.setup,
                result.lookahead, static_cast<unsigned long long>(result.before_tick),
                static_cast<unsigned long long>(result.after_tick), result.output,
                result.sibling_pending, result.sibling);
    Require(name, "actual Thread_Gpu and exact-byte ownership setup",
            result.gpu_thread && result.lookahead && result.setup,
            "fixture did not establish the intended protected-page/byte ownership; not RED");
    Require(name, "command execution and independent GPU producer values",
            result.finished && result.output_read &&
            result.output == (test == 1u || test == 3u ? gpu_word : old_word) &&
            result.sibling_read && result.sibling == sibling_word,
            "command used stale bytes or lost an actual GPU producer");
    if (test == 1u || test == 3u) {
      Require(name, "actual header/payload producer stays coherent",
              result.after_tick > result.before_tick,
              "GPU-modified command bytes were consumed without producer completion");
    }
  }
  for (uint32_t test : {0u, 2u}) {
    const auto& result = results[test];
    Require(name, "clean commands preserve unrelated pending GPU work",
            result.after_tick == result.before_tick && result.sibling_pending,
            "clean PM4 bytes drained unrelated GPU work on a shared protected page");
  }
}

} // namespace
} // namespace Libs::Graphics
