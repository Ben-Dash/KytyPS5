// Synthetic opt-in regression for tests/ResourceTrackingTests.cpp.
// Include inside its anonymous namespace, after Fixture/LinearTestMemory helpers.
// No production source, sampler flags, heap snapshots or mapping slots are injected.
// Intended baseline RED: dynamic sampler source was replaced with the default S#.

struct DynamicSamplerPlanningFixture {
  std::unique_ptr<Fixture> fixture;
  Value image_key, sampler_key, image_handle, sampler_handle;
};

DynamicSamplerPlanningFixture MakeDynamicSamplerPlanningFixture() {
  DynamicSamplerPlanningFixture result;
  result.fixture = std::make_unique<Fixture>();
  auto& f = *result.fixture;
  f.program.bindless_images = true; // Actual existing supported-host planning option.
  const auto image_table = f.Buffer({f.UserData(0u), f.UserData(1u),
                                    f.UserData(2u), f.UserData(3u)});
  const auto sampler_table = f.Buffer({f.UserData(4u), f.UserData(5u),
                                      f.UserData(6u), f.UserData(7u)});
  const auto lane = f.Emit(ValueOpcode::GetBuiltin,
      {Value(static_cast<uint32_t>(StageInputKind::GlobalInvocationId)), Value(0u)});
  const auto selected_lane = f.Emit(ValueOpcode::ReadFirstLane, {lane, Value(true)});
  const auto image_bit = f.Emit(ValueOpcode::BitwiseXor32, {selected_lane, f.UserData(8u)});
  result.image_key = f.Emit(ValueOpcode::BitwiseAnd32, {image_bit, Value(1u)});
  const auto sampler_bit = f.Emit(ValueOpcode::ShiftRightLogical32,
                                  {selected_lane, Value(1u)});
  const auto changed_sampler_bit = f.Emit(ValueOpcode::BitwiseXor32,
                                         {sampler_bit, f.UserData(9u)});
  result.sampler_key = f.Emit(ValueOpcode::BitwiseAnd32,
                              {changed_sampler_bit, Value(1u)});
  const auto image_offset = f.Emit(ValueOpcode::IMul32, {result.image_key, Value(32u)});
  const auto sampler_offset = f.Emit(ValueOpcode::IMul32, {result.sampler_key, Value(16u)});
  std::array<Value, 8> image_words;
  std::array<Value, 4> sampler_words;
  for (uint32_t word = 0; word < image_words.size(); ++word) {
    MemoryInfo read;
    read.kind = ResourceKind::ScalarBuffer;
    read.offset = 16u + word * sizeof(uint32_t);
    image_words[word] = f.Emit(ValueOpcode::ReadConstBuffer, {image_table, image_offset},
                               f.AddMemory(read, 0x100u + word * 4u));
  }
  for (uint32_t word = 0; word < sampler_words.size(); ++word) {
    MemoryInfo read;
    read.kind = ResourceKind::ScalarBuffer;
    read.offset = 16u + word * sizeof(uint32_t);
    sampler_words[word] = f.Emit(ValueOpcode::ReadConstBuffer, {sampler_table, sampler_offset},
                                 f.AddMemory(read, 0x140u + word * 4u));
  }
  result.image_handle = f.Image(image_words, 0x160u);
  result.sampler_handle = f.Sampler(sampler_words, 0x160u);
  MemoryInfo sample;
  sample.kind = ResourceKind::Image;
  sample.image_dimension = Decoder::ImageDimension::Dim2D;
  sample.image_sample_flags = Decoder::ImageSampleFlagLevelZero;
  const auto sampled = f.Emit(ValueOpcode::ImageSampleRaw,
      {result.image_handle, result.sampler_handle, f.ImageAddress()},
      f.AddMemory(sample, 0x160u));
  const auto sampled_x = f.Emit(ValueOpcode::CompositeExtractU32x4, {sampled, Value(0u)});
  f.Emit(ValueOpcode::ReferenceU32, {sampled_x});
  return result;
}

void CheckDynamicSamplerPlan(const DynamicSamplerPlanningFixture& fixture,
                            const ResourcePlan& plan) {
  Check(plan.resource_tracking_complete && plan.info.images.size() == 1u &&
            plan.info.samplers.size() == 1u,
        "dynamic sampler fixture must reach complete planning; setup failure is not RED");
  const auto image_source = plan.info.images[0].source;
  const auto sampler_source = plan.info.samplers[0].source;
  Check(image_source < plan.descriptor_sources.size() &&
            sampler_source < plan.descriptor_sources.size(),
        "dynamic sampler source indices must remain valid");
  const auto& image = plan.descriptor_sources[image_source];
  const auto& sampler = plan.descriptor_sources[sampler_source];
  Check(image.indirect_descriptor.has_value() && image.indirect_descriptor->bindless,
        "independent image heap positive control must be planned before sampler RED");
  Check(sampler.indirect_descriptor.has_value() && sampler.indirect_descriptor->bindless,
        "INTENDED RED: dynamic sampler source was replaced with the default S#");
  Check(image.indirect_descriptor->table_source != sampler.indirect_descriptor->table_source &&
            image.indirect_descriptor->table_offset == 16u &&
            image.indirect_descriptor->table_stride == 32u &&
            sampler.indirect_descriptor->table_offset == 16u &&
            sampler.indirect_descriptor->table_stride == 16u,
        "independent image and sampler roots, offsets and strides must be preserved");
  Check(fixture.image_key != fixture.sampler_key &&
            fixture.image_handle.ResolveInstruction()->Arg(0).Resolve() == fixture.image_key.Resolve() &&
            fixture.sampler_handle.ResolveInstruction()->Arg(0).Resolve() == fixture.sampler_key.Resolve(),
        "sampled instruction must retain two independently computed GPU selector keys");
}

