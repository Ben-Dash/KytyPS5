// Included inside VulkanHarness, after the ordinary CPU/GPU texture helpers.
// Upstream contract: b7b335ac632d63be3c8a5f64339950bfc99cac8d.
static void CheckBcR64TextureAdmission() {
  constexpr const char* name = "BcR64TextureAdmission";
  using F = Prospero::BufferFormat;
  using T = Prospero::TileMode;
  struct Policy { F format; u32 bytes; bool depth_supported; };
  constexpr Policy policy[] = {
      {F::k8Srgb, 1, true}, {F::k8_8Srgb, 2, true},
      {F::k9_9_9_5Float, 4, true}, {F::kBc1Srgb, 8, false},
      {F::kBc7Srgb, 16, false}, {F::k32_32_32_32Float, 16, false},
  };
  // Texture tiling is independent of whether the format is a color attachment.
  // Check BC first so the intended RED specifically identifies compressed R64.
  for (const auto format : {F::kBc1Srgb, F::kBc7Srgb}) {
    TileTextureBlockLayout layout{};
    Require(name, "compressed R64 admission",
            TileGetTextureBlockLayout(format, T::kRenderTarget, false, layout),
            "block-compressed texture was rejected by R64 tiling");
  }
  for (const auto& item : policy) {
    for (const auto tile : {T::kRenderTarget, T::kDepth}) {
      TileTextureBlockLayout layout{};
      const bool supported =
          TileGetTextureBlockLayout(item.format, tile, false, layout);
      const bool expected = tile == T::kRenderTarget || item.depth_supported;
      Require(name, "public texture tiling policy",
              supported == expected &&
                  (!supported || layout.block.bytes_per_element == item.bytes),
              "texture tiling incorrectly used color-attachment format policy");
    }
  }
  TileSurfaceLayout surface{};
  const TileSurfaceDescription large{F::kBc1Srgb, T::kRenderTarget,
      TileSurfaceDimension::Dim2D, 2048, 2048, 1, 1, 8};
  Require(name, "BC1 eight-layer footprint",
          TileGetTiledTextureLayout(large, surface) &&
              surface.total_size == 0x1000000 &&
              surface.block_slice_size == 0x200000 &&
              surface.first_tail_level == 1 &&
              surface.texture.block.block_width == 128 &&
              surface.texture.block.block_height == 64 &&
              surface.mips[0].width == 512 && surface.mips[0].height == 512,
          "compressed block extent or array stride was lost");
  for (const auto format : {F::kBc1Srgb, F::kBc7Srgb}) {
    const TileSurfaceDescription partial_surface{format, T::kRenderTarget,
        TileSurfaceDimension::Dim2D, 67, 51, 1, 1, 8};
    Require(name, "partial compressed block footprint",
            TileGetTiledTextureLayout(partial_surface, surface) &&
                surface.total_size == 8u * 65536u &&
                surface.block_slice_size == 65536u &&
                surface.mips[0].width == 17 && surface.mips[0].height == 13,
            "partial BC texels or eight-layer allocation changed");
  }
  TileTextureBlockLayout invalid{};
  Require(name, "unsupported RGB32 boundary",
          !TileGetTextureBlockLayout(F::k32_32_32Float, T::kRenderTarget,
                                     false, invalid),
          "12-byte elements entered the power-of-two tile backend");
  std::printf("[host]    %-32s ok\n", name);
}

