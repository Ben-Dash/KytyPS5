// Ignored draft. Standalone existing-API queued CPU PM4 neighbors.
// Configure each OFF/64 case before first GuestGpu Process in a fresh process.
// No fake handlers or scheduling observers; all marker accesses use real packets.
namespace Libs::Graphics {
namespace {
void ConfigurePm4QueueQuantumNeighbors(bool enabled = true) {
  Require("Pm4QueueQuantumNeighbors", "actual policy configuration",
      _putenv_s("KYTY_PM4_CPU_QUANTUM_PACKETS", enabled ? "64" : "0") == 0 &&
          _putenv_s("KYTY_RECORD_THREAD", "0") == 0,
      "configuration failed; not intended queue-state failure");
}
void QuantumAppendWrite(std::vector<uint32_t>& commands, uint32_t* destination,
                        uint32_t value) {
  const auto address = reinterpret_cast<uint64_t>(destination);
  commands.insert(commands.end(), {KYTY_PM4(5, Pm4::IT_WRITE_DATA, 0), 0u,
      static_cast<uint32_t>(address), static_cast<uint32_t>(address >> 32u), value});
}
void QuantumAppendCopy(std::vector<uint32_t>& commands, const uint32_t* source,
                       uint32_t* destination) {
  const auto from = reinterpret_cast<uint64_t>(source);
  const auto to = reinterpret_cast<uint64_t>(destination);
  commands.insert(commands.end(), {KYTY_PM4(6, Pm4::IT_COPY_DATA, 0),
      1u | (1u << 8u) | (1u << 20u), static_cast<uint32_t>(from),
      static_cast<uint32_t>(from >> 32u), static_cast<uint32_t>(to),
      static_cast<uint32_t>(to >> 32u)});
}
void QuantumAppendIndirect(std::vector<uint32_t>& commands,
                           std::span<const uint32_t> target, bool chain) {
  const auto address = reinterpret_cast<uint64_t>(target.data());
  commands.insert(commands.end(), {KYTY_PM4(4, Pm4::IT_INDIRECT_BUFFER, 0),
      static_cast<uint32_t>(address), static_cast<uint32_t>(address >> 32u),
      0x0f200000u | (chain ? (1u << 20u) : 0u) | static_cast<uint32_t>(target.size())});
}
// Establish pre-enqueued runnable PM4 queues without a native/GPU wait. The
// bounded host gate never controls packet results or native completion.
struct QuantumHostGate {
  std::binary_semaphore entered{0}, release{0};
  bool timed_out = false, observed = false;
  void Install(GuestGpu& gpu) {
    gpu.SendCommand([this] {
      entered.release();
      timed_out = !release.try_acquire_for(std::chrono::seconds(2));
    });
    observed = entered.try_acquire_for(std::chrono::seconds(1));
  }
};
struct alignas(64) QuantumGuardedWord {
  static constexpr uint32_t Guard = 0x71a56c93u;
  uint32_t before = Guard, value = 0, after = Guard;
  bool Guards() const { return before == Guard && after == Guard; }
};

void CheckPm4QueueQuantumIndirectBoundary(VulkanHarness& vulkan, bool enabled = true) {
  constexpr const char* name = "Pm4QueueQuantumIndirectBoundary";
  QuantumGuardedWord prefix, child_value, suffix, final_fifo;
  QuantumGuardedWord observed_prefix, observed_child, observed_suffix;
  bool controls = false, setup = false, gate_control = false;
  uint32_t capture_prefix = 0, capture_child = 0, capture_suffix = 0;
  auto& renderer = vulkan.RuntimeRenderer();
  {
    renderer.InitializeGpu(nullptr);
    auto& gpu = renderer.GetGpu();
    gpu.SendCommandSync([&] { setup = GuestGpu::IsGpuThread() && !CommandRecordingDeferred(); });
    std::vector<uint32_t> child(60u, 0x80000000u);
    QuantumAppendWrite(child, &child_value.value, 55u);
    std::vector<uint32_t> nested;
    QuantumAppendIndirect(nested, child, true);
    std::vector<uint32_t> parent;
    QuantumAppendWrite(parent, &prefix.value, 11u); // Packet1.
    QuantumAppendIndirect(parent, nested, false); // Packet2; nested chain=3.
    QuantumAppendWrite(parent, &suffix.value, 22u); // Packet65 after child64.
    std::vector<uint32_t> compute;
    QuantumAppendCopy(compute, &prefix.value, &observed_prefix.value);
    QuantumAppendCopy(compute, &child_value.value, &observed_child.value);
    QuantumAppendCopy(compute, &suffix.value, &observed_suffix.value);
    // A resume may not replay the already-run parent prefix or popped child.
    QuantumAppendWrite(compute, &prefix.value, 99u);
    QuantumAppendWrite(compute, &child_value.value, 66u);
    std::vector<uint32_t> tail;
    QuantumAppendWrite(tail, &final_fifo.value, 33u);
    QuantumHostGate gate; gate.Install(gpu);
    gpu.Submit(parent, {}); gpu.Submit(tail, {}); gpu.SubmitCompute(0x20u, compute);
    gate.release.release();
    gpu.WaitForIdle();
    gpu.SendCommandSync([&] {
      renderer.GetCommandScheduler().Finish();
      renderer.GetCommandScheduler().DrainPriorityOperations();
    });
    gate_control = gate.observed && !gate.timed_out;
    capture_prefix = observed_prefix.value;
    capture_child = observed_child.value;
    capture_suffix = observed_suffix.value;
    controls = setup && gate_control && prefix.value == 99u && child_value.value == 66u &&
        suffix.value == 22u && final_fifo.value == 33u && capture_prefix == 11u &&
        capture_child == 55u && prefix.Guards() && child_value.Guards() && suffix.Guards() &&
        final_fifo.Guards() && observed_prefix.Guards() && observed_child.Guards() &&
        observed_suffix.Guards();
    renderer.ShutdownGpu();
  }
  std::printf("[pm4-quantum-ib] enabled=%u controls=%u gate=%u "
      "captured=%u/%u/%u final=%u/%u/%u fifo=%u cleanup=1\n", enabled, controls,
      gate_control, capture_prefix, capture_child, capture_suffix, prefix.value,
      child_value.value, suffix.value, final_fifo.value);
  Require(name, "actual queued indirect-call/chain/full values/no replay/FIFO/guards/cleanup",
      controls, "real parser or cursor/queue-state failure");
  Require(name, "exact final child packet quantum and parent return",
      capture_suffix == (enabled ? 0u : 22u),
      "quantum did not preserve completed child/unexecuted parent cursor, or default OFF changed ordering");
}

void CheckPm4QueueQuantumCeDe(VulkanHarness& vulkan, bool enabled = true) {
  constexpr const char* name = "Pm4QueueQuantumCeDe";
  constexpr uint32_t Count = 128u;
  QuantumGuardedWord ce, de, prefix, tail, snapshot_ce, snapshot_de;
  bool controls = false, setup = false, gate_control = false;
  uint32_t capture_ce = 0, capture_de = 0;
  auto& renderer = vulkan.RuntimeRenderer();
  {
    renderer.InitializeGpu(nullptr);
    auto& gpu = renderer.GetGpu();
    gpu.SendCommandSync([&] { setup = GuestGpu::IsGpuThread() && !CommandRecordingDeferred(); });
    std::vector<uint32_t> constant, draw;
    QuantumAppendWrite(constant, &prefix.value, 101u);
    for (uint32_t i = 1; i <= Count; ++i) {
      constant.insert(constant.end(), {0xc0008400u, 1u, 0xc0008800u, 1u});
      QuantumAppendWrite(constant, &ce.value, i);
      draw.insert(draw.end(), {0xc0008600u, 1u, 0xc0008500u, 0u});
      QuantumAppendWrite(draw, &de.value, i);
    }
    std::vector<uint32_t> compute;
    QuantumAppendCopy(compute, &ce.value, &snapshot_ce.value);
    QuantumAppendCopy(compute, &de.value, &snapshot_de.value);
    std::vector<uint32_t> after;
    QuantumAppendWrite(after, &tail.value, 303u);
    QuantumHostGate gate; gate.Install(gpu);
    gpu.Submit(draw, constant); gpu.Submit(after, {}); gpu.SubmitCompute(0x20u, compute);
    gate.release.release();
    gpu.WaitForIdle();
    gpu.SendCommandSync([&] {
      renderer.GetCommandScheduler().Finish();
      renderer.GetCommandScheduler().DrainPriorityOperations();
    });
    gate_control = gate.observed && !gate.timed_out;
    capture_ce = snapshot_ce.value;
    capture_de = snapshot_de.value;
    controls = setup && gate_control && prefix.value == 101u && ce.value == Count &&
        de.value == Count && tail.value == 303u && prefix.Guards() && ce.Guards() &&
        de.Guards() && tail.Guards() && snapshot_ce.Guards() && snapshot_de.Guards();
    renderer.ShutdownGpu();
  }
  std::printf("[pm4-quantum-cede] enabled=%u controls=%u gate=%u "
      "captured=%u/%u final=%u/%u count=%u fifo=%u cleanup=1\n", enabled, controls,
      gate_control, capture_ce, capture_de, ce.value, de.value, Count, tail.value);
  Require(name, "actual finite CE/DE handshake/resume/FIFO/guards/cleanup",
      controls, "real CE/DE state was lost, reset, replayed or failed to complete");
  const bool observation = enabled ? capture_ce > 0u && capture_ce < Count &&
      capture_de > 0u && capture_de < Count : capture_ce == Count && capture_de == Count;
  Require(name, "one shared finite quantum across CE/DE Process calls and rounds",
      observation, "per-stream/per-round budget reset starved runnable compute or OFF changed semantics");
}
} // namespace
} // namespace Libs::Graphics