void TestDynamicSamplerPlanningAndMaterialization() {
  auto fixture = MakeDynamicSamplerPlanningFixture();
  fixture.fixture->PlanAndTrack();
  auto plan = ExtractResourcePlan(fixture.fixture->program);
  CheckDynamicSamplerPlan(fixture, plan);

  LinearTestMemory memory;
  memory.base = 0x100000u;
  std::ranges::fill(memory.words, 0x8badf00du);
  constexpr uint64_t sampler_base_offset = 0x400u;
  constexpr std::array<std::array<uint32_t, 4>, 2> sampler_records{{
      {0u, 0u, 0u, 0u},                         // nearest, normalized, wrap, LOD0
      {2u | (2u << 3u) | (2u << 6u), 0u, 0u, 0u}}}; // nearest, clamp-to-edge
  for (uint32_t key = 0; key < 2u; ++key) {
    std::array<uint32_t, 8> image{};
    image[0] = 0x200u + key * 0x100u;
    image[1] = (static_cast<uint32_t>(Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float) << 20u) |
        (1u << 30u); // Width-1 low two bits live in dword1, not dword2.
    image[2] = 1u << 14u; // Height-1=1; genuine2x2 image descriptor.
    image[3] = Libs::Graphics::DstSel(4, 5, 6, 7) |
        (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u);
    std::copy(image.begin(), image.end(), memory.words.begin() + (16u + key * 32u) / 4u);
    std::copy(sampler_records[key].begin(), sampler_records[key].end(),
              memory.words.begin() + (sampler_base_offset + 16u + key * 16u) / 4u);
  }
  const auto original_memory = memory.words;
  memory.watched_address = memory.base + sampler_base_offset + 16u;
  // Raw V# bounds are bytes because stride=0. Two records plus one prefix.
  std::array<uint32_t, 10> user_data{
      static_cast<uint32_t>(memory.base), 0u, 80u, 0u,
      static_cast<uint32_t>(memory.base + sampler_base_offset), 0u, 48u, 0u, 0u, 0u};
  const SrtRuntime runtime{.user_data = user_data,
      .read_memory = ReadLinearTestMemory, .userdata = &memory,
      .read_specialization_memory = ReadLinearTestMemory};

  // Full, exactly-one-record, no-complete-record and partial-tail bounds.
  // The planner must keep the exact byte bound; host mirroring may expose only
  // floor((bytes-prefix)/16) complete records, never a rounded-up final record.
  for (const uint32_t bytes : {48u, 32u, 31u, 49u}) {
    user_data[6] = bytes;
    for (const uint32_t changed_keys : {0u, 1u, 0u}) {
      user_data[8] = changed_keys;
      user_data[9] = changed_keys ^ 1u;
      ResourceSnapshot snapshot, reference;
      ResourceSpecialization specialization, reference_specialization;
      ReadLog reads;
      Check(MaterializeResourcesLogged(plan, runtime, snapshot, specialization, reads) &&
                MaterializeResourcesReference(plan, runtime, reference, reference_specialization),
            "dynamic sampler resource materialization must succeed for valid bounded heap roots");
      Check(snapshot.bindless_heaps == reference.bindless_heaps &&
                snapshot.bindless_sampler_heaps == reference.bindless_sampler_heaps &&
                snapshot.flattened_srt == reference.flattened_srt &&
                snapshot.samplers == reference.samplers &&
                specialization == reference_specialization,
            "compiled and reference samplers must share heap/mapping/selector materialization");
      Check(snapshot.bindless_heaps.size() == 1u && snapshot.bindless_sampler_heaps.size() == 1u,
            "INTENDED RED: missing bindless_sampler_heaps producer lost real material sampler identity");
      const auto& image = snapshot.bindless_heaps[0];
      const auto& sampler = snapshot.bindless_sampler_heaps[0];
      Check(image.base == memory.base && image.size == 80u && image.table_offset == 16u &&
                image.record_stride == 32u && sampler.base == memory.base + sampler_base_offset &&
                sampler.size == bytes && sampler.table_offset == 16u && sampler.sampler == 0u &&
                image.mapping_offset != sampler.mapping_offset,
            "materialization must retain two real roots and exact raw V# byte bounds");
      Check(sampler.mapping_offset + 1u < snapshot.flattened_srt.size() &&
                snapshot.flattened_srt[sampler.mapping_offset] == 0u &&
                snapshot.flattened_srt[sampler.mapping_offset + 1u] == 0u,
            "unprepared sampler mapping must fail closed before host region/count patches");
      Check(memory.watched_reads == 0u && memory.words == original_memory,
            "CPU planning must not preselect a lane's S# or read/write sampler records and canaries");
      Check(std::ranges::none_of(reads.entries, [&](const ReadLog::Entry& read) {
              return read.address >= memory.base + sampler_base_offset &&
                     read.address < memory.base + sampler_base_offset + 0x100u;
            }), "GPU-selected sampler records must remain host-mirror inputs, not scalar CPU selection");
    }
  }
  ResourceSnapshot final_snapshot;
  ResourceSpecialization final_specialization;
  Check(MaterializeResources(plan, runtime, final_snapshot, final_specialization),
        "dynamic sampler final specialization must be reproducible");
  ApplyResourceSpecialization(fixture.fixture->program, final_specialization);
  Check(fixture.fixture->program.info.samplers[0].bindless &&
            fixture.fixture->program.info.samplers[0].bindless_mapping_offset ==
                final_snapshot.bindless_sampler_heaps[0].mapping_offset,
        "final sampler resource must activate BindlessSamplerSlot at its own mapping offset");
}
