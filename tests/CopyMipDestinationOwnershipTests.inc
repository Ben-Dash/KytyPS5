// UNCOMPILED proposal. Include after BcAliasOwnershipTests.inc.
// Root must add this method to its existing TextureCacheTestAccess (test only):
// static void CopyImageMip(TextureCache& cache, ImageId dst, ImageId src,
//                          uint32_t level, uint32_t layer) {
//   std::scoped_lock lock(cache.m_lock);
//   cache.CopyImageMip(dst, src, level, layer);
// }
// Production destination RefreshImage must remain absent for the initial RED.
namespace Libs::Graphics {
namespace {

void CheckCopyMipDestinationOwnershipGpu(VulkanHarness& vulkan, bool neighbors = false) {
  constexpr const char* name = "CopyMipDestinationBufferOwnership";
  constexpr uint64_t base = 0x00000002c0100000ull, bytes = 0x40000u;
  constexpr uint64_t destination_address = base + 0x10000u, source_address = base + 0x30000u;
  constexpr uint32_t baseline = 0x44332211u, copied = 0x88776655u;
  constexpr uint32_t width = 8u, levels = 3u, layers = 3u;
  constexpr std::array<uint32_t, 3> test_levels{1u, 0u, 2u}, test_layers{2u, 0u, 1u};
  for (uint32_t test = 0; test < (neighbors ? 3u : 1u); ++test) {
    const uint32_t target_level = test_levels[test], target_layer = test_layers[test];
    const uint32_t source_width = width >> target_level;
    auto destination = BcAliasDesc(name, destination_address, vk::Format::eR32Uint,
        Prospero::BufferFormat::k32UInt, width, width, levels, layers, 4u,
        TextureCache::BindingType::Texture);
    auto source = BcAliasDesc(name, source_address, vk::Format::eR32Uint,
        Prospero::BufferFormat::k32UInt, source_width, source_width, 1u, 1u, 4u,
        TextureCache::BindingType::Storage);
    Require(name, "finite native fixture geometry", destination.info.data.size == 3u * 4096u &&
        source.info.data.size == 4096u && destination.info.data.End() < source_address,
        "R32 fixture must stay within three4KiB slices and a separate source");
    int64_t direct = -1;
    Require(name, "guest allocation", LibKernel::Memory::KernelAllocateDirectMemory(
        0, LibKernel::Memory::KernelGetDirectMemorySize(), bytes, 0x10000u, 0, &direct) == 0,
        "guest allocation failed; not the intended ownership RED");
    void* mapped = reinterpret_cast<void*>(base);
    Require(name, "guest mapping", LibKernel::Memory::KernelMapDirectMemory(
        &mapped, bytes, 0x3, 0x10, direct, 0x10000u) == 0 && mapped == reinterpret_cast<void*>(base),
        "guest mapping failed; not the intended ownership RED");
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
    const auto destination_id = textures.FindImage(destination);
    Require(name, "actual old destination upload", textures.FindTexture(destination_id, destination) != nullptr,
            "destination must initially be a genuine uploaded cache image");
    const auto source_id = textures.FindImage(source);
    Require(name, "actual source upload", textures.FindTexture(source_id, source) != nullptr,
            "source must be a genuine native cache image");
    auto& source_image = textures.GetImage(source_id);
    scheduler.EndRendering();
    const auto command = scheduler.Current().Handle();
    source_image.Transit(vk::ImageLayout::eTransferDstOptimal,
                        vk::AccessFlagBits2::eTransferWrite, {}, command);
    vk::ClearColorValue clear{};
    clear.uint32 = std::array{copied, 0u, 0u, 0u};
    const vk::ImageSubresourceRange source_range{vk::ImageAspectFlagBits::eColor, 0u, 1u, 0u, 1u};
    command.clearColorImage(source_image.backing.image, vk::ImageLayout::eTransferDstOptimal,
                           &clear, 1u, &source_range);
    textures.MarkGpuWritten(source_id);

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
    std::vector<vk::BufferImageCopy> copies;
    std::vector<uint32_t> expected;
    for (uint32_t layer = 0; layer < layers; ++layer) {
      for (uint32_t level = 0; level < levels; ++level) {
        const uint32_t mip_width = width >> level;
        vk::BufferImageCopy copy{};
        copy.bufferOffset = expected.size() * sizeof(uint32_t);
        copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, level, layer, 1u};
        copy.imageExtent = {mip_width, mip_width, 1u};
        copies.push_back(copy);
        expected.insert(expected.end(), mip_width * mip_width,
                        layer == target_layer && level == target_level ? copied : baseline);
      }
    }
    vk::BufferImageCopy source_copy{};
    source_copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0u, 0u, 1u};
    source_copy.imageExtent = {source_width, source_width, 1u};
    const auto native_source = read_image(source_id, {source_copy}, source_width * source_width * 4u);
    const auto old_destination = read_image(destination_id, copies, expected.size() * sizeof(uint32_t));
    const bool old_correct = std::ranges::all_of(old_destination, [](uint32_t value) { return value == UINT32_MAX; });
    const bool source_correct = std::ranges::all_of(native_source, [](uint32_t value) { return value == copied; });

