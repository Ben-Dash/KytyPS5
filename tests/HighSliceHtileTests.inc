// Include INSIDE ShaderRecompilerComputeTests.cpp's existing test namespace,
// after VulkanHarness and its RenderExecutorTestAccess/Require helpers.
// Setup uses the real native depth discovery/registration path, not a fake
// metadata entry or a proposed production representation.
struct HighSliceHtileGuest {
  static constexpr uint64_t Base=0x0000000201200000ull;
  static constexpr uint64_t Bytes=16u*1024u*1024u;
  static constexpr uint64_t Metadata=Base+8u*1024u*1024u;
  int64_t direct=-1;
  HighSliceHtileGuest() {
    Require("HighSliceHtile","bounded direct allocation",
      Libs::LibKernel::Memory::KernelAllocateDirectMemory(0,Libs::LibKernel::Memory::KernelGetDirectMemorySize(),
        Bytes,0x10000u,0,&direct)==0,"setup allocation failed; not a semantic RED");
    void* address=reinterpret_cast<void*>(Base);
    Require("HighSliceHtile","tracked synthetic mapping",
      Libs::LibKernel::Memory::KernelMapDirectMemory(&address,Bytes,0x3,0x10,direct,0x10000u)==0 &&
      address==reinterpret_cast<void*>(Base),"setup mapping failed; not a semantic RED");
    std::memset(address,0,Bytes);
  }
  ~HighSliceHtileGuest() {
    Require("HighSliceHtile","guest cleanup",
      Libs::LibKernel::Memory::KernelMunmap(Base,Bytes)==0 &&
      Libs::LibKernel::Memory::KernelReleaseDirectMemory(direct,Bytes)==0,
      "task-owned mapping was not released");
  }
};

struct HighSliceHtileFixture {
  static constexpr uint32_t Layers=121u,Side=8u;
  HighSliceHtileGuest guest;
  Libs::Graphics::RenderContext& context;
  HW::Context registers{}; HW::UserConfig user_config{}; HW::Shader shaders{};
  RenderDepthInfo depth{};
  bool finished=false;
  explicit HighSliceHtileFixture(VulkanHarness& vulkan):context(vulkan.RuntimeRenderer()) {
    Require("HighSliceHtile","actual device layer capability",
      vulkan.RuntimeContext().GetPhysicalDeviceProperties().limits.maxImageArrayLayers>=Layers,
      "device cannot represent the fixture; not a semantic RED");
    context.MapMemory(HighSliceHtileGuest::Base,HighSliceHtileGuest::Bytes);
    HW::DepthRenderTarget target{};
    target.z_info.format=Prospero::DepthFormat::kZ32F;
    target.z_info.texture_compatibility=Prospero::TextureCompatiblePlaneCompression::kEnable;
    target.z_info.htile_acceleration=true;
    target.z_info.z_compare_base=Prospero::ZCompareBase::kZMax;
    target.z_read_base_addr=target.z_write_base_addr=HighSliceHtileGuest::Base;
    target.htile_data_base_addr=HighSliceHtileGuest::Metadata;
    target.depth_view.slice_start=0u;target.depth_view.slice_max=Layers-1u;
    target.size.valid=true;target.size.x_max=target.size.y_max=Side-1u;
    registers.SetDepthRenderTarget(target);
    HW::DepthControl control{};control.z_enable=true;control.z_write_enable=true;
    control.zfunc=static_cast<uint8_t>(vk::CompareOp::eAlways);
    registers.SetDepthControl(control);registers.SetRenderControl({});
    registers.SetDepthClearValue(0.375f);
    auto& scheduler=context.GetCommandScheduler();
    scheduler.Begin(registers,user_config,shaders);
    RenderExecutorTestAccess::ResolveRenderDepthTarget(context.GetRenderExecutor(),scheduler.Current(),depth);
    Require("HighSliceHtile","actual discovered metadata footprint",
      depth.image_id && depth.desc.info.resources.layers==Layers &&
      depth.desc.info.pixel_format==vk::Format::eD32Sfloat &&
      depth.desc.info.metadata.kind==ImageMetadataKind::Htile &&
      depth.desc.info.metadata.range.address==HighSliceHtileGuest::Metadata &&
      depth.desc.info.metadata.range.size==Libs::Graphics::HtileWriter::LayerBytes(Side,Side)*Layers &&
      depth.desc.info.data.address==HighSliceHtileGuest::Base &&
      depth.desc.info.data.size<=HighSliceHtileGuest::Metadata-HighSliceHtileGuest::Base &&
      context.IsMapped(depth.desc.info.metadata.range.address,depth.desc.info.metadata.range.size) &&
      context.IsMapped(depth.desc.info.data.address,depth.desc.info.data.size),
      "depth/metadata registration geometry is not valid; not a semantic RED");
    auto& cache=context.GetTextureCache();
    (void)cache.FindDepthTarget(depth.image_id,depth.desc);
    Require("HighSliceHtile","real registered owner and layer count",
      cache.IsMeta(HighSliceHtileGuest::Metadata) &&
      cache.MetaOwner(HighSliceHtileGuest::Metadata)==HighSliceHtileGuest::Base &&
      cache.GetImage(depth.image_id).info.resources.layers==Layers,
      "metadata was not registered by the real depth owner");
  }
  void Finish() {
    if(finished)return;
    auto& scheduler=context.GetCommandScheduler();
    RenderExecutorTestAccess::ResetBindings(context.GetRenderExecutor());
    const auto tick=scheduler.CurrentTick();scheduler.Finish();scheduler.WaitPriorityOperations(tick);
    context.UnmapMemory(HighSliceHtileGuest::Base,HighSliceHtileGuest::Bytes);
    finished=true;
  }
  ~HighSliceHtileFixture() { Finish(); }
};

