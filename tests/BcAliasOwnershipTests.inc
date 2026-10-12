// Actual TextureCache ownership regression for integer images over BC storage.
// Comparison source: official dad900c61d6c7ff8aefc9e666ca7c3de4bbf4176.
// Include after EmbeddedFetchGpuTests.inc; this submits finite transfers only.
#include "graphics/host_gpu/renderer/image/textureCommon.h"

namespace Libs::Graphics {
namespace {

struct BcAliasObservation {
  std::string name;
  std::vector<uint32_t> actual, expected;
  bool producers_correct = true, padding_correct = true, cpu_still_white = true;
  uint32_t first_tail = 0, producer_count = 0;
  uint64_t slice_stride = 0;
};

TextureCache::ImageDesc BcAliasDesc(const char* name, uint64_t address,
    vk::Format native_format, Prospero::BufferFormat guest_format,
    uint32_t width, uint32_t height, uint32_t levels, uint32_t layers,
    uint32_t block_bytes, TextureCache::BindingType binding) {
  constexpr auto tile = Prospero::TileMode::kStandard4KB;
  TileSizeAlign total{};
  TileGetTextureTotalSize(guest_format, width, height, layers, levels, tile, false, total);
  const auto layout = TextureCalcUploadLayout(guest_format, width, height, levels,
      layers, tile, total.size, false, name);
  TextureCache::ImageDesc desc{};
  desc.type = binding;
  desc.info.data = {address, total.size};
  desc.info.pixel_format = native_format;
  desc.info.guest_format = guest_format;
  desc.info.type = Prospero::ImageType::kColor2D;
  desc.info.extent = {width, height, 1u};
  desc.info.resources = {levels, layers};
  desc.info.pitch = layout.pitch;
  desc.info.bytes_per_block = block_bytes;
  desc.info.samples = 1u;
  desc.info.tile_mode = tile;
  // This checkout's normalized ImageInfo stores tiled source footprints,
  // including the shared tail, rather than the detiled upload offsets.
  for (uint32_t level = 0; level < levels; ++level) {
    const auto& mip = layout.surface.mips[level];
    desc.info.mip_layout[level] = {mip.offset, mip.size * layers,
                                  mip.padded_width, mip.padded_height};
  }
  desc.view_info.format = native_format;
  desc.view_info.type = layers == 1u ? vk::ImageViewType::e2D : vk::ImageViewType::e2DArray;
  desc.view_info.aspect = vk::ImageAspectFlagBits::eColor;
  desc.view_info.usage = binding == TextureCache::BindingType::Storage
      ? vk::ImageUsageFlagBits::eStorage : vk::ImageUsageFlagBits::eSampled;
  desc.view_info.level_count = levels;
  desc.view_info.layer_count = layers;
  return desc;
}

void BcAliasHostReadBarrier(CommandScheduler& scheduler, vk::Buffer buffer, uint64_t bytes) {
  vk::BufferMemoryBarrier2 barrier{};
  barrier.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands;
  barrier.srcAccessMask = vk::AccessFlagBits2::eMemoryWrite;
  barrier.dstStageMask = vk::PipelineStageFlagBits2::eHost;
  barrier.dstAccessMask = vk::AccessFlagBits2::eHostRead;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.buffer = buffer;
  barrier.size = bytes;
  vk::DependencyInfo dependency{};
  dependency.bufferMemoryBarrierCount = 1u;
  dependency.pBufferMemoryBarriers = &barrier;
  scheduler.Current().Handle().pipelineBarrier2(dependency);
}

BcAliasObservation RunBcAliasOwnershipCase(VulkanHarness& vulkan, uint32_t format_index,
                                         bool second_replacement) {
  constexpr std::array formats{vk::Format::eBc4UnormBlock, vk::Format::eBc5UnormBlock,
                               vk::Format::eBc7UnormBlock};
  constexpr std::array guest_formats{Prospero::BufferFormat::kBc4UNorm,
      Prospero::BufferFormat::kBc5UNorm, Prospero::BufferFormat::kBc7UNorm};
  constexpr uint32_t width = 128u, levels = 8u, layers = 3u;
  constexpr uint64_t base = 0x00000002c0000000ull, allocation_bytes = 0x40000u;
  constexpr uint64_t address = base + 0x10000u, guard_bytes = 256u;
  constexpr uint32_t padding_word = 0x5a5a5a5au;
  const uint32_t block_bytes = format_index == 0u ? 8u : 16u;
  BcAliasObservation observation;
  observation.name = format_index == 0u ? "Bc4RawAliasOwnership" :
      format_index == 1u ? "Bc5RawAliasOwnership" : "Bc7RawAliasOwnership";
  const char* name = observation.name.c_str();
  auto array = BcAliasDesc(name, address, formats[format_index], guest_formats[format_index],
      width, width, levels, layers, block_bytes, TextureCache::BindingType::Texture);
  const auto layout = TextureCalcUploadLayout(array.info.guest_format, width, width,
      levels, layers, array.info.tile_mode, array.info.data.size, false, name);
  observation.first_tail = layout.surface.first_tail_level;
  observation.slice_stride = array.info.data.size / layers;
  Require(name, "bounded packed-tail geometry", array.info.data.size != 0u &&
      address + array.info.data.size + guard_bytes <= base + allocation_bytes &&
      observation.first_tail > 0u && observation.first_tail + 1u < levels &&
      observation.slice_stride == layout.surface.block_slice_size &&
      layout.surface.texture.block.block_size == 4096u,
      "fixture must have multiple mips in a real4KiB tail within256KiB");

  int64_t direct = -1;
  Require(name, "tracked guest allocation", LibKernel::Memory::KernelAllocateDirectMemory(
      0, LibKernel::Memory::KernelGetDirectMemorySize(), allocation_bytes, 0x10000u,
      0, &direct) == 0, "synthetic direct allocation failed; not a semantic RED");
  void* mapped = reinterpret_cast<void*>(base);
  Require(name, "tracked guest mapping", LibKernel::Memory::KernelMapDirectMemory(
      &mapped, allocation_bytes, 0x3, 0x10, direct, 0x10000u) == 0 &&
      mapped == reinterpret_cast<void*>(base), "fixed guest map failed; not a semantic RED");
  std::memset(mapped, 0x5a, allocation_bytes);
  std::memset(reinterpret_cast<void*>(address), 0xff, array.info.data.size);

  auto& context = vulkan.RuntimeRenderer();
  auto& scheduler = context.GetCommandScheduler();
  auto& cache = context.GetTextureCache();
  auto& buffers = context.GetBufferCache();
  HW::Context registers{};
  HW::UserConfig user_config{};
  HW::Shader shaders{};
  scheduler.Begin(registers, user_config, shaders);
  LibKernel::Memory::InstallGpuResources(&context);
  context.MapMemory(base, allocation_bytes);
  // A genuinely uploaded native owner provides independent untouched padding.
  // Subsequent padding probes use FindBuffer/GetBuffer and never refresh it.
  (void)buffers.ObtainBuffer(base, allocation_bytes, false);

  const auto value_for = [&](uint32_t layer, uint64_t offset) {
    return 0x7c000000u | (format_index << 16u) | (layer << 8u) |
           static_cast<uint32_t>(offset / 4096u);
  };
  const auto read_image = [&](ImageId id, const std::vector<vk::BufferImageCopy>& copies,
                              uint64_t bytes) {
    FetchGpuBuffer download(vulkan.RuntimeContext(), bytes, vk::BufferUsageFlagBits::eTransferDst);
    cache.GetImage(id).Download(copies, download.buffer, 0u, bytes, bytes);
    BcAliasHostReadBarrier(scheduler, download.buffer, bytes);
    scheduler.Finish();
    download.Invalidate();
    std::vector<uint32_t> words(bytes / sizeof(uint32_t));
    std::memcpy(words.data(), download.data, bytes);
    return words;
  };
  const auto write_raw = [&](uint32_t layer, uint32_t level, uint32_t value) {
    const auto& mip = array.info.mip_layout[level];
    auto raw = BcAliasDesc(name, address + layer * observation.slice_stride + mip.offset,
        block_bytes == 8u ? vk::Format::eR32G32Uint : vk::Format::eR32G32B32A32Uint,
        block_bytes == 8u ? Prospero::BufferFormat::k32_32UInt : Prospero::BufferFormat::k32_32_32_32UInt,
        mip.pitch, mip.height, 1u, 1u, block_bytes, TextureCache::BindingType::Storage);
    Require(name, "raw footprint matches BC source", raw.info.data.size == mip.size / layers,
            "integer producer must cover exactly one BC storage footprint");
    const auto producer = cache.FindImage(raw);
    const auto view = cache.FindTexture(producer, raw);
    Require(name, "actual native integer producer", view != nullptr &&
        cache.PeekImage(producer) != nullptr && cache.PeekImage(producer)->registered &&
        cache.GetImage(producer).backing.format == raw.info.pixel_format,
        "integer producer was not a real native cache image; not a semantic RED");
    scheduler.EndRendering();
    const auto command = scheduler.Current().Handle();
    auto& image = cache.GetImage(producer);
    image.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite,
                  {}, command);
    vk::ClearColorValue clear{};
    std::fill_n(clear.uint32.data(), 4u, value);
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0u, 1u, 0u, 1u};
    command.clearColorImage(image.backing.image, vk::ImageLayout::eTransferDstOptimal,
                           &clear, 1u, &range);
    cache.MarkGpuWritten(producer);
    // GPU readback of both corners proves an executed write, rather than dirty
    // flags alone. Integer image extents are at most32x32; no guest loops exist.
    std::vector<vk::BufferImageCopy> probes(2u);
    for (uint32_t corner = 0; corner < 2u; ++corner) {
      probes[corner].bufferOffset = corner * block_bytes;
      probes[corner].imageSubresource = {vk::ImageAspectFlagBits::eColor, 0u, 0u, 1u};
      probes[corner].imageOffset = corner == 0u ? vk::Offset3D{} :
          vk::Offset3D{static_cast<int32_t>(mip.pitch - 1u), static_cast<int32_t>(mip.height - 1u), 0};
      probes[corner].imageExtent = {1u, 1u, 1u};
    }
    const auto actual = read_image(producer, probes, 2u * block_bytes);
    observation.producers_correct &= std::ranges::all_of(actual,
        [value](uint32_t word) { return word == value; });
    ++observation.producer_count;
  };
  for (const uint32_t layer : {0u, 2u}) {
    uint64_t previous = UINT64_MAX;
    for (uint32_t level = 0; level < levels; ++level) {
      const auto offset = array.info.mip_layout[level].offset;
      if (offset == previous) continue;
      previous = offset;
      write_raw(layer, level, value_for(layer, offset));
    }
  }

