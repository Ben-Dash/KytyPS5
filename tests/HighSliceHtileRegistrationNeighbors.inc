// Include after HighSliceHtileTests.inc in the existing test namespace.
// The original high-slice state/GPU regression and all of its oracles stay unchanged.
// These cases use the same real depth discovery and public final-acquisition path.
void CheckHighSliceHtileRegistrationNeighbors(VulkanHarness& vulkan) {
  constexpr const char* name="HighSliceHtileRegistrationNeighbors";
  constexpr uint64_t alternate_metadata=HighSliceHtileGuest::Metadata+4u*1024u*1024u;
  uint32_t initial_all=0u,partial_seed_count=0u,expanded_seed_count=0u;
  bool smaller=false,expanded_growth=false,clear_growth=false,bounds=false,memo=false;
  const auto CountCleared=[](TextureCache& cache,uint64_t metadata,uint32_t layers) {
    uint32_t count=0u;
    for(uint32_t layer=0u;layer<layers;++layer)
      count+=cache.IsMetaCleared(metadata,layer)?1u:0u;
    return count;
  };
  const auto Registration=[](const HighSliceHtileFixture& fixture,uint32_t layers) {
    auto desc=fixture.depth.desc;
    const auto depth_slice=desc.info.data.size/desc.info.resources.layers;
    const auto metadata_slice=desc.info.metadata.range.size/desc.info.resources.layers;
    desc.info.resources.layers=layers;
    desc.info.data.size=depth_slice*layers;
    desc.info.mip_layout[0].size=desc.info.data.size;
    desc.info.metadata.range={alternate_metadata,metadata_slice*layers};
    desc.view_info.type=vk::ImageViewType::e2D;
    desc.view_info.base_layer=0u;desc.view_info.layer_count=1u;
    return desc;
  };
  {
    HighSliceHtileFixture fixture(vulkan);
    auto& cache=fixture.context.GetTextureCache();
    initial_all=CountCleared(cache,HighSliceHtileGuest::Metadata,HighSliceHtileFixture::Layers);
    // UINT32_MAX is the internal all-clear sentinel. A genuine partial low-word
    // mask is different: high registered slices have no seed bit in that mask.
    cache.GetImage(fixture.depth.image_id).info.htile_clear_mask=0x80000001u;
    auto desc=Registration(fixture,HighSliceHtileFixture::Layers);
    (void)cache.FindDepthTarget(fixture.depth.image_id,desc);
    partial_seed_count=CountCleared(cache,alternate_metadata,HighSliceHtileFixture::Layers);
    Require(name,"independent partial low-word seed",cache.IsMetaCleared(alternate_metadata,0u) &&
      cache.IsMetaCleared(alternate_metadata,31u) && !cache.IsMetaCleared(alternate_metadata,32u) &&
      !cache.IsMetaCleared(alternate_metadata,120u),"a partial seed fabricated high-slice clears");
    fixture.Finish();
  }
  {
    HighSliceHtileFixture fixture(vulkan);
    auto& cache=fixture.context.GetTextureCache();
    cache.GetImage(fixture.depth.image_id).info.htile_clear_mask=0u;
    auto small_desc=Registration(fixture,33u);
    auto full=Registration(fixture,HighSliceHtileFixture::Layers);
    (void)cache.FindDepthTarget(fixture.depth.image_id,small_desc);
    expanded_seed_count=CountCleared(cache,alternate_metadata,33u);
    const bool before=!cache.IsMetaCleared(alternate_metadata,120u) &&
      !cache.TouchMeta(alternate_metadata,33u,true) && cache.TouchMeta(alternate_metadata,32u,true);
    const auto generation=cache.SurfaceMetaGeneration();
    (void)cache.FindDepthTarget(fixture.depth.image_id,full);
    expanded_growth=before && cache.IsMetaCleared(alternate_metadata,32u) &&
      !cache.IsMetaCleared(alternate_metadata,33u) && !cache.IsMetaCleared(alternate_metadata,120u) &&
      cache.TouchMeta(alternate_metadata,120u,true) && cache.IsMetaCleared(alternate_metadata,120u);
    memo=cache.SurfaceMetaGeneration()>generation;
    (void)cache.FindDepthTarget(fixture.depth.image_id,small_desc);
    smaller=cache.IsMetaCleared(alternate_metadata,32u) && cache.IsMetaCleared(alternate_metadata,120u);
    bounds=!cache.TouchMeta(alternate_metadata,HighSliceHtileFixture::Layers,true) &&
      !cache.TouchMeta(alternate_metadata,UINT32_MAX,true) &&
      !cache.IsMetaCleared(alternate_metadata,HighSliceHtileFixture::Layers);
    Require(name,"expanded seed after grown registration",cache.ExpandMeta(alternate_metadata) &&
      CountCleared(cache,alternate_metadata,HighSliceHtileFixture::Layers)==0u,
      "whole expansion retained higher-word pending clears");
    Require(name,"known allocation after smaller registration",cache.ClearMeta(alternate_metadata) &&
      CountCleared(cache,alternate_metadata,HighSliceHtileFixture::Layers)==HighSliceHtileFixture::Layers,
      "a smaller bound view discarded the already registered array extent");
    fixture.Finish();
  }
  {
    HighSliceHtileFixture fixture(vulkan);
    auto& cache=fixture.context.GetTextureCache();
    auto small_desc=Registration(fixture,33u);
    auto full=Registration(fixture,HighSliceHtileFixture::Layers);
    (void)cache.FindDepthTarget(fixture.depth.image_id,small_desc);
    const bool consumed=cache.TouchMeta(alternate_metadata,0u,false) &&
      cache.TouchMeta(alternate_metadata,32u,false) && !cache.IsMetaCleared(alternate_metadata,120u);
    (void)cache.FindDepthTarget(fixture.depth.image_id,full);
    (void)cache.FindDepthTarget(fixture.depth.image_id,small_desc);
    (void)cache.FindDepthTarget(fixture.depth.image_id,full);
    clear_growth=consumed && !cache.IsMetaCleared(alternate_metadata,0u) &&
      !cache.IsMetaCleared(alternate_metadata,32u) && cache.IsMetaCleared(alternate_metadata,31u) &&
      cache.IsMetaCleared(alternate_metadata,33u) && cache.IsMetaCleared(alternate_metadata,120u);
    fixture.Finish();
  }
  std::printf("[htile-registration-neighbors] initialAll=%u/121 partialSeed=%u/2 expandedSeed=%u/0 smaller=%u growthExpanded=%u growthClear=%u memo=%u bounds=%u\n",
    initial_all,partial_seed_count,expanded_seed_count,smaller,expanded_growth,clear_growth,memo,bounds);
  Require(name,"all-clear sentinel spans registered extent",initial_all==HighSliceHtileFixture::Layers,
    "the internal all-clear initial state lost registered high slices");
  Require(name,"initial legacy and expanded seeds",partial_seed_count==2u && expanded_seed_count==0u,
    "initial seed distinction was not preserved");
  Require(name,"partial/growing registration and memo",smaller && expanded_growth && clear_growth && memo && bounds,
    "registration discarded consumed layers, lost a grown slice or memoized old bounds");
}