void CheckHighSliceHtileState(VulkanHarness& vulkan) {
  const char* name="HighSliceHtileState";
  uint32_t initial_clear_count=0u,expanded_count=0u;
  bool all_clear=false,one_expand=false,one_clear=false,last_clear=false;
  bool bounds=false,unregistered=false,memo=false;
  {
    HighSliceHtileFixture fixture(vulkan);
    auto& cache=fixture.context.GetTextureCache();
    constexpr uint64_t meta=HighSliceHtileGuest::Metadata;
    Require(name,"initial expansion",cache.ExpandMeta(meta),"registered metadata expansion failed");
    for(uint32_t layer=0u;layer<HighSliceHtileFixture::Layers;++layer)
      expanded_count+=cache.IsMetaCleared(meta,layer) ? 1u : 0u;
    bounds=!cache.TouchMeta(meta,HighSliceHtileFixture::Layers,true) &&
      !cache.TouchMeta(meta,UINT32_MAX,true) && !cache.IsMetaCleared(meta,HighSliceHtileFixture::Layers);
    Require(name,"registered whole-array clear",cache.ClearMeta(meta),"registered metadata clear failed");
    for(uint32_t layer=0u;layer<HighSliceHtileFixture::Layers;++layer)
      initial_clear_count+=cache.IsMetaCleared(meta,layer) ? 1u : 0u;
    all_clear=initial_clear_count==HighSliceHtileFixture::Layers;
    // Query same high slice repeatedly to prime the actual generation memo.
    const bool before=cache.IsMetaCleared(meta,32u) && cache.IsMetaCleared(meta,32u);
    const bool touched=cache.TouchMeta(meta,32u,false);
    one_expand=touched && !cache.IsMetaCleared(meta,32u) && cache.IsMetaCleared(meta,31u) &&
      cache.IsMetaCleared(meta,33u) && cache.IsMetaCleared(meta,120u);
    const bool after=cache.TouchMeta(meta,32u,true) && cache.IsMetaCleared(meta,32u);
    one_clear=after && cache.IsMetaCleared(meta,31u) && cache.IsMetaCleared(meta,33u);
    memo=before && touched && after;
    Require(name,"second whole-array expansion",cache.ExpandMeta(meta),"registered metadata expansion failed");
    last_clear=cache.TouchMeta(meta,120u,true) && cache.IsMetaCleared(meta,120u) &&
      !cache.IsMetaCleared(meta,119u) && !cache.IsMetaCleared(meta,32u) && !cache.IsMetaCleared(meta,0u);
    const uint64_t unknown=HighSliceHtileGuest::Base+HighSliceHtileGuest::Bytes+0x8000u;
    unregistered=!cache.ClearMeta(unknown) && !cache.ExpandMeta(unknown) &&
      !cache.TouchMeta(unknown,0u,true) && !cache.IsMetaCleared(unknown,0u);
    fixture.Finish(); // semantic assertions only after native/guest cleanup
  }
  std::printf("[htile-high-state] allClear=%u/121 expanded=%u oneExpand=%u oneClear=%u lastOnly=%u memo=%u bounds=%u unknown=%u\n",
    initial_clear_count,expanded_count,one_expand,one_clear,last_clear,memo,bounds,unregistered);
  Require(name,"all registered layers participate in whole clear",all_clear && expanded_count==0u,
    "registered layers beyond31 lost the whole-array pending HTile clear");
  Require(name,"one-slice state and memo refresh",one_expand && one_clear && last_clear && memo,
    "a high slice update lost state, changed neighbors or retained a stale memo");
  Require(name,"registered footprint bounds and unregistered refusal",bounds && unregistered,
    "layer beyond actual owner extent or an unregistered metadata address was accepted");
}

