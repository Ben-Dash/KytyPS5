// Separate neighbors; the original BindlessLifecycleTests.inc oracle is unchanged.
#if !defined(KYTY_BINDLESS_LIFECYCLE_TEST_ACCESS)
#error "The standalone lifecycle target requires its narrowly scoped native buffer test-access macro"
#endif

namespace Libs::Graphics {
struct BindlessLifecycleTestAccess {
  static uint32_t Translation(BindlessTable &table, uint32_t region, uint32_t key) {
    uint32_t word = 0;
    const auto offset = uint64_t{region + key} * sizeof(uint32_t);
    std::memcpy(&word, table.m_translation->Mapped().data() + offset, sizeof(word));
    return word;
  }
  static void WriteFeedback(BindlessTable &table, uint32_t region, uint32_t key, uint32_t word) {
    const auto offset = uint64_t{region + key} * sizeof(uint32_t);
    std::memcpy(table.m_feedback->Mapped().data() + offset, &word, sizeof(word));
    table.m_feedback->Flush(offset, sizeof(word));
  }
  static uint32_t SnapshotWord(BindlessTable &table, uint32_t region, uint32_t key) {
    const auto offset = uint64_t{region + key} * sizeof(uint32_t);
    table.m_feedback_snapshot->Invalidate(offset, sizeof(uint32_t));
    uint32_t word = 0;
    std::memcpy(&word, table.m_feedback_snapshot->Mapped().data() + offset, sizeof(word));
    return word;
  }
  static uint32_t TailCapacity(const BindlessTable &table) {
    return ShaderRecompiler::IR::WatchdogReportBase - table.m_next_region;
  }
  static uint64_t SnapshotCount(const BindlessTable &table) { return table.m_snapshot_count; }
  struct Ownership { bool exists = false; uint64_t preparation = 0, gpu_tick = 0; };
  static Ownership RegionOwnership(const BindlessTable &table, uint64_t generation) {
    for (const auto &region : table.m_regions)
      if (region.generation == generation)
        return {true, region.preparation, region.last_gpu_tick};
    return {};
  }
};

namespace {
ShaderRecompiler::IR::ImageResource NeighborImageResource() {
  ShaderRecompiler::IR::ImageResource resource{};
  resource.resource_class = ShaderRecompiler::IR::ImageResourceClass::Sampled;
  resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
  resource.read = true;
  return resource;
}

void CheckBindlessPreparationNeighbors(VulkanHarness &vulkan) {
  constexpr const char *name = "BindlessPreparationGrowthAbort";
  auto &graphics = vulkan.RuntimeContext();
  auto &table = vulkan.RuntimeRenderer().GetBindlessTable();
  const auto resource = NeighborImageResource();
  auto image = vulkan.CreateImage2D(name, 1u, 1u, vk::Format::eR32G32B32A32Sfloat,
      vk::ImageUsageFlagBits::eSampled, std::vector<u32>(4u, std::bit_cast<u32>(2.0f)),
      4u, vk::ImageLayout::eShaderReadOnlyOptimal);
  uint32_t slot = 0;
  for (uint32_t expected = BindlessTable::PlaceholderColors; expected <= 7u; ++expected) {
    slot = table.AllocateSlot(BindlessTable::Images2D);
    Require(name, "real image slot", slot == expected, "native slot7 allocation failed");
  }
  table.WriteSlot(BindlessTable::Images2D, slot, image.view, image.layout);
  RequireVulkanSuccess(vulkan.Device().waitIdle(), "idle preparation fixture");
  constexpr uint64_t stable_base = 0x100000u;
  auto *stable = table.FindOrCreateHeap(stable_base, 0u, 32u,
                                       BindlessTable::Images2D, 16u, resource);
  Require(name, "stable fixture", stable != nullptr, "stable allocation failed");
  stable->checked_frame = 100u;
  stable->valid_keys = 16u;
  for (const auto key : {0u, 15u}) {
    stable->slots[key] = 7u;
    stable->settled[key] = stable->listed[key] = 1u;
    stable->record_hash[key] = 0x1000u + key;
    stable->settled_keys.push_back(key);
    table.SetTranslation(*stable, key, 7u);
  }
  // Fill existing arena with genuinely GPU-unused, unleased completed candidates.
  uint32_t fillers = 0;
  while (BindlessLifecycleTestAccess::TailCapacity(table) >= 32768u && fillers < 64u) {
    auto *temporary = table.FindOrCreateHeap(0x200000u + uint64_t{fillers} * 0x100000u,
        0u, 32u, BindlessTable::Images2D, 8192u, resource);
    Require(name, "pressure fixture", temporary != nullptr, "temporary allocation failed");
    temporary->checked_frame = 1u;
    ++fillers;
  }
  Require(name, "bounded pressure fixture", fillers != 0u && fillers < 64u,
          "could not establish pressure in the actual fixed arena");
  graphics.presented_frames.store(100u, std::memory_order_relaxed);
  BindlessTable::RegionUse original{};
  uint64_t final_generation = 0;
  {
    BindlessTable::PreparationScope operation(table);
    stable = table.FindOrCreateHeap(stable_base, 0u, 32u,
                                   BindlessTable::Images2D, 16u, resource);
    original = {stable->region, 16u, stable->region_generation};
    PreparedBindings vertex;
    vertex.bindless_patches.push_back({0u, original.region, original.entries});
    Require(name, "vertex preparation lease", table.IsPreparationRegion(original),
            "actual preparation scope did not lease the vertex region");
    auto *pixel = table.FindOrCreateHeap(0x80000000u, 0u, 32u,
        BindlessTable::Images2D, 32768u, resource);
    Require(name, "later pixel allocation", pixel != nullptr,
            "required pixel stage work was rejected under pressure");
    const BindlessTable::RegionUse pixel_use{pixel->region, 32768u, pixel->region_generation};
    Require(name, "independent stage regions", table.IsPreparationRegion(pixel_use) &&
                pixel_use.region != original.region && table.IsPreparationRegion(original) &&
                stable->region == vertex.bindless_patches[0][1] &&
                BindlessLifecycleTestAccess::Translation(table, original.region, 0u) == 7u &&
                BindlessLifecycleTestAccess::Translation(table, original.region, 15u) == 7u,
            "later-stage pressure invalidated a leased vertex patch or its actual translation words");

    // Grow twice while the original exact incarnation stays leased by this operation.
    for (uint32_t growth = 0; growth < 2u; ++growth) {
      const uint32_t required = stable->entries + 1u;
      stable = table.FindOrCreateHeap(stable_base, 0u, 32u,
                                     BindlessTable::Images2D, required, resource);
      const BindlessTable::RegionUse grown{stable->region, required, stable->region_generation};
      Require(name, "growth incarnation lease", grown.generation != original.generation &&
                  grown.region != original.region && table.IsPreparationRegion(grown) &&
                  table.IsPreparationRegion(original) && stable->slots[0] == 7u &&
                  stable->slots[15] == 7u && stable->settled[0] && stable->settled[15] &&
                  stable->record_hash[15] == 0x100fu &&
                  BindlessLifecycleTestAccess::Translation(table, grown.region, 0u) == 7u &&
                  BindlessLifecycleTestAccess::Translation(table, original.region, 0u) == 7u &&
                  BindlessLifecycleTestAccess::Translation(table, original.region, 15u) == 7u,
              "growth destroyed the older leased incarnation or lost resolved mapping metadata");
      final_generation = grown.generation;
    }
    // No MarkPreparationUsed: this operation aborts without recording a GPU consumer.
  }
  const auto aborted_old = BindlessLifecycleTestAccess::RegionOwnership(table, original.generation);
  const auto aborted_new = BindlessLifecycleTestAccess::RegionOwnership(table, final_generation);
  Require(name, "abort lease release", !table.IsPreparationRegion(original) &&
              aborted_old.exists && aborted_new.exists && aborted_old.preparation == 0u &&
              aborted_new.preparation == 0u && aborted_old.gpu_tick == 0u && aborted_new.gpu_tick == 0u,
          "aborted preparation retained a pin or invented a GPU-use tick");
  stable->checked_frame = 0u;
  // Pressure after abort may now reclaim old incarnations and the inactive owner.
  for (uint32_t iteration = 0; iteration < 64u &&
       BindlessLifecycleTestAccess::RegionOwnership(table, original.generation).exists; ++iteration) {
    auto *temporary = table.FindOrCreateHeap(0x90000000u + uint64_t{iteration} * 0x100000u,
        0u, 32u, BindlessTable::Images2D, 32768u, resource);
    Require(name, "post-abort required work", temporary != nullptr,
            "released preparation prevented bounded allocator progress");
    temporary->checked_frame = 101u + iteration;
  }
  Require(name, "aborted incarnation reclamation",
          !BindlessLifecycleTestAccess::RegionOwnership(table, original.generation).exists,
          "retired region could not be reclaimed after every preparation lease was released");
  vulkan.DestroyImage(&image);
  std::printf("[host] %-32s ok (real mapped words; preparation/growth/abort; no shader dispatch)\n", name);
}

void CheckBindlessFeedbackNeighbors(VulkanHarness &vulkan) {
  constexpr const char *name = "BindlessFeedbackSnapshotOwnership";
  auto &context = vulkan.RuntimeRenderer();
  auto &table = context.GetBindlessTable();
  auto &scheduler = context.GetCommandScheduler();
  const auto resource = NeighborImageResource();
  HW::Context registers{};
  HW::UserConfig user_config{};
  HW::Shader shaders{};
  scheduler.Begin(registers, user_config, shaders);
  auto *old_owner = table.FindOrCreateHeap(0x100000u, 0u, 32u,
      BindlessTable::Images2D, 16u, resource);
  Require(name, "old owner", old_owner != nullptr, "initial heap allocation failed");
  old_owner->valid_keys = 16u;
  old_owner->checked_frame = 0u;
  const auto old_region = old_owner->region;
  const auto old_generation = old_owner->region_generation;
  BindlessLifecycleTestAccess::WriteFeedback(table, old_region, 3u, 1u);
  table.RecordFeedbackSnapshot(scheduler);
  scheduler.Finish();
  Require(name, "actual completed old snapshot", table.SnapshotReady(scheduler) &&
              BindlessLifecycleTestAccess::SnapshotWord(table, old_region, 3u) == 1u,
          "real feedback copy did not preserve the controlled old-owner flag");
  std::vector<uint32_t> requests;
  table.TakeRequests(*old_owner, requests);
  Require(name, "same-owner positive control", requests == std::vector<uint32_t>{3u},
          "actual same-owner snapshot did not deliver exactly its requested key");
  const auto old_snapshot = BindlessLifecycleTestAccess::SnapshotCount(table);
  BindlessTable::Heap *new_owner = nullptr;
  for (uint32_t iteration = 0; iteration < 64u; ++iteration) {
    auto *temporary = table.FindOrCreateHeap(0x200000u + uint64_t{iteration} * 0x100000u,
        0u, 32u, BindlessTable::Images2D, 8192u, resource);
    Require(name, "required new-owner work", temporary != nullptr,
            "allocator failed while establishing actual numeric-region reuse");
    temporary->valid_keys = 8192u;
    temporary->checked_frame = iteration + 1u;
    if (temporary->region == old_region) { new_owner = temporary; break; }
  }
  Require(name, "actual region reuse", new_owner != nullptr &&
              new_owner->region_generation != old_generation &&
              new_owner->region_snapshot == old_snapshot &&
              !BindlessLifecycleTestAccess::RegionOwnership(table, old_generation).exists &&
              BindlessLifecycleTestAccess::SnapshotWord(table, old_region, 3u) == 1u,
          "fixture did not reuse the old numeric region while retaining its older completed snapshot");
  requests.clear();
  table.TakeRequests(*new_owner, requests);
  Require(name, "old snapshot ownership rejection", requests.empty(),
          "new heap accepted another owner's old feedback snapshot (independent of key-bounds policy)");
  BindlessLifecycleTestAccess::WriteFeedback(table, new_owner->region, 5u, 1u);
  table.RecordFeedbackSnapshot(scheduler);
  scheduler.Finish();
  Require(name, "new completed snapshot", table.SnapshotReady(scheduler),
          "new-owner native feedback copy did not complete");
  requests.clear();
  table.TakeRequests(*new_owner, requests);
  Require(name, "new-owner positive control", requests == std::vector<uint32_t>{5u},
          "new owner retained an old flag or lost its genuine current snapshot request");
  std::printf("[host] %-32s ok (key_bounds=%u; real feedback copies; no shader issuer claim)\n",
              name, static_cast<unsigned>(BindlessTable::KeyBoundsEnabled()));
}
} // namespace
} // namespace Libs::Graphics
