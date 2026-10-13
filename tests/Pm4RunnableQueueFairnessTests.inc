// Ignored draft. Include alongside existing public GuestGpu CPU/native harness.
// Configure before constructing renderer/GPU or calling any quantum getter.
// Uses real public submissions, parser, WRITE_DATA and COPY_DATA handlers.
// No production test observer, mock packet handler, infinite GPU loop or timing oracle.
namespace Libs::Graphics {
namespace {
void ConfigurePm4RunnableQueueFairnessTests() {
  Require("Pm4RunnableQueueFairness", "synthetic quantum configuration",
      _putenv_s("KYTY_PM4_CPU_QUANTUM_PACKETS", "64") == 0 &&
      _putenv_s("KYTY_RECORD_THREAD", "0") == 0 &&
      _putenv_s("KYTY_DISPATCH_LOOKAHEAD", "0") == 0,
      "configuration failed; not intended fairness RED");
}

void CheckPm4RunnableQueueFairness(VulkanHarness& vulkan) {
  constexpr const char* name = "Pm4RunnableQueueFairness";
  constexpr uint32_t Count = 4096u;
  constexpr uint32_t Guard = 0xa1579bcdu;
  constexpr uint32_t Unobserved = 0xfedcba98u;
  struct alignas(64) Marker {
    uint32_t before = Guard, progress = 0, after = Guard;
  } graphics;
  struct alignas(64) Snapshot {
    uint32_t before = Guard, value = Unobserved, after = Guard;
  } first, second, other;
  struct alignas(64) Trace {
    uint32_t before = Guard;
    std::array<uint32_t, Count> values{};
    uint32_t after = Guard;
  } trace;
  uint32_t graphics_fifo = 0;
  bool setup = false, gate_entered = false, gate_timeout = false;
  bool queues_idle = false, contents = false, fifo = false, guards = false;
  uint32_t first_snapshot = Unobserved, second_snapshot = Unobserved;
  uint32_t other_snapshot = Unobserved;
  auto& renderer = vulkan.RuntimeRenderer();
  {
    renderer.InitializeGpu(nullptr);
    auto& gpu = renderer.GetGpu();
    gpu.SendCommandSync([&] {
      setup = GuestGpu::IsGpuThread() && !CommandRecordingDeferred() &&
          !renderer.GetBufferCache().IsRegionGpuModified(
              reinterpret_cast<uint64_t>(&graphics.progress), sizeof(uint32_t));
    });
    // Every graphics packet writes its iteration to a disjoint trace word as
    // well as to the current progress word; all bytes have a finite owner.
    std::vector<uint32_t> graphics_commands;
    graphics_commands.reserve(Count * 10u);
    const auto append_write = [](std::vector<uint32_t>& to,
                                 uint32_t* destination, uint32_t value) {
      const auto address = reinterpret_cast<uint64_t>(destination);
      to.insert(to.end(), {KYTY_PM4(5, Pm4::IT_WRITE_DATA, 0), 0u,
          static_cast<uint32_t>(address), static_cast<uint32_t>(address >> 32u), value});
    };
    for (uint32_t i = 1; i <= Count; ++i) {
      append_write(graphics_commands, &graphics.progress, i);
      append_write(graphics_commands, &trace.values[i - 1u], i);
    }
    // Src/dst selectors both 2 (memory), one DWORD, write confirm enabled.
    // Real COPY_DATA -> DmaData -> clean CPU CopyBuffer path, no hidden hook.
    const auto make_snapshot = [&](uint32_t* destination) {
      const auto source = reinterpret_cast<uint64_t>(&graphics.progress);
      const auto target = reinterpret_cast<uint64_t>(destination);
      return std::array<uint32_t, 6>{KYTY_PM4(6, Pm4::IT_COPY_DATA, 0),
          1u | (1u << 8u) | (1u << 20u), static_cast<uint32_t>(source),
          static_cast<uint32_t>(source >> 32u), static_cast<uint32_t>(target),
          static_cast<uint32_t>(target >> 32u)};
    };
    const auto first_commands = make_snapshot(&first.value);
    const auto second_commands = make_snapshot(&second.value);
    const auto other_commands = make_snapshot(&other.value);
    std::vector<uint32_t> graphics_tail;
    append_write(graphics_tail, &graphics_fifo, 0x12345678u);
    // The bounded host gate only establishes simultaneous runnable queues.
    // It is released before the test measures semantics, and never waits GPU.
    std::binary_semaphore gate_enter{0}, release_gate{0};
    gpu.SendCommand([&] {
      gate_enter.release();
      gate_timeout = !release_gate.try_acquire_for(std::chrono::seconds(2));
    });
    gate_entered = gate_enter.try_acquire_for(std::chrono::seconds(1));
    gpu.Submit(graphics_commands, {});
    gpu.Submit(graphics_tail, {});
    gpu.SubmitCompute(0x20u, first_commands);
    gpu.SubmitCompute(0x20u, second_commands);
    gpu.SubmitCompute(0x21u, other_commands);
    release_gate.release();
    gpu.WaitForIdle();
    // All borrowed vectors/markers remain alive until real processing idle
    // and renderer teardown. Record the oracle only after synchronization.
    gpu.SendCommandSync([&] {
      queues_idle = GuestGpu::IsGpuThread();
      renderer.GetCommandScheduler().Finish();
      renderer.GetCommandScheduler().DrainPriorityOperations();
    });
    first_snapshot = first.value;
    second_snapshot = second.value;
    other_snapshot = other.value;
    contents = graphics.progress == Count;
    for (uint32_t i = 0; i < Count; ++i) contents &= trace.values[i] == i + 1u;
    // Queue 0 and queue 0x20 must keep their own submission FIFO. First and
    // second capture monotonically growing progress; no queue reset/replay.
    fifo = graphics_fifo == 0x12345678u && first.value <= second.value &&
        second.value <= Count && first.value != Unobserved &&
        other.value != Unobserved && other.value <= Count;
    guards = graphics.before == Guard && graphics.after == Guard &&
        first.before == Guard && first.after == Guard &&
        second.before == Guard && second.after == Guard &&
        other.before == Guard && other.after == Guard &&
        trace.before == Guard && trace.after == Guard;
    renderer.ShutdownGpu();
  }
  std::printf("[pm4-fairness] setup=%u gate=%u timeout=%u idle=%u contents=%u "
      "fifo=%u guards=%u first=%u second=%u other=%u count=%u cleanup=1\n",
      setup, gate_entered, gate_timeout, queues_idle, contents, fifo, guards,
      first_snapshot, second_snapshot, other_snapshot, Count);
  Require(name, "public queue/parser/full values/FIFO/canary/cleanup controls before fairness",
      setup && gate_entered && !gate_timeout && queues_idle && contents && fifo && guards,
      "setup, parser, completion, FIFO, value, guard or cleanup failure; NOT intended fairness RED");
  Require(name, "runnable compute observes graphics between finite packets",
      first_snapshot > 0u && first_snapshot < Count && other_snapshot > 0u &&
          other_snapshot < Count,
      "runnable graphics monopolized the whole finite PM4 buffer before other queues ran");
}
} // namespace
} // namespace Libs::Graphics
