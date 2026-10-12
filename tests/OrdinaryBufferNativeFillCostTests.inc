// Include INSIDE VulkanHarness; globally include graphics/host_gpu/renderer/gpuZones.h.
// Real existing APIs only. Setup/golden failure is not the intended cost RED.
void CheckOrdinaryBufferNativeFillCost() {
  constexpr const char* name = "OrdinaryBufferNativeFillCost";
  constexpr uint64_t base = 0x0000000208e00000ull, bytes = 0x10000u;
  constexpr u32 prefix = 16u, words = 64u;
  constexpr u32 sentinel = 0xdeadbeefu, old_bias = 0x40000000u, fill = 0x11223344u;
  EnsureRuntimeContext();
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
  std::fill_n(static_cast<u32*>(mapped), bytes / sizeof(u32), sentinel);

  std::vector<u32> primer;
  AppendVMovU32(&primer, 1, old_bias);
  primer.push_back(EncodeVop2(0x25u, 1, Vgpr(0), 1));
  primer.push_back(EncodeMubuf0(0x1cu, 0, true, false)); // raw store not uniform-fill eligible
  primer.push_back(EncodeMubuf1(1, 0, 0)); // (vdata,srsrc,vaddr)
  AppendEnd(&primer);
  std::vector<u32> code;
  AppendVMovU32(&code, 1, fill);
  code.push_back(EncodeVop1(0x01u, 2, 4)); // s4=workgroup X
  code.push_back(EncodeVop2(0x1au, 2, InlineU32(6), 2));
  code.push_back(EncodeVop2(0x25u, 2, Vgpr(0), 2));
  code.push_back(EncodeMubuf0(0x04u, 0, true, false)); // BUFFER_STORE_FORMAT_X
  code.push_back(EncodeMubuf1(1, 0, 2));
  AppendEnd(&code);
  for (const auto* shader : {&primer, &code})
    ShaderMapUserData(reinterpret_cast<uint64_t>(shader->data()),
      {.type = Prospero::ShaderBinaryType::kCs,
       .code_size_bytes = static_cast<u32>(shader->size() * sizeof(u32))});

  struct Counts {
    u32 game_dispatch = 0, buffer_copy = 0;
    static void Mark(void* opaque, vk::CommandBuffer, DrainStats::Zone zone, uint64_t, uint64_t) {
      auto& counts = *static_cast<Counts*>(opaque);
      if (zone == DrainStats::Zone::GameDispatch) ++counts.game_dispatch;
      if (zone == DrainStats::Zone::BufferCopy) ++counts.buffer_copy;
    }
  } counts;
  bool correct = false, cpu_old_before_fence = false, native_dirty_after = false;
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
    const auto SetShader = [&](const std::vector<u32>& shader, bool group_x) {
      shaders.SetCsShader({.data_addr = reinterpret_cast<uint64_t>(shader.data()),
        .num_thread_x = words, .num_thread_y = 1, .num_thread_z = 1,
        .wave_size = 64, .user_sgpr = 4, .tgid_x_en = group_x});
      ShaderBufferResource descriptor{};
      descriptor.UpdateAddress48(base + prefix);
      descriptor.fields[1] |= 4u << 16u;
      descriptor.fields[2] = words;
      descriptor.fields[3] = DstSel(4,5,6,7) |
        (static_cast<u32>(Prospero::BufferFormat::k32UInt) << 12u);
      for (u32 i = 0; i < 4; ++i)
        shaders.SetCsUserSgpr(i, descriptor.fields[i], HW::UserSgprType::Unknown);
    };
    SetShader(code, true);
    ShaderComputeInputInfo info{};
    const auto program = context.GetPipelineCache().GetComputeProgram(
      shaders.GetCs(), registers.GetShaderRegisters(), info);
    Require(name, "actual complete uniform-fill proof setup", bool(program) &&
      info.stage.resources != nullptr &&
      info.stage.resources->uniform_fill.kind == ShaderRecompiler::IR::UniformFillKind::Buffer &&
      info.stage.resources->uniform_fill.uniform &&
      info.stage.resources->uniform_fill.value == fill &&
      info.stage.resources->uniform_fill.words == 1,
      "real existing IR did not prove complete literal fill; not a cost RED");
    Require(name, "native fill pipeline warm-up setup",
      context.GetPipelineCache().GetComputePipeline(info, program).pipeline != nullptr,
      "literal fill pipeline did not warm; not a cost RED");
    SetShader(primer, false);
    context.GetRenderExecutor().DispatchDirect(0, scheduler.Current(), 1,1,1, 0x41u);
    scheduler.Finish();
    Require(name, "GPU-owned destination setup", cache.IsRegionGpuModified(base + prefix, words * 4u),
      "native primer did not leave destination GPU-owned; not a cost RED");

    // Prove the independent primer natively without publishing any guest bytes.
    const auto owner = BufferCacheTestAccess::PageOwner(cache, base + prefix);
    Require(name, "native owner setup", bool(owner), "native primer has no cached owner");
    auto& source = cache.GetBuffer(owner);
    auto probe = CreateHostBuffer(name, words * sizeof(u32), vk::BufferUsageFlagBits::eTransferDst, {});
    const auto native = scheduler.Current().Handle();
    vk::MemoryBarrier before{};
    before.srcAccessMask = vk::AccessFlagBits::eMemoryWrite;
    before.dstAccessMask = vk::AccessFlagBits::eTransferRead;
    native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
      vk::PipelineStageFlagBits::eTransfer, {}, 1,&before, 0,nullptr, 0,nullptr);
    const vk::BufferCopy copy{source.Offset(base + prefix), 0, words * sizeof(u32)};
    native.copyBuffer(source.Handle(), probe.buffer, 1, &copy);
    vk::MemoryBarrier after{};
    after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    after.dstAccessMask = vk::AccessFlagBits::eHostRead;
    native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
      vk::PipelineStageFlagBits::eHost, {}, 1,&after, 0,nullptr, 0,nullptr);
    scheduler.Finish();
    const auto previous = ReadBuffer(name, probe, words);
    for (u32 i = 0; i < words; ++i)
      Require(name, "independent native primer values", previous[i] == old_bias + i,
        "primer GPU contents differ before the fill; not a cost RED");
    DestroyBuffer(&probe);
    Require(name, "ordinary mapped target setup",
      context.IsMapped(base + prefix, words * 4u) &&
      !context.GetTextureCache().IsRegionRegistered(base + prefix, words * 4u) &&
      !context.GetTextureCache().IsMeta(base + prefix),
      "target overlaps an image/metadata owner; not a cost RED");

    SetShader(code, true);
    Require(name, "isolated CPU marker setup", GpuZones::g_marker == nullptr,
            "foreign GPU marker is installed; not a cost RED");
    {
      struct MarkerScope {
        ~MarkerScope() { GpuZones::g_marker = nullptr; GpuZones::g_context = nullptr; }
      } marker;
      GpuZones::g_marker = &Counts::Mark;
      GpuZones::g_context = &counts;
      const auto recording_tick = scheduler.CurrentTick();
      context.GetRenderExecutor().DispatchDirect(0, scheduler.Current(), 1,1,1, 0x41u);
      std::array<u32, words> before_fence{};
      Require(name, "backing observation setup",
        LibKernel::Memory::TryReadBacking(base + prefix, before_fence.data(), sizeof(before_fence)),
        "kernel backing could not be observed before completion");
      cpu_old_before_fence = scheduler.CurrentTick() == recording_tick &&
        std::ranges::all_of(before_fence, [](u32 word) { return word == sentinel; });
      native_dirty_after = cache.IsRegionGpuModified(base + prefix, words * 4u);
    }
    scheduler.Finish();
    cache.ReadMemory(base, 128u * sizeof(u32));
    scheduler.Finish();
    std::array<u32,128> actual{};
    Require(name, "actual result publication setup",
      LibKernel::Memory::TryReadBacking(base, actual.data(), sizeof(actual)),
      "fill results were not readable after native completion");
    correct = true;
    for (u32 i = 0; i < actual.size(); ++i) {
      const auto expected = i >= prefix / 4 && i < prefix / 4 + words ? fill : sentinel;
      correct &= actual[i] == expected;
    }
    context.UnmapMemory(base, bytes);
  });
  context.ShutdownGpu();
  LibKernel::Memory::InstallGpuResources(nullptr);
  Require(name, "owned mapping cleanup", LibKernel::Memory::KernelMunmap(base, bytes) == 0,
          "owned mapping cleanup failed");
  Require(name, "owned allocation cleanup",
          LibKernel::Memory::KernelReleaseDirectMemory(direct, bytes) == 0,
          "owned allocation cleanup failed");
  Require(name, "CPU remains old until native completion", cpu_old_before_fence && native_dirty_after,
          "literal fill bypassed native GPU ownership or published CPU bytes before the fence");
  Require(name, "exact fill and surrounding native sentinels", correct,
          "literal fill changed values or wrote outside its guest range");
  std::printf("[cost] native ordinary fill GameDispatch=%u BufferCopy=%u CPUold=1\n",
              counts.game_dispatch, counts.buffer_copy);
  Require(name, "DMA-equivalent fill emits no compute kernel", counts.game_dispatch == 0,
          "proven GPU-owned literal fill still executes the redundant compute kernel");
  Require(name, "native transfer work remains", counts.buffer_copy != 0,
          "fill lost its native buffer-transfer command");
}
