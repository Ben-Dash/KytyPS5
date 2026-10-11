// Native cache/RenderExecutor regression. No guest or synthetic shader dispatch.
#include "common/virtualMemory.h"
#include "graphics/host_gpu/renderer/render.h"

namespace Libs::Graphics {
namespace {

void CheckBindlessImageAccessAge(VulkanHarness &vulkan) {
  constexpr const char *name = "BindlessCommitRefreshesImageAccessAge";
  constexpr uintptr_t base = 0x0000000200200000ull;
  constexpr uint64_t allocation_size = 0x10000u;
  constexpr uint64_t allocation_alignment = 0x10000u;
  int64_t direct_offset = -1;
  Require(name, "actual guest direct allocation",
          LibKernel::Memory::KernelAllocateDirectMemory(
              0, LibKernel::Memory::KernelGetDirectMemorySize(), allocation_size,
              allocation_alignment, 0, &direct_offset) == 0,
          "real direct-memory allocation failed; not a semantic RED");
  void *mapped = reinterpret_cast<void *>(base);
  Require(name, "actual tracked guest mapping",
          LibKernel::Memory::KernelMapDirectMemory(&mapped, allocation_size, 0x3, 0x10,
              direct_offset, allocation_alignment) == 0 && mapped == reinterpret_cast<void *>(base),
          "fixed tracked direct-memory mapping failed; not a semantic RED");
  const uint32_t texel = 0xff336699u;
  std::memcpy(reinterpret_cast<void *>(base), &texel, sizeof(texel));
  std::memcpy(reinterpret_cast<void *>(base + 0x1000u), &texel, sizeof(texel));
  uint32_t first_backed = 0, second_backed = 0;
  Require(name, "real readable guest backing",
          LibKernel::Memory::TryReadBacking(base, &first_backed, sizeof(first_backed)) &&
              LibKernel::Memory::TryReadBacking(base + 0x1000u, &second_backed, sizeof(second_backed)) &&
              first_backed == texel && second_backed == texel,
          "registered guest backing did not contain both actual written texels; not a semantic RED");
  auto &context = vulkan.RuntimeRenderer();
  auto &scheduler = context.GetCommandScheduler();
  auto &cache = context.GetTextureCache();
  auto &table = context.GetBindlessTable();
  HW::Context registers{};
  HW::UserConfig user_config{};
  HW::Shader shaders{};
  scheduler.Begin(registers, user_config, shaders);
  LibKernel::Memory::InstallGpuResources(&context);
  context.MapMemory(base, allocation_size);

  const auto descriptor = [&](uint64_t address) {
    TextureCache::ImageDesc desc{};
    desc.type = TextureCache::BindingType::Texture;
    desc.info.data = {address, sizeof(texel)};
    desc.info.pixel_format = vk::Format::eR8G8B8A8Unorm;
    desc.info.guest_format = Prospero::BufferFormat::k8_8_8_8UNorm;
    desc.info.type = Prospero::ImageType::kColor2D;
    desc.info.extent = {1u, 1u, 1u};
    desc.info.resources = {1u, 1u};
    desc.info.pitch = 1u;
    desc.info.bytes_per_block = sizeof(texel);
    desc.info.samples = 1u;
    desc.info.tile_mode = Prospero::TileMode::kLinear;
    desc.info.mip_layout[0] = {0u, sizeof(texel), 1u, 1u};
    desc.view_info.format = desc.info.pixel_format;
    desc.view_info.type = vk::ImageViewType::e2D;
    desc.view_info.aspect = vk::ImageAspectFlagBits::eColor;
    desc.view_info.usage = vk::ImageUsageFlagBits::eSampled;
    desc.view_info.level_count = desc.view_info.layer_count = 1u;
    return desc;
  };
  auto bindless_desc = descriptor(base);
  const auto bindless_image = cache.FindImage(bindless_desc);
  const auto bindless_view = cache.FindTexture(bindless_image, bindless_desc);
  auto direct_desc = descriptor(base + 0x1000u);
  uint64_t direct_generation = 0;
  const auto direct_image = cache.FindImage(direct_desc, false, &direct_generation);
  const auto direct_view = cache.FindTexture(direct_image, direct_desc);
  // A cold insertion registers an image but does not promise a reusable unique
  // generation. A repeated exact lookup establishes the real direct-reuse contract.
  const auto repeated_direct = cache.FindImage(direct_desc, false, &direct_generation);
  const auto *initial = cache.PeekImage(bindless_image);
  Require(name, "real cache/image views", initial != nullptr && initial->registered &&
              !initial->info.data.Empty() && bindless_view != nullptr && direct_view != nullptr &&
              repeated_direct == direct_image && direct_generation != 0u,
          "actual registered cache images were not acquired; not a semantic RED");
  const uint64_t first_access = initial->tick_accessed_last;
  const auto slot = table.SharedSlot(BindlessTable::Images2D, bindless_image,
                                   bindless_view, vk::ImageLayout::eShaderReadOnlyOptimal);
  Require(name, "actual shared native slot", slot != 0u,
          "actual native cache view did not receive a shared bindless slot");
  auto resource = NeighborImageResource();
  auto *heap = table.FindOrCreateHeap(base + 0x8000u, 0u, 32u,
                                     BindlessTable::Images2D, 1u, resource);
  Require(name, "actual bindless heap", heap != nullptr, "fixture heap allocation failed");
  heap->valid_keys = 1u;
  heap->slots[0] = slot;
  heap->settled[0] = heap->listed[0] = 1u;
  heap->settled_keys.push_back(0u);
  heap->resolved.push_back(bindless_image);
  table.SetTranslation(*heap, 0u, slot);
  table.AddImageReference(bindless_image, *heap, 0u);
  cache.GetImage(bindless_image).bindless_pinned = true;

  // Age through actual native submission and GC-frame boundaries, not forged
  // image tick fields. The only GPU work is finite image upload/empty submissions.
  for (uint32_t iteration = 0; iteration < 33u; ++iteration) {
    scheduler.Finish();
    cache.TickFrame();
  }
  const auto current_tick = scheduler.CurrentTick();
  const auto state = cache.CurrentTargetState();
  initial = cache.PeekImage(bindless_image);
  Require(name, "real older access history", initial != nullptr && initial->registered &&
              initial->tick_accessed_last == first_access && current_tick > first_access + 32u,
          "fixture did not establish the actual older access history; not a semantic RED");

  // Positive control is the public direct-binding reuse path: the same cache,
  // real image and real tick refresh both access ownership and LRU recency.
  Require(name, "direct binding reuse positive control",
          cache.RefindImage(direct_image, direct_generation, direct_desc, 0u),
          "actual direct-binding reuse could not refind its native cache image");
  const auto *direct = cache.PeekImage(direct_image);
  Require(name, "direct access age positive control", direct != nullptr && direct->registered &&
              direct->tick_accessed_last == current_tick && direct->lru_tick == state.gc_tick,
          "direct-binding reuse did not establish independently expected current access/recency");

  ShaderRecompiler::IR::CompiledShaderInfo program{};
  program.stage = ShaderType::Compute;
  ShaderRecompiler::IR::ResourceSnapshot snapshot{};
  ShaderStageRuntime runtime{&program, &snapshot};
  PreparedBindings prepared;
  prepared.runtime = &runtime;
  prepared.bindless_images = heap->resolved;
  prepared.shader_data.resize(program.bindings.ShaderDataDwords());
  vk::DescriptorSetLayoutCreateInfo empty{};
  std::array<vk::DescriptorSetLayout, 3> layouts{nullptr, nullptr, table.Layout()};
  RequireVulkanSuccess(vulkan.Device().createDescriptorSetLayout(&empty, nullptr, &layouts[0]),
                       "native empty compute descriptor layout");
  RequireVulkanSuccess(vulkan.Device().createDescriptorSetLayout(&empty, nullptr, &layouts[1]),
                       "native empty intermediate descriptor layout");
  PipelineCache::Pipeline pipeline{};
  pipeline.descriptor_set_layout = layouts[0];
  pipeline.pixel_set_layout = layouts[1];
  pipeline.uses_bindless = true;
  vk::PipelineLayoutCreateInfo layout_info{};
  layout_info.setLayoutCount = static_cast<uint32_t>(layouts.size());
  layout_info.pSetLayouts = layouts.data();
  RequireVulkanSuccess(vulkan.Device().createPipelineLayout(&layout_info, nullptr,
                                                           &pipeline.pipeline_layout),
                       "actual bindless-only native pipeline layout");
  std::array<PreparedBindings *, 1> stages{&prepared};
  {
    BindlessTable::PreparationScope operation(table);
    // Lease the actual region as the real preparation path does before commit.
    (void)table.FindOrCreateHeap(base + 0x8000u, 0u, 32u,
                                BindlessTable::Images2D, 1u, resource);
    context.GetRenderExecutor().CommitBindings(scheduler.Current(),
        vk::PipelineBindPoint::eCompute, pipeline, stages);
  }
  const auto *committed = cache.PeekImage(bindless_image);
  std::printf("[host] actual bindless commit access: initial=%llu current=%llu observed=%llu "
              "gc_tick=%llu lru=%llu direct_access=%llu; shader_dispatches=0\n",
              static_cast<unsigned long long>(first_access),
              static_cast<unsigned long long>(current_tick),
              static_cast<unsigned long long>(committed != nullptr ? committed->tick_accessed_last : 0u),
              static_cast<unsigned long long>(state.gc_tick),
              static_cast<unsigned long long>(committed != nullptr ? committed->lru_tick : 0u),
              static_cast<unsigned long long>(direct->tick_accessed_last));
  Require(name, "actual bindless commit access ownership", committed != nullptr && committed->registered &&
              committed->tick_accessed_last == current_tick && committed->lru_tick == state.gc_tick,
          "actual CommitBindings left a currently bound native image with its older access/recency; direct reuse refreshed the same contract");
  scheduler.Finish();
  vulkan.Device().destroyPipelineLayout(pipeline.pipeline_layout, nullptr);
  vulkan.Device().destroyDescriptorSetLayout(layouts[1], nullptr);
  vulkan.Device().destroyDescriptorSetLayout(layouts[0], nullptr);
  context.UnmapMemory(base, allocation_size);
  LibKernel::Memory::InstallGpuResources(nullptr);
  Require(name, "guest fixture unmap", LibKernel::Memory::KernelMunmap(base, allocation_size) == 0,
          "native tracked guest mapping cleanup failed");
  Require(name, "guest direct allocation cleanup",
          LibKernel::Memory::KernelReleaseDirectMemory(direct_offset, allocation_size) == 0,
          "native direct-memory allocation cleanup failed");
  std::printf("[host] %-32s ok (actual cache/CommitBindings; no shader dispatch)\n", name);
}
} // namespace
} // namespace Libs::Graphics
