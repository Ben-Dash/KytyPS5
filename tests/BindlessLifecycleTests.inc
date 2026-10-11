// Included after the native harness and before main. This checks the actual
// BindlessTable allocator's host state; it submits no shader or game workload.
namespace Libs::Graphics {
namespace {

void CheckBindlessLifecycle(VulkanHarness &vulkan) {
  constexpr const char *name = "BindlessStableMappingUnderChurn";
  auto &context = vulkan.RuntimeRenderer();
  auto &graphics = vulkan.RuntimeContext();
  auto &table = context.GetBindlessTable();
  Require(name, "production bindless setup", table.Enabled(),
          "actual BindlessTable was not initialized; this is not a passing regression");

  // Slot7 is genuinely allocated and backed by an ordinary native sampled image.
  auto image = vulkan.CreateImage2D(
      name, 1u, 1u, vk::Format::eR32G32B32A32Sfloat,
      vk::ImageUsageFlagBits::eSampled,
      std::vector<u32>(4u, std::bit_cast<u32>(1.0f)), 4u,
      vk::ImageLayout::eShaderReadOnlyOptimal);
  uint32_t slot = 0;
  for (uint32_t expected = BindlessTable::PlaceholderColors; expected <= 7u; ++expected) {
    slot = table.AllocateSlot(BindlessTable::Images2D);
    Require(name, "sampled slot allocation", slot == expected,
            "could not allocate the independently expected native slot7");
  }
  table.WriteSlot(BindlessTable::Images2D, slot, image.view, image.layout);
  RequireVulkanSuccess(vulkan.Device().waitIdle(), "idle native fixture setup");

  constexpr uint64_t stable_base = 0x100000u;
  constexpr uint32_t stable_keys = 16u;
  ShaderRecompiler::IR::ImageResource resource{};
  resource.resource_class = ShaderRecompiler::IR::ImageResourceClass::Sampled;
  resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
  resource.read = true;
  auto *stable = table.FindOrCreateHeap(stable_base, 0u, 32u,
                                       BindlessTable::Images2D, stable_keys, resource);
  Require(name, "stable allocation", stable != nullptr,
          "the first small stable heap could not be allocated");
  stable->valid_keys = stable_keys;
  stable->slots[0] = slot;
  stable->settled[0] = 1u;
  stable->listed[0] = 1u;
  stable->settled_keys.push_back(0u);
  stable->record_hash[0] = 0x12345678u;
  table.SetTranslation(*stable, 0u, slot);
  const auto stable_region = stable->region;
  PreparedBindings prepared;
  prepared.bindless_patches.push_back({0u, stable_region, stable_keys});
  uint64_t fresh_capacity_requested = stable->entries;

  // Every iteration uses A and introduces a different temporary table. No GPU
  // command references the temporary: setup is idle and the test never dispatches.
  // checked_frame is the existing host activity metadata. At the next frame,
  // the previous temporary is completed/inactive while A remains in active use.
  constexpr uint32_t temporary_keys = 8192u;
  constexpr uint32_t iterations = 64u;
  for (uint32_t iteration = 0; iteration < iterations; ++iteration) {
    const uint64_t frame = uint64_t{iteration} + 1u;
    graphics.presented_frames.store(frame, std::memory_order_relaxed);
    auto current = std::ranges::find_if(table.Heaps(), [](const auto &heap) {
      return heap.base == stable_base;
    });
    Require(name, "stable activity", current != table.Heaps().end(),
            "stable heap disappeared before the next active frame");
    current->checked_frame = frame;
    current->valid_keys = stable_keys;

    const uint64_t temporary_base = 0x200000u + uint64_t{iteration} * 0x100000u;
    auto *temporary = table.FindOrCreateHeap(
        temporary_base, 0u, 32u, BindlessTable::Images2D, temporary_keys, resource);
    Require(name, "required temporary allocation", temporary != nullptr,
            "allocator rejected required temporary work instead of reclaiming completed inactive regions");
    temporary->checked_frame = frame;
    temporary->valid_keys = temporary_keys;
    fresh_capacity_requested += temporary->entries;

    // Look up A again: a saved pointer is deliberately never dereferenced after
    // allocation, because current ResetHeaps destroys every previous Heap.
    current = std::ranges::find_if(table.Heaps(), [](const auto &heap) {
      return heap.base == stable_base;
    });
    if (current == table.Heaps().end()) {
      std::printf("[host] lifecycle RED: temporary=%u fresh_capacity=%llu "
                  "stable_region=%u original_slot=%u; active heap removed by unrelated churn\n",
                  iteration + 1u,
                  static_cast<unsigned long long>(fresh_capacity_requested),
                  stable_region, slot);
    }
    Require(name, "stable mapping survives unrelated churn",
            current != table.Heaps().end(),
            "allocator exhaustion discarded the active stable heap and its resolved mapping");
    Require(name, "settled mapping survives unrelated churn",
            current->region == prepared.bindless_patches[0][1] &&
                current->valid_keys == prepared.bindless_patches[0][2] &&
                current->settled[0] == 1u && current->slots[0] == 7u &&
                current->record_hash[0] == 0x12345678u &&
                current->settled_keys == std::vector<uint32_t>{0u},
            "unrelated dynamic tables changed the active stable region or resolved mapping");
  }
  Require(name, "actual allocation pressure",
          fresh_capacity_requested > ShaderRecompiler::IR::WatchdogReportBase,
          "fixture did not exercise more fresh region capacity than the actual fixed arena");
  RequireVulkanSuccess(vulkan.Device().waitIdle(), "idle native fixture cleanup");
  vulkan.DestroyImage(&image);
  std::printf("[host] %-32s ok (host-state; no shader dispatch)\n", name);
}

} // namespace
} // namespace Libs::Graphics