void CheckBcR64TextureGpuRoundTrip() {
  constexpr const char* name = "BcR64TextureGpuRoundTrip";
  // Actual public admission is checked before any native test buffer allocation.
  CheckBcR64TextureAdmission();
  EnsureRuntimeContext();
  CommandScheduler scheduler(Renderer(), m_runtime_context);
  HW::Context registers{};
  HW::UserConfig user_config{};
  HW::Shader shaders{};
  scheduler.Begin(registers, user_config, shaders);
  StreamBuffer parameters(m_runtime_context, scheduler, MemoryUsage::Stream, 1u << 20);
  TileManager tiler(m_runtime_context, scheduler, parameters);
  const auto host_barrier = [&](vk::Buffer buffer, uint64_t size,
                                 vk::PipelineStageFlags stage, vk::AccessFlags access) {
    vk::BufferMemoryBarrier barrier{};
    barrier.srcAccessMask = access;
    barrier.dstAccessMask = vk::AccessFlagBits::eHostRead;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer;
    barrier.size = size;
    scheduler.Current().Handle().pipelineBarrier(
        stage, vk::PipelineStageFlagBits::eHost, {}, 0, nullptr, 1, &barrier, 0, nullptr);
  };
  const auto words = [](const std::vector<uint8_t>& bytes) {
    std::vector<u32> result(bytes.size() / sizeof(u32));
    std::memcpy(result.data(), bytes.data(), bytes.size());
    return result;
  };
  const auto fill = [](std::vector<uint8_t>& bytes, u32 salt) {
    for (size_t i = 0; i < bytes.size(); ++i) {
      // Values vary across every byte, block, row and layer.
      bytes[i] = static_cast<uint8_t>((i * 37u) ^ (i >> 8u) ^ (i >> 16u) ^ salt);
    }
  };
  for (const auto format : {Prospero::BufferFormat::kBc1Srgb,
                            Prospero::BufferFormat::kBc7Srgb}) {
    constexpr auto tile = Prospero::TileMode::kRenderTarget;
    constexpr u32 width = 67, height = 51, layers = 8;
    TileSizeAlign total{};
    TileGetTextureTotalSize(format, width, height, layers, 1, tile, false, total);
    const auto layout = TextureCalcUploadLayout(format, width, height, 1, layers,
                                                 tile, total.size, false, name);
    const auto copies = TextureBuildImageCopies(layout);
    std::vector<GpuTileInfo> infos;
    Require(name, "current public layout builders",
            TextureBuildGpuTileInfos(total.size, copies, layout, 1, infos) &&
                total.size == layers * 65536u && infos.size() == layers,
            "current layout builders lost compressed array slices");
    uint64_t linear_size = 0;
    for (u32 layer = 0; layer < layers; ++layer) {
      const auto& info = infos[layer];
      Require(name, "actual layer tile infos",
              info.family == TileBlockFamily::RenderTarget64KB &&
                  info.width == 17 && info.height == 13 &&
                  info.surface_z == layer && info.tiled_offset == layer * 65536ull &&
                  info.bytes_per_element == (format == Prospero::BufferFormat::kBc1Srgb ? 8u : 16u),
              "partial block geometry, bytes or absolute layer XOR input changed");
      linear_size = std::max(linear_size, info.linear_offset + info.linear_size);
    }
    Require(name, "bounded allocation", linear_size != 0 && linear_size <= (1u << 20),
            "synthetic BC case exceeded its predeclared one-MiB linear budget");
    // CPU reference uses validated address helpers, while fixed public footprints
    // above independently constrain the layout. GPU loops are finite17x13x8.
    const auto reference = [&](bool to_tiled, std::vector<uint8_t>& dst,
                                const std::vector<uint8_t>& src) {
      for (const auto& info : infos) {
        TileBlockLayout block{};
        Require(name, "CPU block layout",
                TileGetBlockLayout(info.family, info.bytes_per_element, block),
                "CPU reference rejected an admitted tile layout");
        for (u32 y = 0; y < info.height; ++y) {
          for (u32 x = 0; x < info.width; ++x) {
            u32 local = 0, block_xor = 0;
            Require(name, "CPU block offsets",
                    TileGetBlockOffset(block, x, y, 0, local) &&
                        TileGetBlockXor(block, 0, 0, info.surface_z, block_xor),
                    "CPU reference rejected an in-range compressed block");
            const auto tiled = info.tiled_offset + (local ^ block_xor);
            const auto linear = info.linear_offset +
                (static_cast<uint64_t>(y) * info.pitch + x) * info.bytes_per_element;
            const auto output = to_tiled ? tiled : linear;
            const auto input = to_tiled ? linear : tiled;
            Require(name, "CPU reference bounds",
                    output + info.bytes_per_element <= dst.size() &&
                        input + info.bytes_per_element <= src.size(),
                    "CPU reference escaped declared transfer ranges");
            std::memcpy(dst.data() + output, src.data() + input, info.bytes_per_element);
          }
        }
      }
    };
    std::vector<uint8_t> tiled_bytes(total.size);
    fill(tiled_bytes, static_cast<u32>(format));
    std::vector<uint8_t> expected_linear(linear_size, 0);
    reference(false, expected_linear, tiled_bytes);
    auto input = CreateHostBuffer(name, total.size, AllFlags, words(tiled_bytes));
    auto output = CreateHostBuffer(name, linear_size, AllFlags,
        std::vector<u32>(linear_size / sizeof(u32), 0xababababu));
    const auto detiled = tiler.Detile(input.buffer, 0, total.size, linear_size, infos);
    const vk::BufferCopy copy{detiled.offset, 0, linear_size};
    scheduler.Current().Handle().copyBuffer(detiled.buffer, output.buffer, 1, &copy);
    host_barrier(output.buffer, linear_size, vk::PipelineStageFlagBits::eTransfer,
                   vk::AccessFlagBits::eTransferWrite);
    scheduler.Finish();
    const auto actual_linear = ReadBuffer(name, output, linear_size / sizeof(u32));
    DestroyBuffer(&output);
    DestroyBuffer(&input);
    Require(name, "GPU detile complete array and padding",
            actual_linear == words(expected_linear),
            "GPU detile changed compressed bytes, layer offsets or padding");

    std::vector<uint8_t> linear_bytes(linear_size);
    fill(linear_bytes, static_cast<u32>(format) ^ 0x51u);
    std::vector<uint8_t> expected_tiled(total.size, 0xab);
    reference(true, expected_tiled, linear_bytes);
    input = CreateHostBuffer(name, linear_size, AllFlags, words(linear_bytes));
    output = CreateHostBuffer(name, total.size, AllFlags,
        std::vector<u32>(total.size / sizeof(u32), 0xababababu));
    tiler.Tile(input.buffer, 0, linear_size, output.buffer, 0, total.size, infos);
    host_barrier(output.buffer, total.size, vk::PipelineStageFlagBits::eComputeShader,
                   vk::AccessFlagBits::eShaderWrite);
    scheduler.Finish();
    const auto actual_tiled = ReadBuffer(name, output, total.size / sizeof(u32));
    DestroyBuffer(&output);
    DestroyBuffer(&input);
    Require(name, "GPU tile complete array and untouched padding",
            actual_tiled == words(expected_tiled),
            "GPU tile changed compressed bytes, layer XOR or untouched padding");
  }
  scheduler.Finish();
  std::printf("[gpu]     %-32s ok (BC1/BC7, eight layers, two directions)\n", name);
}