struct HighSliceHtileDownload {
  GraphicContext& graphics; vk::Buffer buffer=nullptr; VmaAllocation allocation=nullptr;
  void* data=nullptr;uint64_t size=0u;
  HighSliceHtileDownload(GraphicContext& context,uint64_t bytes):graphics(context),size(bytes) {
    vk::BufferCreateInfo info{};info.size=bytes;info.usage=vk::BufferUsageFlagBits::eTransferDst;
    VmaAllocationCreateInfo memory{};memory.usage=VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    memory.flags=VMA_ALLOCATION_CREATE_MAPPED_BIT|VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    memory.requiredFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    VkBuffer native=VK_NULL_HANDLE;VmaAllocationInfo mapping{};
    RequireVulkanSuccess(static_cast<vk::Result>(vmaCreateBuffer(graphics.allocator,
      reinterpret_cast<const VkBufferCreateInfo*>(&info),&memory,&native,&allocation,&mapping)),"high-slice readback allocation");
    buffer=native;data=mapping.pMappedData;
    Require("HighSliceHtile","host readback mapping",data!=nullptr,"setup mapping failed");
  }
  void Invalidate() { RequireVulkanSuccess(static_cast<vk::Result>(vmaInvalidateAllocation(graphics.allocator,
    allocation,0u,size)),"high-slice readback invalidate"); }
  ~HighSliceHtileDownload() { if(buffer!=nullptr)vmaDestroyBuffer(graphics.allocator,static_cast<VkBuffer>(buffer),allocation); }
};