  std::vector<vk::BufferImageCopy> copies;
  for (uint32_t layer = 0; layer < layers; ++layer) {
    for (uint32_t level = 0; level < levels; ++level) {
      const auto mip_width = std::max(width >> level, 1u);
      const auto mip_height = std::max(width >> level, 1u);
      const auto blocks = ((mip_width + 3u) / 4u) * ((mip_height + 3u) / 4u);
      vk::BufferImageCopy copy{};
      copy.bufferOffset = observation.expected.size() * sizeof(uint32_t);
      copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, level, layer, 1u};
      copy.imageExtent = {mip_width, mip_height, 1u};
      copies.push_back(copy);
      observation.expected.insert(observation.expected.end(), blocks * block_bytes / sizeof(uint32_t),
          layer == 1u ? UINT32_MAX : value_for(layer, array.info.mip_layout[level].offset));
    }
  }
  Require(name, "bounded complete GPU oracle", copies.size() == 24u &&
      observation.expected.size() * sizeof(uint32_t) < 0x20000u,
      "all24 subresources must fit the predeclared128KiB readback budget");
  const auto acquire_array = [&] {
    const auto consumer = cache.FindImage(array);
    Require(name, "actual BC consumer view", cache.FindTexture(consumer, array) != nullptr &&
        cache.GetImage(consumer).backing.format == array.info.pixel_format &&
        cache.GetImage(consumer).backing.layers == layers &&
        cache.GetImage(consumer).backing.mip_levels == levels,
        "consumer must be an actual BC native array with its full mip chain");
    return consumer;
  };
  observation.actual = read_image(acquire_array(), copies,
                                  observation.expected.size() * sizeof(uint32_t));
  if (second_replacement && observation.actual == observation.expected && observation.producers_correct) {
    constexpr uint32_t replacement = 0x13579bdfu;
    write_raw(2u, observation.first_tail, replacement);
    for (uint32_t level = observation.first_tail; level < levels; ++level) {
      const auto& copy = copies[2u * levels + level];
      const auto begin = copy.bufferOffset / sizeof(uint32_t);
      const auto end = level + 1u < levels ? copies[2u * levels + level + 1u].bufferOffset / sizeof(uint32_t)
                                        : observation.expected.size();
      std::fill(observation.expected.begin() + begin, observation.expected.begin() + end, replacement);
    }
    observation.actual = read_image(acquire_array(), copies,
                                    observation.expected.size() * sizeof(uint32_t));
  }
  // Inspect outer padding and the complete untouched layer from the resident
  // native owner without asking the cache to synchronize another image.
  {
    const uint64_t untouched_bytes = observation.slice_stride;
    const uint64_t bytes = 2u * guard_bytes + untouched_bytes;
    FetchGpuBuffer padding(vulkan.RuntimeContext(), bytes, vk::BufferUsageFlagBits::eTransferDst);
    auto& source = buffers.GetBuffer(buffers.FindBuffer(base, allocation_bytes));
    vk::BufferMemoryBarrier2 ready{};
    ready.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands;
    ready.srcAccessMask = vk::AccessFlagBits2::eMemoryWrite;
    ready.dstStageMask = vk::PipelineStageFlagBits2::eCopy;
    ready.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
    ready.srcQueueFamilyIndex = ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ready.buffer = source.Handle();
    ready.offset = source.Offset(base);
    ready.size = allocation_bytes;
    vk::DependencyInfo dependency{};
    dependency.bufferMemoryBarrierCount = 1u;
    dependency.pBufferMemoryBarriers = &ready;
    const auto command = scheduler.Current().Handle();
    command.pipelineBarrier2(dependency);
    const std::array<vk::BufferCopy, 3> probes{{
        {source.Offset(address - guard_bytes), 0u, guard_bytes},
        {source.Offset(address + array.info.data.size), guard_bytes, guard_bytes},
        {source.Offset(address + observation.slice_stride), 2u * guard_bytes, untouched_bytes}}};
    command.copyBuffer(source.Handle(), padding.buffer, static_cast<uint32_t>(probes.size()), probes.data());
    BcAliasHostReadBarrier(scheduler, padding.buffer, bytes);
    scheduler.Finish();
    padding.Invalidate();
    const auto* actual = static_cast<const uint32_t*>(padding.data);
    observation.padding_correct = std::all_of(actual, actual + 2u * guard_bytes / sizeof(uint32_t),
        [](uint32_t word) { return word == padding_word; }) &&
        std::all_of(actual + 2u * guard_bytes / sizeof(uint32_t), actual + bytes / sizeof(uint32_t),
            [](uint32_t word) { return word == UINT32_MAX; });
  }
  // Read the backing alias, bypassing any fault that could conceal CPU refresh.
  std::vector<uint32_t> backing(array.info.data.size / sizeof(uint32_t));
  observation.cpu_still_white = LibKernel::Memory::TryReadBacking(address, backing.data(), array.info.data.size) &&
      std::ranges::all_of(backing, [](uint32_t word) { return word == UINT32_MAX; });
  scheduler.Finish();
  context.UnmapMemory(base, allocation_bytes);
  LibKernel::Memory::InstallGpuResources(nullptr);
  Require(name, "guest fixture cleanup", LibKernel::Memory::KernelMunmap(base, allocation_bytes) == 0 &&
      LibKernel::Memory::KernelReleaseDirectMemory(direct, allocation_bytes) == 0,
      "task-owned guest mapping was not released");
  return observation;
}