    // Executed GPU writes transfer ownership of the COMPLETE destination backing.
    // The native image intentionally still contains the earlier white upload.
    auto [owner, owner_offset] = buffers.ObtainBuffer(destination_address, destination.info.data.size, true);
    Require(name, "actual buffer owner", owner != nullptr, "no native destination buffer owner");
    owner->Fill(owner_offset, destination.info.data.size, baseline);
    textures.InvalidateMemoryFromGPU(destination_address, destination.info.data.size,
                                   TextureCache::GpuWriteSource::Fill);
    const bool destination_pending = textures.GetImage(destination_id).IsBufferModified() &&
                                     !textures.GetImage(destination_id).IsCpuDirty();
    FetchGpuBuffer buffer_control(vulkan.RuntimeContext(), destination.info.data.size,
                                  vk::BufferUsageFlagBits::eTransferDst);
    vk::BufferMemoryBarrier2 buffer_ready{};
    buffer_ready.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands;
    buffer_ready.srcAccessMask = vk::AccessFlagBits2::eMemoryWrite;
    buffer_ready.dstStageMask = vk::PipelineStageFlagBits2::eCopy;
    buffer_ready.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
    buffer_ready.srcQueueFamilyIndex = buffer_ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    buffer_ready.buffer = owner->Handle();
    buffer_ready.offset = owner_offset;
    buffer_ready.size = destination.info.data.size;
    vk::DependencyInfo dependency{};
    dependency.bufferMemoryBarrierCount = 1u;
    dependency.pBufferMemoryBarriers = &buffer_ready;
    const auto recording = scheduler.Current().Handle();
    recording.pipelineBarrier2(dependency);
    const vk::BufferCopy buffer_copy{owner_offset, 0u, destination.info.data.size};
    recording.copyBuffer(owner->Handle(), buffer_control.buffer, 1u, &buffer_copy);
    BcAliasHostReadBarrier(scheduler, buffer_control.buffer, destination.info.data.size);
    scheduler.Finish();
    buffer_control.Invalidate();
    const auto* buffer_words = static_cast<const uint32_t*>(buffer_control.data);
    const bool buffer_correct = std::all_of(buffer_words,
        buffer_words + destination.info.data.size / sizeof(uint32_t),
        [](uint32_t value) { return value == baseline; });

    // Exercise the production cache entry, not Image::CopyMip alone. Do not
    // FindTexture/RefreshImage the destination here: that would conceal the bug.
    TextureCacheTestAccess::CopyImageMip(textures, destination_id, source_id, target_level, target_layer);
    const auto actual = read_image(destination_id, copies, expected.size() * sizeof(uint32_t));
    std::vector<uint32_t> cpu(destination.info.data.size / sizeof(uint32_t));
    const bool cpu_white = LibKernel::Memory::TryReadBacking(destination_address, cpu.data(), destination.info.data.size) &&
        std::ranges::all_of(cpu, [](uint32_t value) { return value == UINT32_MAX; });
    scheduler.Finish();
    context.UnmapMemory(base, bytes);
    LibKernel::Memory::InstallGpuResources(nullptr);
    Require(name, "owned guest cleanup", LibKernel::Memory::KernelMunmap(base, bytes) == 0 &&
        LibKernel::Memory::KernelReleaseDirectMemory(direct, bytes) == 0, "guest cleanup failed");

    uint64_t mismatches = 0u;
    for (size_t index = 0; index < actual.size(); ++index) {
      mismatches += actual[index] != expected[index];
    }
    std::printf("[gpu] %s level=%u layer=%u oldWhite=%u source=%u buffer=%u pending=%u cpuWhite=%u "
                "words=%zu mismatches=%llu\n", name, target_level, target_layer, old_correct,
        source_correct, buffer_correct, destination_pending, cpu_white, actual.size(),
        static_cast<unsigned long long>(mismatches));
    Require(name, "independent producer controls", old_correct && source_correct && buffer_correct &&
        destination_pending && cpu_white, "setup did not establish actual pending GPU buffer ownership");
    Require(name, "partial native copy preserves destination GPU bytes", actual == expected,
            "copy preserved stale image data outside the copied mip/layer instead of current GPU buffer bytes");
  }
}

} // namespace
} // namespace Libs::Graphics
