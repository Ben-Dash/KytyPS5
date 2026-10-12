// Uncompiled test-only proposal. Include INSIDE VulkanHarness after its existing
// CheckComputeMetaClearClassification member. At global file scope add
// #include <chrono> and <thread>. No new production API.
// Root must compile/run this before any production recording-age submit patch.
// GetComputePipeline/UniformFillKind enum/bootstrap setup must match current APIs.
// Setup/build failures are not the intended RED; preserve final byte/tick oracles.
void CheckDispatchRecordingAgeBudget() {
  constexpr const char* name = "DispatchRecordingAgeBudget";
  constexpr uint64_t base = 0x0000000208c00000ull, bytes = 0x10000u;
  constexpr uint32_t prefix = 16u, words = 64u;
  constexpr uint32_t sentinel = 0xdeadbeefu;
  EnsureRuntimeContext();
  RenderContext context(m_runtime_context);
  context.InitializeGpu(nullptr);
  auto& gpu = context.GetGpu();
  int64_t direct = -1;
  Require(name, "owned direct allocation setup",
    LibKernel::Memory::KernelAllocateDirectMemory(0,
      LibKernel::Memory::KernelGetDirectMemorySize(), bytes, bytes, 0, &direct) == 0,
    "owned direct allocation failed; not a cost RED");
  void* mapped = reinterpret_cast<void*>(base);
  Require(name, "owned direct mapping setup",
    LibKernel::Memory::KernelMapDirectMemory(&mapped, bytes, 0x3, 0x10, direct, bytes) == 0 &&
      mapped == reinterpret_cast<void*>(base),
    "owned direct mapping failed; not a cost RED");
  std::fill_n(static_cast<uint32_t*>(mapped), bytes / 4u, sentinel);
  for (u32 lane = 0; lane < words; ++lane)
    static_cast<uint32_t*>(mapped)[prefix / 4u + lane] = 0xfffffff0u + lane;
  LibKernel::Memory::InstallGpuResources(&context);
  context.MapMemory(base, bytes);

  std::vector<u32> code;
  code.push_back(EncodeMubuf0(0x0cu, 0, true, false)); // BUFFER_LOAD_DWORD
  code.push_back(EncodeMubuf1(1u, 0u, 0u)); // data v1, V# s[0:3], index v0
  code.push_back(EncodeVop2(0x25u, 1u, InlineU32(1u), 1u)); // modulo32bit +1
  code.push_back(EncodeMubuf0(0x1cu, 0, true, false)); // BUFFER_STORE_DWORD
  code.push_back(EncodeMubuf1(1u, 0u, 0u));
  AppendEnd(&code);
  ShaderMapUserData(reinterpret_cast<uint64_t>(code.data()),
    {.type = Prospero::ShaderBinaryType::kCs,
     .code_size_bytes = static_cast<u32>(code.size() * sizeof(u32))});

  bool correct = false, aged_tick_advanced = false;
  gpu.SendCommandSync([&] {
    auto& scheduler = context.GetCommandScheduler();
    HW::Context registers{};
    HW::UserConfig config{};
    HW::Shader shaders{};
    scheduler.Begin(registers, config, shaders);
    HW::CsStageRegisters regs{};
    regs.data_addr = reinterpret_cast<uint64_t>(code.data());
    regs.num_thread_x = words;
    regs.num_thread_y = regs.num_thread_z = 1;
    regs.wave_size = 64;
    regs.user_sgpr = 4;
    regs.tgid_x_en = false;
    shaders.SetCsShader(regs);
    ShaderBufferResource descriptor{};
    descriptor.UpdateAddress48(base + prefix);
    descriptor.fields[1] |= 4u << 16u;
    descriptor.fields[2] = words;
    descriptor.fields[3] = DstSel(4, 5, 6, 7) |
      (static_cast<u32>(Prospero::BufferFormat::k32UInt) << 12u);
    for (u32 i = 0; i < 4; ++i)
      shaders.SetCsUserSgpr(i, descriptor.fields[i], HW::UserSgprType::Unknown);

    ShaderComputeInputInfo info{};
    auto program = context.GetPipelineCache().GetComputeProgram(
      shaders.GetCs(), registers.GetShaderRegisters(), info);
    Require(name, "actual program materialization setup", bool(program) &&
      info.stage.resources != nullptr,
      "real synthetic load/add/store program was not materialized; not a cost RED");
    Require(name, "not a DMA-equivalent pure fill setup",
      info.stage.resources->uniform_fill.kind == ShaderRecompiler::IR::UniformFillKind::None,
      "load/add/store kernel was unexpectedly recognized as pure fill; not a cost RED");
    Require(name, "real pipeline warm-up setup",
      context.GetPipelineCache().GetComputePipeline(info, program).pipeline != nullptr,
      "synthetic pipeline did not warm; not a cost RED");
    context.GetRenderExecutor().DispatchDirect(0, scheduler.Current(), 1, 1, 1, 0x41u);
    const auto first_tick = scheduler.CurrentTick();
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
    context.GetRenderExecutor().DispatchDirect(0, scheduler.Current(), 1, 1, 1, 0x41u);
    aged_tick_advanced = scheduler.CurrentTick() > first_tick;
    scheduler.Finish();
    context.GetBufferCache().ReadMemory(base + prefix, words * 4u);
    scheduler.Finish();
    std::array<uint32_t, 128> actual{};
    Require(name, "actual result publication setup",
      LibKernel::Memory::TryReadBacking(base, actual.data(), sizeof(actual)),
      "synthetic result was not readable after real completion; not a cost RED");
    correct = true;
    for (u32 i = 0; i < actual.size(); ++i) {
      const auto expected = i >= prefix / 4u && i < prefix / 4u + words ?
        (0xfffffff0u + (i - prefix / 4u) + 2u) : sentinel;
      if (actual[i] != expected && i < 12u)
        std::printf("[gpu] recording-age word%u actual=%08x expected=%08x\n", i, actual[i], expected);
      correct &= actual[i] == expected;
    }
    context.UnmapMemory(base, bytes);
  });
  context.ShutdownGpu();
  LibKernel::Memory::InstallGpuResources(nullptr);
  Require(name, "owned mapping cleanup", LibKernel::Memory::KernelMunmap(base, bytes) == 0,
          "owned mapping cleanup failed; not a cost RED");
  Require(name, "owned allocation cleanup",
          LibKernel::Memory::KernelReleaseDirectMemory(direct, bytes) == 0,
          "owned allocation cleanup failed; not a cost RED");
  Require(name, "both dispatches and surrounding sentinels", correct,
          "two load/add/store kernels changed results or wrote outside their guest range");
  std::printf("[cost] real DispatchDirect agedTickAdvanced=%u\n", aged_tick_advanced ? 1u : 0u);
  Require(name, "aged recording submitted before next dispatch", aged_tick_advanced,
          "already recorded GPU work stays buffered past the shared recording-age budget");
}
