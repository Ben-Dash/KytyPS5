// Test-only proposal: include INSIDE VulkanHarness before CheckNativeIndirectDispatch.
// Uses the production GPU thread, command processor, memory protection, ordinary
// and BDA writers, renderer, native color attachment, and independent GPU readback.
  void CheckGpuCountedMultiDraw(bool include_bda = true) {
    constexpr const char* name = "GpuCountedMultiDraw";
    constexpr uintptr_t base = 0x0000000206c00000ull;
    constexpr uint64_t allocation_size = 0x100000u;
    constexpr uint64_t count_address = base + 0x4000u; // its own aligned BDA window
    constexpr uint64_t args_address = base + 0x10000u;
    constexpr uint64_t once_address = base + 0x18000u;
    constexpr uint64_t vertex_mask_address = base + 0x1c000u;
    constexpr uint64_t vertex_address = base + 0x20000u;
    constexpr uint64_t color_address = base + 0x40000u;
    constexpr uint64_t index_address = base + 0x50000u;
    constexpr uint32_t width = 96u, height = 32u, stride = 32u, max_draws = 3u;
    constexpr uint32_t allocated_args = 5u;
    constexpr uint32_t guard = 0x8badf00du;

    // Pos.xyzw + constant triangle RGB(A):9 distinct records, all real guest data.
    std::array<float, 9u * 8u> vertex_data{};
    for (uint32_t draw = 0; draw < 3u; ++draw) {
      const float left = -1.0f + float(draw) * (2.0f / 3.0f);
      const std::array<std::array<float, 2>, 3> triangle{{
          {left + 0.02f, -0.75f}, {left + 2.0f / 3.0f - 0.02f, -0.75f},
          {left + 1.0f / 3.0f, 0.75f}}};
      for (uint32_t corner = 0; corner < 3u; ++corner) {
        auto* record = vertex_data.data() + (draw * 3u + corner) * 8u;
        record[0] = triangle[corner][0]; record[1] = triangle[corner][1];
        record[2] = 0.0f; record[3] = 1.0f;
        for (uint32_t channel = 0; channel < 3u; ++channel)
          record[4u + channel] = channel == draw ? 1.0f : 0.0f;
        record[7] = 1.0f;
      }
    }
    std::vector<u32> vertex_code{EncodeVop1(0x01u, 20u, Vgpr(5u)),
        EncodeMubuf0(0x0eu, 0u, true, false), EncodeMubuf1(0u, 2u, 20u),
        EncodeMubuf0(0x0eu, 16u, true, false), EncodeMubuf1(4u, 2u, 20u)};
    AppendVMovU32(&vertex_code, 8u, 1u);
    vertex_code.push_back(EncodeVop2(0x1au, 8u, Vgpr(20u), 8u)); //1<<actual vertex index
    vertex_code.insert(vertex_code.end(), {
        EncodeMubuf0(0x3au, 0u, false, false), EncodeMubuf1(8u, 3u, 0u),
        EncodeExp0(0x0cu, 0xfu), EncodeExp1(0u, 1u, 2u, 3u),
        EncodeExp0(0x20u, 0xfu), EncodeExp1(4u, 5u, 6u, 7u)});
    AppendEnd(&vertex_code);
    std::vector<u32> pixel_code;
    for (u32 channel = 0; channel < 3u; ++channel) {
      pixel_code.push_back(EncodeVintrp(0u, 20u + channel, 0u, channel, 0u));
      pixel_code.push_back(EncodeVintrp(1u, 20u + channel, 0u, channel, 1u));
    }
    AppendVMovLiteral(&pixel_code, 23u, 0x3f800000u);
    pixel_code.insert(pixel_code.end(), {
        EncodeExp0(0u, 0xfu), EncodeExp1(20u, 21u, 22u, 23u)});
    AppendEnd(&pixel_code);
    static ShaderUserData native_user_data{};
    ShaderMapUserData(reinterpret_cast<uint64_t>(vertex_code.data()),
        {.type = Prospero::ShaderBinaryType::kGs, .user_data = &native_user_data,
         .code_size_bytes = u32(vertex_code.size() * sizeof(u32))});
    ShaderMapUserData(reinterpret_cast<uint64_t>(pixel_code.data()),
        {.type = Prospero::ShaderBinaryType::kPs,
         .code_size_bytes = u32(pixel_code.size() * sizeof(u32))});
    const auto make_writer = [&](bool bda, bool indexed) {
      std::vector<u32> code;
      code.push_back(EncodeVop1(0x01u, 1u, 12u)); // s12=N
      if (bda) {
        code.push_back(EncodeVop1(0x01u, 2u, 0u));
        code.push_back(EncodeVop1(0x01u, 3u, 1u));
        code.push_back(EncodeFlat0(0x1cu, 0u)); // FLAT_STORE_DWORD, not LOAD
        code.push_back(EncodeFlat1(0u, 0x7du, 1u, 2u));
      } else {
        code.push_back(EncodeMubuf0(0x1cu, 0u, false, false));
        code.push_back(EncodeMubuf1(1u, 0u, 0u));
      }
      for (u32 draw = 0; draw < 3u; ++draw) {
        const std::array<u32, 5> args{3u, 1u, draw * 3u, 0u, 0u};
        const u32 words = indexed ? 5u : 4u;
        for (u32 word = 0; word < words; ++word) {
          AppendVMovU32(&code, 1u, args[word]);
          code.push_back(EncodeMubuf0(0x1cu, draw * stride + word * sizeof(u32),
                                     false, false));
          code.push_back(EncodeMubuf1(1u, 1u, 0u));
        }
      }
      AppendVMovU32(&code, 1u, 1u);
      code.push_back(EncodeMubuf0(0x32u, 0u, false, false));
      code.push_back(EncodeMubuf1(1u, 2u, 0u)); // producer exactly once
      AppendEnd(&code);
      return code;
    };
    std::array<std::vector<u32>, 4> writers{
        make_writer(false, false), make_writer(false, true),
        make_writer(true, false), make_writer(true, true)};
    for (const auto& code : writers)
      ShaderMapUserData(reinterpret_cast<uint64_t>(code.data()),
          {.type = Prospero::ShaderBinaryType::kCs,
           .code_size_bytes = u32(code.size() * sizeof(u32))});

    EnsureRuntimeContext();
    int64_t direct_offset = -1;
    Require(name, "real guest allocation", LibKernel::Memory::KernelAllocateDirectMemory(
        0, LibKernel::Memory::KernelGetDirectMemorySize(), allocation_size, 0x10000u,
        0, &direct_offset) == 0, "allocation failed; not a semantic RED");
    void* mapped = reinterpret_cast<void*>(base);
    Require(name, "real guest mapping", LibKernel::Memory::KernelMapDirectMemory(
        &mapped, allocation_size, 0x3u, 0x10u, direct_offset, 0x10000u) == 0 &&
        mapped == reinterpret_cast<void*>(base), "mapping failed; not a semantic RED");
    std::memset(mapped, 0, allocation_size);
    std::memcpy(reinterpret_cast<void*>(vertex_address), vertex_data.data(), sizeof(vertex_data));
    for (u32 i = 0; i < 9u; ++i) reinterpret_cast<u32*>(index_address)[i] = i;

    struct Result {
      bool bda, indexed;
      u32 count, expected_draws, gpu_count_before, gpu_count_after, producer_once, vertex_mask;
      bool cpu_old_before, guards_match, rgb_match, border_match;
    };
    std::vector<Result> results;
    bool direct_controls_ok = true;
    {
      RenderContext context(m_runtime_context);
      context.InitializeGpu(nullptr);
      LibKernel::Memory::InstallGpuResources(&context);
      context.GetGpu().SendCommandSync([&] {
        CommandProcessor processor(context, 0);
        processor.BufferInit();
        context.MapMemory(base, allocation_size);
        auto& registers = processor.GetCtx();
        auto& user_config = processor.GetUcfg();
        auto& shaders = processor.GetShCtx();
        auto& scheduler = context.GetCommandScheduler();
        auto& cache = context.GetBufferCache();
        auto& textures = context.GetTextureCache();
        auto& executor = context.GetRenderExecutor();
        registers.SetViewportTransformControl(0x300u);
        registers.SetViewportScaleOffset(0u, width / 2.0f, width / 2.0f,
                                          height / 2.0f, height / 2.0f, 1.0f, 0.0f);
        registers.SetViewportZMax(0u, 1.0f);
        registers.SetScreenScissor(0, 0, width, height);
        registers.SetWindowScissor(0, 0, width, height, false);
        registers.SetGenericScissor(0, 0, width, height, false);
        registers.SetViewportScissor(0u, 0, 0, width, height, false);
        registers.SetRenderTargetMask(0xfu);
        registers.SetShaderMask(0xfu);
        registers.SetTargetOutputMode(0u, 4u);
        registers.SetModeControl({});
        registers.SetDepthControl({});
        registers.SetDepthShaderControl({});
        registers.SetColorBase(0u, {.addr = color_address});
        registers.SetColorInfo(0u, {
            .format = Prospero::ChannelLayout::k32_32_32_32,
            .channel_type = Prospero::ChannelType::kFloat,
            .channel_order = Prospero::ChannelOrder::kStandard});
        registers.SetColorAttrib2(0u, {.height = height - 1u, .width = width - 1u});
        registers.SetColorAttrib3(0u, {.tile_mode = Prospero::TileMode::kLinear,
                                     .dimension = 1u, .metadata_pipe_aligned = true});
        registers.SetPsInControl(0x8001u);
        registers.SetPsInputEna(2u);
        registers.SetPsInputAddr(2u);
        registers.SetPsInputSettings(0u, 0u);
        user_config.SetPrimitiveType(Prospero::PrimitiveType::kTriList);
        shaders.SetEsShaderBase(reinterpret_cast<uint64_t>(vertex_code.data()));
        shaders.SetPsShaderBase(reinterpret_cast<uint64_t>(pixel_code.data()));
        shaders.SetGsShaderResource2({.user_sgpr = 8u});
        const auto descriptor = [&](uint64_t address, u32 element_stride, u32 records,
                                    Prospero::BufferFormat format = Prospero::BufferFormat::k32UInt) {
          ShaderBufferResource d{};
          d.UpdateAddress48(address);
          d.fields[1] |= element_stride << 16u;
          d.fields[2] = records;
          d.fields[3] = DstSel(4, 5, 6, 7) |
              (u32(format) << 12u);
          return d;
        };
        const auto set_vs_buffer = [&](u32 sgpr, uint64_t address, u32 element_stride, u32 records) {
          const auto d = descriptor(address, element_stride, records,
              sgpr == 0u ? Prospero::BufferFormat::k32_32_32_32Float
                         : Prospero::BufferFormat::k32UInt);
          for (u32 word = 0; word < 4u; ++word)
            shaders.SetGsUserSgpr(sgpr + word, d.fields[word], HW::UserSgprType::Vsharp);
        };
        const auto set_cs_buffer = [&](u32 sgpr, uint64_t address, u32 bytes) {
          const auto d = descriptor(address, 0u, bytes);
          for (u32 word = 0; word < 4u; ++word)
            shaders.SetCsUserSgpr(sgpr + word, d.fields[word], HW::UserSgprType::Unknown);
        };
        set_vs_buffer(0u, vertex_address, 32u, 9u);
        set_vs_buffer(4u, vertex_mask_address, 0u, 4u);
        processor.SetIndexType(1u);
        processor.SetIndexBaseAddress(index_address);
        processor.SetIndexBufferSize(9u);
        processor.SetDrawIndirectArgsBaseAddress(args_address);
        RenderColorInfo target{};
        RenderExecutorTestAccess::ResolveRenderColorTarget(executor, scheduler.Current(), target, 0u);
        Require(name, "real color target shape", target.image_id &&
                    target.desc.info.extent == vk::Extent3D{width, height, 1u},
                "graphics setup failed; not a semantic RED");
        // PeekPage observes the owner's actual writable/BDA backing without
        // allocating, uploading, marking dirty, or refreshing guest memory.
        // A read-only ObtainBuffer can return an immutable stream snapshot.
        const auto record_count_probe = [&](const Buffer& probe, uint64_t destination_offset) {
          const auto [native_buffer, page_offset] = cache.PeekPage(count_address);
          Require(name, "producer registered actual count page", bool(native_buffer),
                  "producer has no native count owner; not a semantic RED");
          const auto count_offset = page_offset +
              (count_address & (BufferCache::CACHING_PAGESIZE - 1u));
          scheduler.EndRendering();
          auto command = scheduler.Current().Handle();
          vk::BufferMemoryBarrier ready{};
          ready.srcAccessMask = vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eMemoryWrite;
          ready.dstAccessMask = vk::AccessFlagBits::eTransferRead;
          ready.srcQueueFamilyIndex = ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          ready.buffer = native_buffer; ready.offset = count_offset; ready.size = sizeof(u32);
          command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                                  vk::PipelineStageFlagBits::eTransfer, {},
                                  0u, nullptr, 1u, &ready, 0u, nullptr);
          const vk::BufferCopy copy{count_offset, destination_offset, sizeof(u32)};
          command.copyBuffer(native_buffer, probe.buffer, 1u, &copy);
          // No Finish, host map, or guest download here. The first copy is
          // queued before the consumer; both results are read after it.
        };
        const auto read_count_probe = [&](const Buffer& probe) {
          auto command = scheduler.Current().Handle();
          vk::BufferMemoryBarrier host{};
          host.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
          host.dstAccessMask = vk::AccessFlagBits::eHostRead;
          host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          host.buffer = probe.buffer; host.size = probe.size;
          command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                  vk::PipelineStageFlagBits::eHost, {},
                                  0u, nullptr, 1u, &host, 0u, nullptr);
          scheduler.Finish();
          return ReadBuffer(name, probe, 2u);
        };

        // First prove the exact production graphics setup with CPU-written
        // arguments. A black target or wrong ABI here is not a COUNT RED.
        for (const bool indexed : {false, true}) {
          *reinterpret_cast<u32*>(vertex_mask_address) = 0u;
          std::memset(reinterpret_cast<void*>(args_address), 0, allocated_args * stride);
          for (u32 draw = 0; draw < max_draws; ++draw) {
            const std::array<u32, 5> args{3u, 1u, draw * 3u, 0u, 0u};
            std::memcpy(reinterpret_cast<void*>(args_address + draw * stride), args.data(),
                        (indexed ? 5u : 4u) * sizeof(u32));
          }
          TextureCacheTestAccess::ClearImage(textures, scheduler.Current(), target.image_id,
              {vk::ImageAspectFlagBits::eColor, 0u, 1u, 0u, 1u}, {});
          const std::array<u32, 9> packet{
              0u, Pm4::SH_NOP, Pm4::SH_NOP, 0u, max_draws, 0u, 0u, stride, 2u};
          Require(name, "direct-count PM4 positive control",
                  CpOpDrawIndirectMulti(processor, indexed ? 0xc0083800u : 0xc0082c00u,
                                        packet.data(), 0u, 0u) == 9u,
                  "invalid direct-count setup; not a semantic RED");
          const auto pixels = ReadCachedTexel(name, context, target.image_id,
                                              {}, {width, height, 1u});
          cache.ReadMemory(vertex_mask_address, 4u);
          u32 vertex_mask = 0u;
          direct_controls_ok &= pixels.size() == width * height * 4u &&
              LibKernel::Memory::TryReadBacking(vertex_mask_address, &vertex_mask, sizeof(vertex_mask)) &&
              vertex_mask == 0x1ffu;
          for (u32 draw = 0; direct_controls_ok && draw < max_draws; ++draw) {
            const auto pixel = 4u * ((height / 2u) * width + (16u + draw * 32u));
            for (u32 component = 0; component < 4u; ++component) {
              const float expected = component == draw || component == 3u ? 1.0f : 0.0f;
              direct_controls_ok &= std::abs(std::bit_cast<float>(pixels[pixel + component]) - expected) < 0.00001f;
            }
          }
          std::printf("[gpu] count-multi DIRECT indexed=%u RGB=%u VSmask=%08x\n",
                      indexed ? 1u : 0u, direct_controls_ok ? 1u : 0u, vertex_mask);
        }
        if (!direct_controls_ok) {
          context.UnmapMemory(base, allocation_size);
          scheduler.Finish();
          return;
        }

        for (const bool bda : {false, true}) {
          if (bda && !include_bda) continue;
          for (const bool indexed : {false, true}) {
            for (const u32 count : {0u, 1u, 2u, 3u, 5u}) {
              // Normal CPU writes exercise the real ownership fault/upload path.
              // Count and argument pages are distinct from shader tables/vertices.
              *reinterpret_cast<u32*>(count_address) = 0u;
              for (u32 i = 1; i < 4u; ++i) reinterpret_cast<u32*>(count_address)[i] = guard;
              // Two additional zero-work records keep even a failed max-count
              // boundary bounded; canaries follow all physically valid records.
              std::memset(reinterpret_cast<void*>(args_address), 0, allocated_args * stride);
              for (u32 i = 0; i < 4u; ++i)
                reinterpret_cast<u32*>(args_address + allocated_args * stride)[i] = guard;
              *reinterpret_cast<u32*>(once_address) = 0u;
              *reinterpret_cast<u32*>(vertex_mask_address) = 0u;
              for (u32 i = 1; i < 4u; ++i) {
                reinterpret_cast<u32*>(once_address)[i] = guard;
                reinterpret_cast<u32*>(vertex_mask_address)[i] = guard;
              }
              TextureCacheTestAccess::ClearImage(textures, scheduler.Current(), target.image_id,
                  {vk::ImageAspectFlagBits::eColor, 0u, 1u, 0u, 1u}, {});
              const auto& writer = writers[(bda ? 2u : 0u) + (indexed ? 1u : 0u)];
              shaders.SetCsShader({.data_addr = reinterpret_cast<uint64_t>(writer.data()),
                                   .num_thread_x = 1u, .num_thread_y = 1u, .num_thread_z = 1u,
                                   .wave_size = 64u, .user_sgpr = 13u});
              set_cs_buffer(0u, count_address, 4u);
              set_cs_buffer(4u, args_address, max_draws * stride);
              set_cs_buffer(8u, once_address, 4u);
              shaders.SetCsUserSgpr(12u, count, HW::UserSgprType::Unknown);
              auto count_probe = CreateHostBuffer(name, 2u * sizeof(u32),
                  vk::BufferUsageFlagBits::eTransferDst, {UINT32_MAX, UINT32_MAX});
              processor.DispatchDirect(1u, 1u, 1u, 0x41u);
              record_count_probe(count_probe, 0u);
              u32 cpu_shadow = UINT32_MAX;
              // Observation only; never used as the consumer count.
              const bool cpu_old = LibKernel::Memory::TryReadBacking(
                  count_address, &cpu_shadow, sizeof(cpu_shadow)) && cpu_shadow == 0u;
              const std::array<u32, 9> packet{
                  0u, Pm4::SH_NOP, Pm4::SH_NOP, 1u << 30u, max_draws,
                  u32(count_address), u32(count_address >> 32u), stride, 2u};
              Require(name, "actual PM4 multi-draw parser",
                      CpOpDrawIndirectMulti(processor, indexed ? 0xc0083800u : 0xc0082c00u,
                                            packet.data(), 0u, 0u) == 9u,
                      "valid packet failed; not a semantic RED");
              record_count_probe(count_probe, sizeof(u32));
              const auto gpu_values = read_count_probe(count_probe);
              DestroyBuffer(&count_probe);
              const auto pixels = ReadCachedTexel(name, context, target.image_id,
                                                   {}, {width, height, 1u});
              cache.ReadMemory(count_address, 16u);
              cache.ReadMemory(args_address, allocated_args * stride + 16u);
              cache.ReadMemory(once_address, 16u);
              cache.ReadMemory(vertex_mask_address, 16u);
              std::array<u32, 4> count_words{}, once_words{}, vertex_calls{}, args_guards{};
              Require(name, "post-consumer ordinary witness readback",
                      LibKernel::Memory::TryReadBacking(count_address, count_words.data(), 16u) &&
                      LibKernel::Memory::TryReadBacking(once_address, once_words.data(), 16u) &&
                      LibKernel::Memory::TryReadBacking(vertex_mask_address, vertex_calls.data(), 16u) &&
                      LibKernel::Memory::TryReadBacking(args_address + allocated_args * stride,
                                                       args_guards.data(), 16u),
                      "witness memory unreadable; not a semantic RED");
              const u32 expected_draws = std::min(count, max_draws);
              bool rgb = pixels.size() == width * height * 4u;
              for (u32 draw = 0; rgb && draw < max_draws; ++draw) {
                const auto pixel = 4u * ((height / 2u) * width + (16u + draw * 32u));
                for (u32 component = 0; component < 4u; ++component) {
                  const float expected = draw < expected_draws &&
                      (component == draw || component == 3u) ? 1.0f : 0.0f;
                  rgb &= std::abs(std::bit_cast<float>(pixels[pixel + component]) - expected) < 0.00001f;
                }
              }
              bool border = pixels.size() == width * height * 4u;
              for (u32 x = 0; border && x < width; ++x)
                for (u32 component = 0; component < 4u; ++component)
                  border &= pixels[x * 4u + component] == 0u &&
                      pixels[((height - 1u) * width + x) * 4u + component] == 0u;
              bool guards = std::all_of(args_guards.begin(), args_guards.end(),
                                       [&](u32 word) { return word == guard; });
              for (u32 i = 1; i < 4u; ++i)
                guards &= count_words[i] == guard && once_words[i] == guard && vertex_calls[i] == guard;
              results.push_back({bda, indexed, count, expected_draws, gpu_values[0], gpu_values[1],
                                  once_words[0], vertex_calls[0], cpu_old, guards, rgb, border});
              std::printf("[gpu] count-multi bda=%u indexed=%u count=%u max=%u "
                          "GPUbefore=%u GPUafter=%u CPUold=%u producer=%u VSmask=%08x RGB=%u guards=%u\n",
                          bda ? 1u : 0u, indexed ? 1u : 0u, count, max_draws,
                          gpu_values[0], gpu_values[1],
                          cpu_old ? 1u : 0u, once_words[0], vertex_calls[0],
                          rgb ? 1u : 0u, guards && border ? 1u : 0u);
            }
          }
        }
        context.UnmapMemory(base, allocation_size);
        scheduler.Finish();
      });
      LibKernel::Memory::InstallGpuResources(nullptr);
      context.ShutdownGpu();
    }
    Require(name, "owned guest cleanup", LibKernel::Memory::KernelMunmap(base, allocation_size) == 0 &&
                LibKernel::Memory::KernelReleaseDirectMemory(direct_offset, allocation_size) == 0,
            "synthetic allocation cleanup failed");

    // Numerical assertions happen after all task-owned rendering resources and
    // mappings are drained/released. A correct baseline closes this lead.
    Require(name, "CPU-written direct-count RGB controls",
            direct_controls_ok, "graphics/ABI setup is not valid; not an intended COUNT RED");
    Require(name, "all bounded count cases ran", results.size() == (include_bda ? 20u : 10u),
            "fixture did not execute all cases; not an intended COUNT RED");
    for (const auto& result : results) {
      Require(name, "producer completed and CPU shadow stayed old before consumer",
              result.gpu_count_before == result.count && result.gpu_count_after == result.count &&
                  result.producer_once == 1u && result.cpu_old_before,
              "not intended RED: producer/input setup did not preserve the independent witness");
      Require(name, "actual RGB coverage and independent vertex-index mask",
              result.rgb_match && result.vertex_mask == ((1u << (3u * result.expected_draws)) - 1u),
              "intended RED: positive GPU count did not execute exactly min(count,max) RGB draws");
      Require(name, "complete count/argument/producer/draw guards",
              result.guards_match && result.border_match, "indirect draw changed independent canaries");
    }
    std::printf("[gpu] GpuCountedMultiDraw %zu bounded ownership-preserving variants PASS\n", results.size());
  }