void ConfigureBcAliasOwnershipTests() {
  Require("BcAliasOwnership", "synchronous observation profile", _putenv_s("KYTY_RECORD_THREAD", "0") == 0,
          "configuration failed before the first getter; not a semantic RED");
}

void CheckBcAliasOwnershipGpu(VulkanHarness& vulkan, bool neighbors = false) {
  // BC7 is the initial unchanged RED/GREEN; neighbors add BC4/BC5 and a second
  // packed-tail producer after the consumer already exists.
  const uint32_t first = neighbors ? 0u : 2u;
  for (uint32_t format = first; format < 3u; ++format) {
    const auto observation = RunBcAliasOwnershipCase(vulkan, format, neighbors);
    const char* name = observation.name.c_str();
    uint64_t mismatches = 0u, first_bad = UINT64_MAX;
    for (size_t word = 0; word < observation.expected.size(); ++word) {
      if (observation.actual[word] != observation.expected[word]) {
        if (first_bad == UINT64_MAX) first_bad = word;
        ++mismatches;
      }
    }
    std::printf("[gpu] %s producers=%u rawReadback=%u cpuWhite=%u padding=%u tail=%u stride=%llu "
                "subresources=24 words=%zu mismatches=%llu firstBad=%llu actual=%08x expected=%08x replacement=%u\n",
        name, observation.producer_count, observation.producers_correct, observation.cpu_still_white,
        observation.padding_correct, observation.first_tail,
        static_cast<unsigned long long>(observation.slice_stride), observation.expected.size(),
        static_cast<unsigned long long>(mismatches), static_cast<unsigned long long>(first_bad),
        first_bad == UINT64_MAX ? 0u : observation.actual[first_bad],
        first_bad == UINT64_MAX ? 0u : observation.expected[first_bad], neighbors);
    Require(name, "executed native producer positive control", observation.producers_correct,
            "integer image GPU writes did not read back correctly; not the intended alias RED");
    Require(name, "CPU backing stays stale", observation.cpu_still_white,
            "CPU publication would conceal the GPU-only alias contract");
    Require(name, "untouched native layer and padding", observation.padding_correct,
            "alias publication changed unrelated native layer/padding bytes");
    Require(name, "complete BC array GPU readback", observation.actual == observation.expected,
            "BC consumer lost native integer producer bytes in mip/tail storage");
    std::printf("[gpu] %-32s ok (complete native BC blocks; finite transfers)\n", name);
  }
}

} // namespace
} // namespace Libs::Graphics
