namespace Libs::Graphics {
namespace {
void CheckRetiredImageMixedSize(VulkanHarness& vulkan) {
  constexpr const char* name="RetiredImageMixedSize";
  constexpr uint32_t sentinel=0xdeadbeefu;constexpr uint64_t guard=256u;
  auto& graphics=vulkan.RuntimeContext();auto& scheduler=vulkan.RuntimeRenderer().GetCommandScheduler();
  Require(name,"direct real worker",GuestGpu::IsGpuThread()&&!CommandRecordingDeferred(),
      "native standalone worker setup required");
  size_t count=0;
  Require(name,"initial pool empty",NativeImageRecyclerRetainedBytesForTests(graphics.allocator,&count)==0u&&count==0u,
      "unexpected preexisting pool is setup failure");
  const char* previous_raw=std::getenv("KYTY_IMAGE_RECYCLE_MAX_BYTES");
  const std::string previous=previous_raw!=nullptr?previous_raw:"";
  HW::Context registers{};HW::UserConfig user_config{};HW::Shader shaders{};
  scheduler.Begin(registers,user_config,shaders);
  std::array<std::unique_ptr<Image>,4> images;std::array<uint64_t,4> allocation_bytes{};
  constexpr std::array<uint32_t,4> widths{32u,64u,128u,128u};
  constexpr std::array<uint32_t,4> patterns{0x7139a5c3u,0xa3e97513u,0x71953ae3u,0xc395713au};
  for(size_t index=0;index<images.size();++index) {
    ImageInfo info{};info.pixel_format=vk::Format::eR8G8B8A8Uint;
    info.guest_format=Prospero::BufferFormat::k8_8_8_8UInt;info.type=Prospero::ImageType::kColor2D;
    info.extent={widths[index],widths[index],1};info.resources={1,1};info.pitch=widths[index];
    info.bytes_per_block=4;info.samples=1;info.tile_mode=Prospero::TileMode::kLinear;
    images[index]=std::make_unique<Image>(graphics,scheduler,info);
    VmaAllocationInfo native{};vmaGetAllocationInfo(graphics.allocator,images[index]->backing.allocation,&native);
    allocation_bytes[index]=native.size;
  }
  Require(name,"three bounded actual allocation sizes",allocation_bytes[0]>0u&&
      allocation_bytes[0]<allocation_bytes[1]&&allocation_bytes[1]<allocation_bytes[2]&&
      allocation_bytes[2]==allocation_bytes[3]&&allocation_bytes[2]<4u*1024u*1024u&&
      allocation_bytes[0]+allocation_bytes[1]<allocation_bytes[2],
      "real driver must produce3 distinct tiny sizes; no fabricated allocation size or pressure");
  auto native_check=[&](size_t index,bool upload) {
    const uint64_t bytes=uint64_t{widths[index]}*widths[index]*4u;
    FetchGpuBuffer source(graphics,bytes,vk::BufferUsageFlagBits::eTransferSrc);
    std::fill_n(static_cast<uint32_t*>(source.data),bytes/4u,patterns[index]);source.Flush();
    FetchGpuBuffer output(graphics,bytes+2u*guard,vk::BufferUsageFlagBits::eTransferDst);
    std::fill_n(static_cast<uint32_t*>(output.data),output.size/4u,sentinel);output.Flush();
    vk::BufferImageCopy copy{};copy.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1};
    copy.imageExtent={widths[index],widths[index],1};
    if(upload)images[index]->Upload(std::span{&copy,size_t{1}},source.buffer,0,bytes,bytes);
    copy.bufferOffset=guard;
    images[index]->Download(std::span{&copy,size_t{1}},output.buffer,0,output.size,output.size);
    BcAliasHostReadBarrier(scheduler,output.buffer,output.size);
    const auto tick=scheduler.CurrentTick();scheduler.Finish();scheduler.WaitPriorityOperations(tick);
    output.Invalidate();const auto* actual=static_cast<const uint32_t*>(output.data);
    Require(name,"real mixed image bits/guards/completion",scheduler.IsFree(tick)&&
        std::all_of(actual,actual+guard/4u,[](uint32_t v){return v==sentinel;})&&
        std::all_of(actual+guard/4u,actual+(bytes+guard)/4u,[&](uint32_t v){return v==patterns[index];})&&
        std::all_of(actual+(bytes+guard)/4u,actual+output.size/4u,[](uint32_t v){return v==sentinel;}),
        "native bytes/canaries/fence failed before retirement policy check");
  };
  for(size_t index=0;index<images.size();++index)native_check(index,true);
  const uint64_t cap=allocation_bytes[2];const auto text=std::to_string(cap);
  Require(name,"process-only cap",_putenv_s("KYTY_IMAGE_RECYCLE_MAX_BYTES",text.c_str())==0,
      "could not set completed-idle-cache planning cap");
  images[0].reset();images[1].reset();
  Require(name,"both small retired owners fit before large",
      NativeImageRecyclerRetainedBytesForTests(graphics.allocator,&count)==allocation_bytes[0]+allocation_bytes[1]&&count==2u,
      "both small completed objects must be genuinely present before multi-eviction");
  const auto held_handle=images[3]->backing.image;
  images[2].reset();
  const auto retained=NativeImageRecyclerRetainedBytesForTests(graphics.allocator,&count);
  const auto retained_count=count;
  native_check(3u,false);
  Require(name,"live mixed owner untouched",images[3]->backing.image==held_handle,
      "idle policy evicted a still-live native image");
  images[3].reset();NativeImageRecyclerClearForTests(graphics.allocator);
  Require(name,"idle/env cleanup",NativeImageRecyclerRetainedBytesForTests(graphics.allocator,&count)==0u&&count==0u&&
      _putenv_s("KYTY_IMAGE_RECYCLE_MAX_BYTES",previous.c_str())==0,
      "test-owned idle allocations or planning environment remain");
  std::printf("[retired-image-mixed] nativeBits=PASS guards=PASS completion=PASS live=PASS cleanup=PASS "
      "actualSizes=%llu/%llu/%llu cap=%llu retained=%llu count=%zu\n",
      static_cast<unsigned long long>(allocation_bytes[0]),static_cast<unsigned long long>(allocation_bytes[1]),
      static_cast<unsigned long long>(allocation_bytes[2]),static_cast<unsigned long long>(cap),
      static_cast<unsigned long long>(retained),retained_count);
  Require(name,"one large insertion evicts both older small entries",retained==cap&&retained_count==1u,
      "multi-victim oldest eviction did not use actual mixed native allocation bytes");
}
} // namespace
} // namespace Libs::Graphics
