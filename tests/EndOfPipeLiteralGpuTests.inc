// Separate literal-only proof; the original two-case EOP oracle remains unchanged.
namespace Libs::Graphics {
namespace {
void CheckEndOfPipeLiteralGpu(VulkanHarness& vulkan) {
  constexpr const char* name = "EndOfPipeLiteralGpu";
  constexpr uint64_t literal = 0x1122334455667788ull;
  std::array<EopGpuObservation, 1> observations{};
  {
    // Kernel-backed mapped guest labels are deliberately ordinary BufferCache
    // ranges: no registered/GPU-dirty page fault can make observation wait.
    // Registered and GPU-dirty label publication are separate required neighbors.
    FetchGpuGuest guest;
    auto& renderer = vulkan.RuntimeRenderer();
    LibKernel::Memory::InstallGpuResources(&renderer);
    renderer.MapMemory(FetchGpuGuest::Base, FetchGpuGuest::Bytes);
    auto& scheduler = renderer.GetCommandScheduler();
    auto& cache = renderer.GetBufferCache();
    CommandProcessor processor(renderer, 0);
    processor.Reset();
    processor.BufferInit();
    FetchGpuBuffer producer(vulkan.RuntimeContext(), 16u, vk::BufferUsageFlagBits::eTransferDst);
    for (uint32_t k = 0; k < observations.size(); ++k) {
      auto& observed = observations[k];
      observed.name = k == 0u ? "Eop64LiteralBottom" : "Eop64GpuClockBottom";
      const uint64_t address = FetchGpuGuest::Base + 0x8000u + k * 0x1000u;
      auto* label = reinterpret_cast<uint64_t*>(address);
      observed.sentinel = 0x8899aabbccddeef0ull + k;
      std::memcpy(label, &observed.sentinel, sizeof(uint64_t));
      Require(name, "ordinary tracked label setup", !cache.IsRegionRegistered(address, 8u) &&
                  !cache.HasGpuDirtyBytes(address, 8u),
              "label must be Kernel-mapped but BufferCache-unregistered/clean; not a semantic RED");
      std::printf("[host] %s label=0x%llx kernelMapped=1 bufferRegistered=0 gpuDirty=0\n",
                  observed.name, static_cast<unsigned long long>(address));
      std::memset(producer.data, 0, producer.size);
      producer.Flush();
      const uint32_t marker = 0x12340000u + k;
      auto command = scheduler.Current().Handle();
      vk::MemoryBarrier ready{};
      ready.srcAccessMask = vk::AccessFlagBits::eHostWrite;
      ready.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
      command.pipelineBarrier(vk::PipelineStageFlagBits::eHost, vk::PipelineStageFlagBits::eTransfer,
          {}, 1u, &ready, 0u, nullptr, 0u, nullptr);
      command.fillBuffer(producer.buffer, 0u, producer.size, marker);
      vk::MemoryBarrier done{};
      done.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
      done.dstAccessMask = vk::AccessFlagBits::eHostRead;
      command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
          {}, 1u, &done, 0u, nullptr, 0u, nullptr);
      observed.tick = scheduler.CurrentTick();
      observed.recorded_pending = !scheduler.IsFree(observed.tick);
      // 0x28 is BOTTOM_OF_PIPE_TS. The existing API's internal index0 is
      // accepted for the normalized64-bit RELEASE_MEM path (no IRQ/cache flush).
      processor.WriteAtEndOfPipe64(0u, 0u, 0x28u, 0u, 0u, k == 0u ? 2u : 4u,
                                    label, literal, 0u);
      // Observe the actual Kernel backing without a guest-page fault/readback:
      // a valid owner-aware implementation may protect the label page here.
      const bool backing_peek = LibKernel::Memory::TryReadBacking(
          address, &observed.before_submit, sizeof(uint64_t));
      observed.tick_unchanged = backing_peek && scheduler.CurrentTick() == observed.tick &&
          !scheduler.IsFree(observed.tick);
      observed.clock_before_submit = Sync::ReadReferenceClock();
      // Drain before evaluating any failing observation: no outstanding GPU
      // gate/work, dangling native buffer or guest label survives the oracle.
      scheduler.Finish();
      scheduler.WaitPriorityOperations(observed.tick);
      producer.Invalidate();
      const auto* words = static_cast<const uint32_t*>(producer.data);
      observed.producer_complete = scheduler.IsFree(observed.tick) &&
          std::all_of(words, words + 4u, [marker](uint32_t word) { return word == marker; });
      std::memcpy(&observed.after_finish, label, sizeof(uint64_t));
      observed.clock_after_finish = Sync::ReadReferenceClock();
      std::printf("[gpu] %s tick=%llu pending=%u unchanged=%u sentinel=%016llx beforeSubmit=%016llx afterFinish=%016llx producer=%u clockBracket=%016llx..%016llx\n",
          observed.name, static_cast<unsigned long long>(observed.tick), observed.recorded_pending,
          observed.tick_unchanged, static_cast<unsigned long long>(observed.sentinel),
          static_cast<unsigned long long>(observed.before_submit),
          static_cast<unsigned long long>(observed.after_finish), observed.producer_complete,
          static_cast<unsigned long long>(observed.clock_before_submit),
          static_cast<unsigned long long>(observed.clock_after_finish));
    }
    // Follow the normal mapped-resource unmap lifecycle before releasing guest memory.
    // It invalidates the cache's CPU write watches after all backing publications drain.
    renderer.UnmapMemory(FetchGpuGuest::Base, FetchGpuGuest::Bytes);
    LibKernel::Memory::InstallGpuResources(nullptr);
  }
  for (const auto& observed : observations) {
    Require(observed.name, "real pending/completed producer setup",
            observed.recorded_pending && observed.tick_unchanged && observed.producer_complete,
            "must observe a recorded-unsubmitted producer then actual native completion; not the intended RED");
    Require(observed.name, "EOP label not published before its GPU work",
            observed.before_submit == observed.sentinel,
            "actual public CP published a guest label while the prior native fill was still unsubmitted");
    Require(observed.name, "completed literal EOP publication",
            observed.after_finish == literal,
            "completed label must hold the requested literal after actual native completion");
  }
}
} // namespace
} // namespace Libs::Graphics