void CheckHighSliceHtileGpu(VulkanHarness& vulkan) {
  const char* name="HighSliceHtileGpu";
  constexpr uint32_t texels=HighSliceHtileFixture::Side*HighSliceHtileFixture::Side;
  constexpr uint32_t copies_count=7u;
  std::array<uint32_t,texels*copies_count> actual{};
  std::array<bool,4> load_clear{};
  {
    HighSliceHtileFixture fixture(vulkan);
    auto& context=fixture.context;auto& scheduler=context.GetCommandScheduler();
    auto& cache=context.GetTextureCache();auto& executor=context.GetRenderExecutor();
    HighSliceHtileDownload download(vulkan.RuntimeContext(),sizeof(actual));
    constexpr uint64_t meta=HighSliceHtileGuest::Metadata;
    Require(name,"real prior depth producer",cache.ClearImageFromBuffer(scheduler.Current(),
      fixture.depth.desc.info.data.address,fixture.depth.desc.info.data.size,std::bit_cast<uint32_t>(0.625f)),
      "setup did not seed the actual registered depth image");
    Require(name,"pending whole-array HTile clear",cache.ClearMeta(meta),"metadata registration failed");
    const auto acquire=[&](uint32_t layer,uint32_t pass) {
      auto depth=fixture.depth;depth.desc.view_info.type=vk::ImageViewType::e2D;
      depth.desc.view_info.base_layer=layer;depth.desc.view_info.layer_count=1u;
      depth.depth_clear_enable=false;depth.depth_load_clear_enable=false;
      depth.depth_write_enable=false;depth.depth_test_enable=true;
      depth.depth_compare_op=vk::CompareOp::eAlways;depth.depth_clear_value=0.375f;
      RenderExecutorTestAccess::BindRenderTarget(executor,depth.image_id);
      RenderColorInfo no_color{};
      const auto rendering=RenderExecutorTestAccess::AcquireRenderTargets(executor,scheduler.Current(),&no_color,0u,depth);
      load_clear[pass]=depth.depth_load_clear_enable;
      scheduler.BeginRendering(rendering);scheduler.EndRendering();
      RenderExecutorTestAccess::ResetBindings(executor);
    };
    const auto copy=[&](uint32_t layer,uint32_t destination) {
      const vk::BufferImageCopy region{destination*texels*4u,0u,0u,
        {vk::ImageAspectFlagBits::eDepth,0u,layer,1u},{},
        vk::Extent3D{HighSliceHtileFixture::Side,HighSliceHtileFixture::Side,1u}};
      cache.GetImage(fixture.depth.image_id).Download(std::span{&region,1},download.buffer,0u,download.size,download.size);
    };
    acquire(31u,0u);copy(31u,0u);copy(30u,1u);
    acquire(32u,1u);copy(32u,2u);copy(33u,3u);
    acquire(120u,2u);copy(120u,4u);copy(119u,5u);
    // A real native GPU write after consuming slice32's pending clear. Its
    // next acquisition must LOAD0.75, preserving once-only metadata consumption.
    auto& image=cache.GetImage(fixture.depth.image_id);
    image.Transit(vk::ImageLayout::eTransferDstOptimal,vk::AccessFlagBits2::eTransferWrite,
      ImageSubresourceRange{0u,1u,32u,1u},scheduler.Current().Handle());
    const vk::ImageSubresourceRange native_range{vk::ImageAspectFlagBits::eDepth,0u,1u,32u,1u};
    const vk::ClearDepthStencilValue later{0.75f,0u};
    scheduler.Current().Handle().clearDepthStencilImage(image.backing.image,vk::ImageLayout::eTransferDstOptimal,&later,1u,&native_range);
    cache.MarkGpuWritten(fixture.depth.image_id);
    acquire(32u,3u);copy(32u,6u);
    vk::MemoryBarrier2 host{};host.srcStageMask=vk::PipelineStageFlagBits2::eTransfer;
    host.srcAccessMask=vk::AccessFlagBits2::eTransferWrite;host.dstStageMask=vk::PipelineStageFlagBits2::eHost;
    host.dstAccessMask=vk::AccessFlagBits2::eHostRead;
    vk::DependencyInfo dependency{};dependency.memoryBarrierCount=1u;dependency.pMemoryBarriers=&host;
    scheduler.Current().Handle().pipelineBarrier2(dependency);
    const auto tick=scheduler.CurrentTick();scheduler.Finish();scheduler.WaitPriorityOperations(tick);
    download.Invalidate();std::memcpy(actual.data(),download.data,sizeof(actual));
    fixture.Finish();
  }
  const auto plane_is=[&](uint32_t plane,float expected) {
    return std::ranges::all_of(std::span{actual}.subspan(plane*texels,texels),
      [=](uint32_t value){return value==std::bit_cast<uint32_t>(expected);});
  };
  std::printf("[htile-high-gpu] loadClear31/32/120/repeat32=%u,%u,%u,%u firstDepthBits=%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
    load_clear[0],load_clear[1],load_clear[2],load_clear[3],actual[0],actual[texels],actual[2u*texels],
    actual[3u*texels],actual[4u*texels],actual[5u*texels],actual[6u*texels]);
  Require(name,"slice31 depth-control neighbor",load_clear[0] && plane_is(0u,0.375f),
    "ordinary tracked slice31 failed; not the intended high-slice RED");
  Require(name,"pending clears32/120 reach real depth readback",load_clear[1] && load_clear[2] &&
    plane_is(2u,0.375f) && plane_is(4u,0.375f),"registered high slices missed their pending HTile clear");
  Require(name,"disjoint layer preservation",plane_is(1u,0.625f) && plane_is(3u,0.625f) && plane_is(5u,0.625f),
    "a one-slice clear touched its neighboring depth layer");
  Require(name,"one-time clear preserves later GPU producer",!load_clear[3] && plane_is(6u,0.75f),
    "a consumed high-slice clear overwrote the next real GPU depth value");
}
