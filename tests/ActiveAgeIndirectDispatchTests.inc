// Include INSIDE VulkanHarness. Separate neighbor; do not weaken CheckNativeIndirectDispatch.
// Test-only StreamBufferTestAccess helper needed:
// static uint64_t ReservationOffset(const StreamBuffer& buffer) { return buffer.m_offset; }
void CheckActiveAgeIndirectDispatch() {
  constexpr const char* name = "ActiveAgeIndirectDispatch";
  constexpr uint64_t base = 0x0000000208d00000ull, bytes = 0x20000u;
  constexpr uint64_t args_address = base + 0x100u;
  constexpr uint64_t output_address = base + 0x10100u;
  constexpr u32 sentinel = 0xa5a5a5a5u;
  EnsureRuntimeContext();
  int64_t direct = -1;
  Require(name, "owned direct allocation setup",
    LibKernel::Memory::KernelAllocateDirectMemory(0,
      LibKernel::Memory::KernelGetDirectMemorySize(), bytes, 0x10000u, 0, &direct) == 0,
    "native indirect fixture allocation failed");
  void* mapped = reinterpret_cast<void*>(base);
  Require(name, "owned direct mapping setup",
    LibKernel::Memory::KernelMapDirectMemory(&mapped, bytes, 0x3, 0x10, direct, 0x10000u) == 0 &&
      mapped == reinterpret_cast<void*>(base), "native indirect fixture mapping failed");
  std::fill_n(static_cast<u32*>(mapped), bytes / sizeof(u32), sentinel);
  std::fill_n(reinterpret_cast<u32*>(args_address), 3, 0u);

  std::vector<u32> producer;
  AppendVMovU32(&producer, 0, 0);
  AppendVMovU32(&producer, 1, 2);
  producer.push_back(EncodeMubuf0(0x1cu, 0, false, true));
  producer.push_back(EncodeMubuf1(1, 0, 0)); // (vdata,srsrc,vaddr)
  AppendVMovU32(&producer, 1, 1);
  for (u32 offset : {4u, 8u}) {
    producer.push_back(EncodeMubuf0(0x1cu, offset, false, true));
    producer.push_back(EncodeMubuf1(1, 0, 0));
  }
  AppendEnd(&producer);

  std::vector<u32> consumer;
  consumer.push_back(EncodeVop1(0x01u, 1, 4)); // v1=s4 workgroup X
  consumer.push_back(EncodeVop2(0x1au, 1, InlineU32(2), 1)); // group*4
  consumer.push_back(EncodeVop2(0x25u, 1, Vgpr(0), 1)); // global X
  consumer.push_back(EncodeVop2(0x25u, 2, InlineU32(1), 1)); // independent +1 output
  consumer.push_back(EncodeMubuf0(0x1cu, 0, true, false));
  consumer.push_back(EncodeMubuf1(2, 0, 1));
  AppendEnd(&consumer);
  for (const auto* code : {&producer, &consumer}) {
    ShaderMapUserData(reinterpret_cast<uint64_t>(code->data()),
      {.type = Prospero::ShaderBinaryType::kCs,
       .code_size_bytes = static_cast<u32>(code->size() * sizeof(u32))});
  }
  bool tick_advanced = false, no_download = false, args_stale = false;
  bool values_correct = false, args_correct = false;
  RenderContext context(m_runtime_context);
  context.InitializeGpu(nullptr);
  LibKernel::Memory::InstallGpuResources(&context);
  context.MapMemory(base, bytes);
  context.GetGpu().SendCommandSync([&] {
    auto& scheduler = context.GetCommandScheduler();
    auto& cache = context.GetBufferCache();
    HW::Context registers{};
    HW::UserConfig user_config{};
    HW::Shader shaders{};
    scheduler.Begin(registers, user_config, shaders);
    const auto SetShader = [&](const std::vector<u32>& code, u32 threads,
                               bool group_x, uint64_t address, u32 records) {
      shaders.SetCsShader({.data_addr = reinterpret_cast<uint64_t>(code.data()),
        .num_thread_x = threads, .num_thread_y = 1, .num_thread_z = 1,
        .wave_size = 64, .user_sgpr = 4, .tgid_x_en = group_x});
      ShaderBufferResource descriptor{};
      descriptor.UpdateAddress48(address);
      descriptor.fields[1] |= 4u << 16u;
      descriptor.fields[2] = records;
      descriptor.fields[3] = DstSel(4, 5, 6, 7) |
        (static_cast<u32>(Prospero::BufferFormat::k32UInt) << 12u);
      for (u32 i = 0; i < 4; ++i)
        shaders.SetCsUserSgpr(i, descriptor.fields[i], HW::UserSgprType::Unknown);
    };
    const auto Warm = [&] {
      ShaderComputeInputInfo input{};
      const auto program = context.GetPipelineCache().GetComputeProgram(
        shaders.GetCs(), registers.GetShaderRegisters(), input);
      Require(name, "actual pipeline warm-up setup", bool(program) &&
        context.GetPipelineCache().GetComputePipeline(input, program).pipeline != nullptr,
        "native producer/consumer program did not warm");
    };
    SetShader(consumer, 4, true, output_address, 16);
    Warm();
    SetShader(producer, 1, false, args_address, 3);
    Warm();
    context.GetRenderExecutor().DispatchDirect(0, scheduler.Current(), 1, 1, 1, 0x41u);
    Require(name, "native producer ownership setup", cache.IsRegionGpuModified(args_address, 12),
            "argument producer did not create native GPU-written memory");
    auto& downloads = BufferCacheTestAccess::DownloadBuffer(cache);
    const auto before_downloads = StreamBufferTestAccess::ReservationOffset(downloads);
    const auto before_indirect = scheduler.CurrentTick();
    SetShader(consumer, 4, true, output_address, 16);
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
    context.GetRenderExecutor().DispatchIndirect(0, scheduler.Current(), args_address, 0x41u);
    tick_advanced = scheduler.CurrentTick() > before_indirect;
    scheduler.Finish();
    no_download = StreamBufferTestAccess::ReservationOffset(downloads) == before_downloads;
    std::array<u32, 3> backing_before{};
    Require(name, "argument backing observation setup",
      LibKernel::Memory::TryReadBacking(args_address, backing_before.data(), sizeof(backing_before)),
      "kernel argument backing could not be observed");
    args_stale = backing_before == std::array<u32,3>{0,0,0};

    cache.ReadMemory(output_address - 16, 24u * sizeof(u32));
    scheduler.Finish();
    std::array<u32, 24> output{};
    Require(name, "consumer backing observation setup",
      LibKernel::Memory::TryReadBacking(output_address - 16, output.data(), sizeof(output)),
      "indirect consumer results were not published");
    values_correct = true;
    for (u32 index = 0; index < output.size(); ++index) {
      const auto expected = index >= 4 && index < 12 ? index - 3u : sentinel;
      values_correct &= output[index] == expected;
    }
    cache.ReadMemory(args_address, 12);
    scheduler.Finish();
    std::array<u32,3> args_after{};
    Require(name, "explicit argument publication setup",
      LibKernel::Memory::TryReadBacking(args_address, args_after.data(), sizeof(args_after)),
      "explicit argument publication could not be observed");
    args_correct = args_after == std::array<u32,3>{2,1,1};
    context.UnmapMemory(base, bytes);
  });
  context.ShutdownGpu();
  LibKernel::Memory::InstallGpuResources(nullptr);
  Require(name, "owned mapping cleanup", LibKernel::Memory::KernelMunmap(base, bytes) == 0,
          "owned mapping cleanup failed");
  Require(name, "owned allocation cleanup",
          LibKernel::Memory::KernelReleaseDirectMemory(direct, bytes) == 0,
          "owned allocation cleanup failed");
  Require(name, "active age split with required native order", tick_advanced,
          "active age budget did not submit the preceding real producer");
  Require(name, "indirect args stayed native", no_download && args_stale,
          "indirect count arguments caused a download reservation or CPU publication");
  Require(name, "two groups four lanes and surrounding sentinels", values_correct,
          "native indirect arguments lost producer order, coverage or descriptor bounds");
  Require(name, "explicit argument bytes", args_correct,
          "independent producer did not write exact2,1,1 arguments");
  std::printf("[gpu] %-32s ok nativeArgs=1 ageSplit=1 outputWords=8\n", name);
}
