// Include after Pm4QueueQuantumNeighbors.inc; its helpers are real PM4 encoders.
// Additional actual FIFO witness. Delivered earlier fixtures stay unchanged.
namespace Libs::Graphics {
namespace {
void CheckPm4QueueQuantumFifo(VulkanHarness& vulkan) {
  constexpr const char* name = "Pm4QueueQuantumFifo";
  constexpr uint32_t Count = 257u;
  QuantumGuardedWord graphics, compute, graphics_tail, compute_tail;
  bool setup = false, gate_control = false, values = false, guards = false;
  auto& renderer = vulkan.RuntimeRenderer();
  {
    renderer.InitializeGpu(nullptr);
    auto& gpu = renderer.GetGpu();
    gpu.SendCommandSync([&] { setup = GuestGpu::IsGpuThread() && !CommandRecordingDeferred(); });
    std::vector<uint32_t> draw, dispatch, draw_tail, dispatch_tail;
    for (uint32_t i = 1; i <= Count; ++i) {
      QuantumAppendWrite(draw, &graphics.value, i);
      QuantumAppendWrite(dispatch, &compute.value, i);
    }
    // A tail execution must observe its OWN queue's prior submission complete.
    // A requeue-at-back defect captures64/128/... instead of257 after a yield.
    QuantumAppendCopy(draw_tail, &graphics.value, &graphics_tail.value);
    QuantumAppendCopy(dispatch_tail, &compute.value, &compute_tail.value);
    QuantumHostGate gate; gate.Install(gpu);
    gpu.Submit(draw, {}); gpu.Submit(draw_tail, {});
    gpu.SubmitCompute(0x20u, dispatch); gpu.SubmitCompute(0x20u, dispatch_tail);
    gate.release.release();
    gpu.WaitForIdle();
    gpu.SendCommandSync([&] {
      renderer.GetCommandScheduler().Finish();
      renderer.GetCommandScheduler().DrainPriorityOperations();
    });
    gate_control = gate.observed && !gate.timed_out;
    values = graphics.value == Count && compute.value == Count &&
        graphics_tail.value == Count && compute_tail.value == Count;
    guards = graphics.Guards() && compute.Guards() && graphics_tail.Guards() && compute_tail.Guards();
    renderer.ShutdownGpu();
  }
  std::printf("[pm4-quantum-fifo] setup=%u gate=%u full=%u/%u "
      "capturedTail=%u/%u expected=%u guards=%u cleanup=1\n", setup, gate_control,
      graphics.value, compute.value, graphics_tail.value, compute_tail.value, Count, guards);
  Require(name, "real queued completion/canaries/cleanup controls",
      setup && gate_control && graphics.value == Count && compute.value == Count && guards,
      "setup, finite execution, guard or cleanup failure; not queue FIFO failure");
  Require(name, "same-queue tails see completed prior submissions across yields",
      values, "a partially processed submission was requeued after its own queue's next submission");
}
} // namespace
} // namespace Libs::Graphics
