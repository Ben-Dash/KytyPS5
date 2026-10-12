// UNCOMPILED neighbor proposal. Include after BcAliasOwnershipTests.inc.
// Requires no new friend accessor; VulkanHarness(true) is required by the tiler.
// One actual integer-image texel overwrites one BC7 block in a packed tail.
namespace Libs::Graphics {
namespace {

void CheckBcAliasPartialTailPaddingGpu(VulkanHarness& vulkan) {
  constexpr const char* name = "Bc7PartialTailPaddingOwnership";
  constexpr uint64_t base = 0x00000002c0200000ull, bytes = 0x40000u;
  constexpr uint64_t address = base + 0x10000u;
  constexpr uint32_t width = 128u, levels = 8u, layers = 3u, block_bytes = 16u;
  constexpr uint32_t modified_layer = 2u, marker = 0x13579bdfu;
  auto array = BcAliasDesc(name, address, vk::Format::eBc7UnormBlock,
      Prospero::BufferFormat::kBc7UNorm, width, width, levels, layers, block_bytes,
      TextureCache::BindingType::Texture);
  const auto layout = TextureCalcUploadLayout(array.info.guest_format, width, width,
      levels, layers, array.info.tile_mode, array.info.data.size, false, name);
  const uint32_t tail_level = layout.surface.first_tail_level;
  Require(name, "bounded shared packed tail", tail_level == 2u &&
      layout.surface.block_slice_size == 24576u && array.info.data.size == 73728u,
      "fixture must retain the fixed BC7 tail geometry proved in the primary case");
  const auto& tail = layout.surface.mips[tail_level];
  const uint64_t tail_address = address + modified_layer * layout.surface.block_slice_size + tail.offset;
  auto raw = BcAliasDesc(name, tail_address, vk::Format::eR32G32B32A32Uint,
      Prospero::BufferFormat::k32_32_32_32UInt, tail.padded_width, tail.padded_height,
      1u, 1u, block_bytes, TextureCache::BindingType::Storage);
  Require(name, "one4KiB raw tail image", raw.info.data.size == 4096u &&
      raw.info.extent.width == 16u && raw.info.extent.height == 16u &&
      tail.tail_x < 16u && tail.tail_y < 16u,
      "integer image must represent exactly the packed tail's storage block");
  int64_t direct = -1;
  Require(name, "guest allocation", LibKernel::Memory::KernelAllocateDirectMemory(
      0, LibKernel::Memory::KernelGetDirectMemorySize(), bytes, 0x10000u, 0, &direct) == 0,
      "guest allocation failed; not a tail padding RED");
  void* mapped = reinterpret_cast<void*>(base);
  Require(name, "guest mapping", LibKernel::Memory::KernelMapDirectMemory(
      &mapped, bytes, 0x3, 0x10, direct, 0x10000u) == 0 && mapped == reinterpret_cast<void*>(base),
      "guest mapping failed; not a tail padding RED");
  std::memset(mapped, 0xff, bytes);
  auto& context = vulkan.RuntimeRenderer();
  auto& scheduler = context.GetCommandScheduler();
  auto& textures = context.GetTextureCache();
  auto& buffers = context.GetBufferCache();
  HW::Context registers{};
  HW::UserConfig user_config{};
  HW::Shader shaders{};
  scheduler.Begin(registers, user_config, shaders);
  LibKernel::Memory::InstallGpuResources(&context);
  context.MapMemory(base, bytes);
  (void)buffers.ObtainBuffer(base, bytes, false);
  // The pre-existing sampled consumer tests the reverse alias-retirement path.
  const auto original_array = textures.FindImage(array);
  Require(name, "initial sampled BC array", textures.FindTexture(original_array, array) != nullptr,
          "initial BC consumer must be an actual native image");
  const auto producer = textures.FindImage(raw);
  Require(name, "actual raw producer", textures.FindTexture(producer, raw) != nullptr,
          "raw producer must be an actual initialized native image");
  {
    FetchGpuBuffer upload(vulkan.RuntimeContext(), block_bytes, vk::BufferUsageFlagBits::eTransferSrc);
    std::fill_n(static_cast<uint32_t*>(upload.data), 4u, marker);
    upload.Flush();
    vk::BufferMemoryBarrier2 host{};
    host.srcStageMask = vk::PipelineStageFlagBits2::eHost;
    host.srcAccessMask = vk::AccessFlagBits2::eHostWrite;
    host.dstStageMask = vk::PipelineStageFlagBits2::eCopy;
    host.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = upload.buffer;
    host.size = block_bytes;
    vk::DependencyInfo dependency{};
    dependency.bufferMemoryBarrierCount = 1u;
    dependency.pBufferMemoryBarriers = &host;
    scheduler.Current().Handle().pipelineBarrier2(dependency);
    vk::BufferImageCopy overwrite{};
    overwrite.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0u, 0u, 1u};
    overwrite.imageOffset = {static_cast<int32_t>(tail.tail_x), static_cast<int32_t>(tail.tail_y), 0};
    overwrite.imageExtent = {1u, 1u, 1u};
    textures.GetImage(producer).Upload(std::span{&overwrite, size_t{1}}, upload.buffer,
                                      0u, block_bytes, block_bytes);
    textures.MarkGpuWritten(producer);
    scheduler.Finish(); // upload source cannot die before its finite copy completes
  }
  const auto read_image = [&](ImageId id, const std::vector<vk::BufferImageCopy>& copies,
                              uint64_t read_bytes) {
    FetchGpuBuffer result(vulkan.RuntimeContext(), read_bytes, vk::BufferUsageFlagBits::eTransferDst);
    textures.GetImage(id).Download(copies, result.buffer, 0u, read_bytes, read_bytes);
    BcAliasHostReadBarrier(scheduler, result.buffer, read_bytes);
    scheduler.Finish();
    result.Invalidate();
    std::vector<uint32_t> actual(read_bytes / sizeof(uint32_t));
    std::memcpy(actual.data(), result.data, read_bytes);
    return actual;
  };
  vk::BufferImageCopy raw_copy{};
  raw_copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0u, 0u, 1u};
  raw_copy.imageExtent = raw.info.extent;
  const auto raw_actual = read_image(producer, {raw_copy}, raw.info.data.size);
  std::vector<uint32_t> raw_expected(4096u / sizeof(uint32_t), UINT32_MAX);
  const auto raw_word = (tail.tail_y * raw.info.extent.width + tail.tail_x) * 4u;
  std::fill_n(raw_expected.begin() + raw_word, 4u, marker);

