// Actual native image/cache/CommitBindings neighbors. Original age oracle is unchanged.
namespace Libs::Graphics {
struct RenderExecutorTestAccess {
  static void BindTarget(RenderExecutor &executor, ImageId id) { executor.BindRenderTarget(id); }
  static void ResetBindings(RenderExecutor &executor) { executor.ResetBindings(); }
};
struct TextureCacheTestAccess {
  static void FreeImage(TextureCache &cache, ImageId id) {
    std::scoped_lock lock(cache.m_lock);
    cache.FreeImage(id);
  }
};
namespace {

void CheckBindlessImageAccessNeighbors(VulkanHarness &vulkan) {
  constexpr const char *name = "BindlessImageAccessNeighbors";
  constexpr uint64_t base = 0x0000000200400000ull, bytes = 0x10000u, alignment = 0x10000u;
  int64_t direct_offset = -1;
  Require(name, "tracked guest allocation", LibKernel::Memory::KernelAllocateDirectMemory(
      0, LibKernel::Memory::KernelGetDirectMemorySize(), bytes, alignment, 0, &direct_offset) == 0,
      "actual direct-memory allocation failed");
  void *mapped = reinterpret_cast<void *>(base);
  Require(name, "tracked guest mapping", LibKernel::Memory::KernelMapDirectMemory(
      &mapped, bytes, 0x3, 0x10, direct_offset, alignment) == 0 &&
      mapped == reinterpret_cast<void *>(base), "actual direct-memory map failed");
  constexpr uint32_t texel = 0xff112233u;
  for (uint32_t index = 0; index < 4u; ++index) {
    const auto address = base + uint64_t{index} * 0x1000u;
    std::memcpy(reinterpret_cast<void *>(address), &texel, sizeof(texel));
    uint32_t backed = 0;
    Require(name, "actual backing", LibKernel::Memory::TryReadBacking(address, &backed, sizeof(backed)) &&
                backed == texel, "mapped image source was not genuine readable guest backing");
  }
  auto &context = vulkan.RuntimeRenderer();
  auto &scheduler = context.GetCommandScheduler();
  auto &cache = context.GetTextureCache();
  auto &table = context.GetBindlessTable();
  auto &executor = context.GetRenderExecutor();
  HW::Context registers{};
  HW::UserConfig user_config{};
  HW::Shader shaders{};
  scheduler.Begin(registers, user_config, shaders);
  LibKernel::Memory::InstallGpuResources(&context);
  context.MapMemory(base, bytes);
  const auto descriptor = [&](uint64_t address) {
    TextureCache::ImageDesc desc{};
    desc.type = TextureCache::BindingType::Texture;
    desc.info.data = {address, address != 0 ? sizeof(texel) : 0u};
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
  std::array<ImageId, 4> ids{};
  std::array<vk::ImageView, 4> views{};
  for (uint32_t index = 0; index < ids.size(); ++index) {
    auto desc = descriptor(base + uint64_t{index} * 0x1000u);
    ids[index] = cache.FindImage(desc);
    views[index] = cache.FindTexture(ids[index], desc);
    const auto *image = cache.PeekImage(ids[index]);
    Require(name, "actual native cache image", image != nullptr && image->registered &&
                views[index] != nullptr, "real registered cache image/view acquisition failed");
  }
  const auto active = ids[0], target = ids[1], victim = ids[2];
  auto empty_desc = descriptor(0u);
  const auto empty = cache.FindImage(empty_desc);
  Require(name, "actual empty-data image", cache.PeekImage(empty) != nullptr &&
              cache.PeekImage(empty)->info.data.Empty(), "cache did not create its real null image");
  RenderExecutorTestAccess::BindTarget(executor, target);
  const auto slot = table.SharedSlot(BindlessTable::Images2D, active, views[0],
                                   vk::ImageLayout::eShaderReadOnlyOptimal);
  Require(name, "real shared native slot", slot != 0u, "shared native slot acquisition failed");
  const auto resource = NeighborImageResource();
  auto *heap = table.FindOrCreateHeap(base + 0x8000u, 0u, 32u,
                                     BindlessTable::Images2D, 1u, resource);
  Require(name, "real heap", heap != nullptr, "actual heap allocation failed");
  heap->valid_keys = 1u;
  heap->slots[0] = slot;
  heap->settled[0] = 1u;
  heap->resolved.push_back(active);
  table.SetTranslation(*heap, 0u, slot);
  table.AddImageReference(active, *heap, 0u);
  cache.GetImage(active).bindless_pinned = true;
  for (uint32_t iteration = 0; iteration < 3u; ++iteration) {
    scheduler.Finish();
    cache.TickFrame();
  }
  TextureCacheTestAccess::FreeImage(cache, victim);
  const auto *unregistered = cache.PeekImage(victim);
  Require(name, "actual unregistered owner", unregistered != nullptr && !unregistered->registered,
          "actual deletion did not expose its deferred unregistered native owner");

  vk::DescriptorSetLayoutCreateInfo empty_layout{};
  std::array<vk::DescriptorSetLayout, 3> layouts{nullptr, nullptr, table.Layout()};
  RequireVulkanSuccess(vulkan.Device().createDescriptorSetLayout(&empty_layout, nullptr, &layouts[0]),
                       "native neighbor descriptor layout0");
  RequireVulkanSuccess(vulkan.Device().createDescriptorSetLayout(&empty_layout, nullptr, &layouts[1]),
                       "native neighbor descriptor layout1");
  PipelineCache::Pipeline pipeline{};
  pipeline.descriptor_set_layout = layouts[0];
  pipeline.pixel_set_layout = layouts[1];
  pipeline.uses_bindless = true;
  vk::PipelineLayoutCreateInfo layout_info{};
  layout_info.setLayoutCount = layouts.size();
  layout_info.pSetLayouts = layouts.data();
  RequireVulkanSuccess(vulkan.Device().createPipelineLayout(&layout_info, nullptr,
                                                           &pipeline.pipeline_layout),
                       "real neighbor pipeline layout");
  struct Observation { uint64_t accessed, lru; vk::ImageLayout layout; };
  const auto observe = [&](ImageId id) {
    const auto *image = cache.PeekImage(id);
    Require(name, "raw observation", image != nullptr, "expected native cache owner is missing");
    return Observation{image->tick_accessed_last, image->lru_tick, image->backing.state.layout};
  };
  const auto commit = [&](ShaderType shader_stage, std::vector<ImageId> bindlist) {
    ShaderRecompiler::IR::CompiledShaderInfo program{};
    program.stage = shader_stage;
    ShaderRecompiler::IR::ResourceSnapshot snapshot{};
    ShaderStageRuntime runtime{&program, &snapshot};
    PreparedBindings prepared;
    prepared.runtime = &runtime;
    prepared.bindless_images = std::move(bindlist);
    prepared.shader_data.resize(program.bindings.ShaderDataDwords());
    std::array<PreparedBindings *, 1> stages{&prepared};
    BindlessTable::PreparationScope operation(table);
    (void)table.FindOrCreateHeap(base + 0x8000u, 0u, 32u,
                                BindlessTable::Images2D, 1u, resource);
    executor.CommitBindings(scheduler.Current(), shader_stage == ShaderType::Compute
        ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics,
        pipeline, stages);
  };
  const auto check_active = [&](uint64_t tick, uint64_t gc_tick) {
    const auto after = observe(active);
    Require(name, "actual eligible access", after.accessed == tick && after.lru == gc_tick &&
                after.layout == vk::ImageLayout::eShaderReadOnlyOptimal,
            "eligible real bindless image lost current access/recency or its sampled layout");
  };
  const auto skipped_target = observe(target), skipped_empty = observe(empty),
             skipped_unregistered = observe(victim);
  const auto tick = scheduler.CurrentTick(), gc_tick = cache.CurrentTargetState().gc_tick;
  commit(ShaderType::Compute, {active, active, target, empty, victim, active});
  check_active(tick, gc_tick);
  const auto target_after = observe(target), empty_after = observe(empty),
             unregistered_after = observe(victim);
  Require(name, "skip eligibility preserved", target_after.accessed == skipped_target.accessed &&
              target_after.lru == skipped_target.lru && target_after.layout == skipped_target.layout &&
              empty_after.accessed == skipped_empty.accessed && empty_after.lru == skipped_empty.lru &&
              empty_after.layout == skipped_empty.layout &&
              unregistered_after.accessed == skipped_unregistered.accessed &&
              unregistered_after.lru == skipped_unregistered.lru &&
              unregistered_after.layout == skipped_unregistered.layout,
          "target, empty or actual unregistered owner was touched/transited despite the existing skip contract");
  // Repeating at the same tick must be safe and idempotent even with duplicate IDs.
  commit(ShaderType::Compute, {active, active});
  check_active(tick, gc_tick);
  scheduler.Finish();
  cache.TickFrame();
  Require(name, "already sampled layout", observe(active).layout == vk::ImageLayout::eShaderReadOnlyOptimal,
          "fixture did not retain its real already-read-only native layout");
  const auto next_tick = scheduler.CurrentTick(), next_gc = cache.CurrentTargetState().gc_tick;
  commit(ShaderType::Compute, {active, active, target, empty});
  check_active(next_tick, next_gc);
  // Complete the actual second deferred-delete stage, producing a genuine stale ID.
  scheduler.Finish();
  cache.TickFrame();
  Require(name, "actual stale generational ID", cache.PeekImage(victim) == nullptr,
          "actual deferred native deletion did not finish; cannot claim stale-ID coverage");
  const auto graphics_tick = scheduler.CurrentTick(), graphics_gc = cache.CurrentTargetState().gc_tick;
  commit(ShaderType::Vertex, {victim, active, active, target, empty});
  check_active(graphics_tick, graphics_gc);
  Require(name, "stale skip preserved", cache.PeekImage(victim) == nullptr,
          "stale bindlist ID changed actual cache slot ownership");
  RenderExecutorTestAccess::ResetBindings(executor);
  scheduler.Finish();
  vulkan.Device().destroyPipelineLayout(pipeline.pipeline_layout, nullptr);
  vulkan.Device().destroyDescriptorSetLayout(layouts[1], nullptr);
  vulkan.Device().destroyDescriptorSetLayout(layouts[0], nullptr);
  context.UnmapMemory(base, bytes);
  LibKernel::Memory::InstallGpuResources(nullptr);
  Require(name, "tracked guest unmap", LibKernel::Memory::KernelMunmap(base, bytes) == 0,
          "tracked guest unmap failed");
  Require(name, "direct-memory cleanup", LibKernel::Memory::KernelReleaseDirectMemory(direct_offset, bytes) == 0,
          "actual direct-memory cleanup failed");
  std::printf("[host] %-32s ok (actual compute/graphics commits; duplicates/repeat/skip; no shader dispatch; binding_plan=%s)\n",
              name, std::getenv("KYTY_BINDING_PLAN") != nullptr ? std::getenv("KYTY_BINDING_PLAN") : "default");
}
} // namespace
} // namespace Libs::Graphics