  std::vector<vk::BufferImageCopy> copies;
  std::vector<uint32_t> expected;
  for (uint32_t layer = 0; layer < layers; ++layer) {
    for (uint32_t level = 0; level < levels; ++level) {
      const auto mip_width = std::max(width >> level, 1u);
      const auto blocks = ((mip_width + 3u) / 4u) * ((mip_width + 3u) / 4u);
      vk::BufferImageCopy copy{};
      copy.bufferOffset = expected.size() * sizeof(uint32_t);
      copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, level, layer, 1u};
      copy.imageExtent = {mip_width, mip_width, 1u};
      copies.push_back(copy);
      expected.insert(expected.end(), blocks * 4u, UINT32_MAX);
      // The chosen raw texel is exactly the first compressed block of this mip.
      if (layer == modified_layer && level == tail_level) {
        std::fill_n(expected.begin() + copy.bufferOffset / sizeof(uint32_t), 4u, marker);
      }
    }
  }
  const auto consumer = textures.FindImage(array);
  Require(name, "replacement BC consumer", textures.FindTexture(consumer, array) != nullptr,
          "replacement BC consumer must be a real native array");
  const auto actual = read_image(consumer, copies, expected.size() * sizeof(uint32_t));
  FetchGpuBuffer tail_download(vulkan.RuntimeContext(), 4096u, vk::BufferUsageFlagBits::eTransferDst);
  auto& owner = buffers.GetBuffer(buffers.FindBuffer(base, bytes));
  vk::BufferMemoryBarrier2 ready{};
  ready.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands;
  ready.srcAccessMask = vk::AccessFlagBits2::eMemoryWrite;
  ready.dstStageMask = vk::PipelineStageFlagBits2::eCopy;
  ready.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
  ready.srcQueueFamilyIndex = ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  ready.buffer = owner.Handle();
  ready.offset = owner.Offset(tail_address);
  ready.size = 4096u;
  vk::DependencyInfo dependency{};
  dependency.bufferMemoryBarrierCount = 1u;
  dependency.pBufferMemoryBarriers = &ready;
  scheduler.Current().Handle().pipelineBarrier2(dependency);
  const vk::BufferCopy tail_copy{owner.Offset(tail_address), 0u, 4096u};
  scheduler.Current().Handle().copyBuffer(owner.Handle(), tail_download.buffer, 1u, &tail_copy);
  BcAliasHostReadBarrier(scheduler, tail_download.buffer, 4096u);
  scheduler.Finish();
  tail_download.Invalidate();
  std::vector<uint32_t> tiled_actual(4096u / sizeof(uint32_t));
  std::memcpy(tiled_actual.data(), tail_download.data, 4096u);
  std::vector<uint32_t> tiled_expected(4096u / sizeof(uint32_t), UINT32_MAX);
  uint32_t tiled_offset = 0;
  Require(name, "typed CPU address reference", TileGetBlockOffset(layout.surface.texture.block,
      tail.tail_x, tail.tail_y, 0u, tiled_offset) && tiled_offset <= 4096u - block_bytes,
      "reference address helper rejected the selected tail texel");
  std::fill_n(tiled_expected.begin() + tiled_offset / sizeof(uint32_t), 4u, marker);
  std::vector<uint32_t> backing(array.info.data.size / sizeof(uint32_t));
  const bool cpu_white = LibKernel::Memory::TryReadBacking(address, backing.data(), array.info.data.size) &&
      std::ranges::all_of(backing, [](uint32_t value) { return value == UINT32_MAX; });
  context.UnmapMemory(base, bytes);
  LibKernel::Memory::InstallGpuResources(nullptr);
  Require(name, "guest cleanup", LibKernel::Memory::KernelMunmap(base, bytes) == 0 &&
      LibKernel::Memory::KernelReleaseDirectMemory(direct, bytes) == 0, "guest cleanup failed");
  std::printf("[gpu] %s tail=%u rawPixel=%u,%u tiledByte=%u raw=%u cpuWhite=%u "
              "BC=%u fullTail=%u unchangedPaddingBytes=4080\n", name, tail_level,
      tail.tail_x, tail.tail_y, tiled_offset, raw_actual == raw_expected, cpu_white,
      actual == expected, tiled_actual == tiled_expected);
  Require(name, "independent partial native write control", raw_actual == raw_expected && cpu_white,
          "one real raw texel must change while CPU and255 other raw texels remain white");
  Require(name, "complete BC consumer blocks", actual == expected,
          "partial raw write was lost or changed another BC block/mip/layer");
  Require(name, "all untouched tail bytes", tiled_actual == tiled_expected,
          "publication changed any of the4080 native tail bytes outside the single16-byte write");
}

} // namespace
} // namespace Libs::Graphics
