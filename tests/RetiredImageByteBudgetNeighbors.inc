namespace Libs::Graphics {
namespace {
void CheckRetiredImageByteBudgetNeighbors(VulkanHarness& vulkan) {
  constexpr const char* name="RetiredImageByteBudgetNeighbors";
  constexpr uint32_t width=64,height=64,sentinel=0xdeadbeefu;
  constexpr uint64_t bytes=uint64_t{width}*height*4u,guard=256u;
  auto& graphics=vulkan.RuntimeContext();auto& scheduler=vulkan.RuntimeRenderer().GetCommandScheduler();
  Require(name,"real bounded worker",GuestGpu::IsGpuThread()&&!CommandRecordingDeferred(),
      "native standalone direct worker required before policy controls");
  size_t count=0;
  Require(name,"empty initial idle pool",NativeImageRecyclerRetainedBytesForTests(graphics.allocator,&count)==0u&&count==0u,
      "a nonempty foreign pool is setup failure");
  const char* previous_raw=std::getenv("KYTY_IMAGE_RECYCLE_MAX_BYTES");
  const std::string previous=previous_raw!=nullptr?previous_raw:"";
  auto set_cap=[&](uint64_t value) {
    const auto text=std::to_string(value);
    Require(name,"process-only retired byte cap",_putenv_s("KYTY_IMAGE_RECYCLE_MAX_BYTES",text.c_str())==0,
        "process environment setup failed");
  };
  HW::Context registers{};HW::UserConfig user_config{};HW::Shader shaders{};
  scheduler.Begin(registers,user_config,shaders);
  ImageInfo info{};info.pixel_format=vk::Format::eR8G8B8A8Uint;
  info.guest_format=Prospero::BufferFormat::k8_8_8_8UInt;info.type=Prospero::ImageType::kColor2D;
  info.extent={width,height,1};info.resources={1,1};info.pitch=width;info.bytes_per_block=4;
  info.samples=1;info.tile_mode=Prospero::TileMode::kLinear;
  auto roundtrip=[&](Image& image,uint32_t word) {
    FetchGpuBuffer source(graphics,bytes,vk::BufferUsageFlagBits::eTransferSrc);
    std::fill_n(static_cast<uint32_t*>(source.data),bytes/4u,word);source.Flush();
    FetchGpuBuffer output(graphics,bytes+2u*guard,vk::BufferUsageFlagBits::eTransferDst);
    std::fill_n(static_cast<uint32_t*>(output.data),output.size/4u,sentinel);output.Flush();
    vk::BufferImageCopy copy{};copy.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1};
    copy.imageExtent={width,height,1};
    image.Upload(std::span{&copy,size_t{1}},source.buffer,0,bytes,bytes);
    copy.bufferOffset=guard;
    image.Download(std::span{&copy,size_t{1}},output.buffer,0,output.size,output.size);
    BcAliasHostReadBarrier(scheduler,output.buffer,output.size);
    const auto tick=scheduler.CurrentTick();scheduler.Finish();scheduler.WaitPriorityOperations(tick);
    output.Invalidate();const auto* actual=static_cast<const uint32_t*>(output.data);
    Require(name,"native exact bytes/canaries before retirement",scheduler.IsFree(tick)&&
        std::all_of(actual,actual+guard/4u,[](uint32_t v){return v==sentinel;})&&
        std::all_of(actual+guard/4u,actual+(bytes+guard)/4u,[&](uint32_t v){return v==word;})&&
        std::all_of(actual+(bytes+guard)/4u,actual+output.size/4u,[](uint32_t v){return v==sentinel;}),
        "real native image bytes/guard/readiness failed before a policy assertion");
  };
  auto image=std::make_unique<Image>(graphics,scheduler,info);
  VmaAllocationInfo allocation{};vmaGetAllocationInfo(graphics.allocator,image->backing.allocation,&allocation);
  const uint64_t one=allocation.size;
  Require(name,"tiny actual allocation",image->backing.recyclable&&one>1u&&one<4u*1024u*1024u,
      "fixture must remain a small actual allocation");
  roundtrip(*image,0x91ab37e3u);
  set_cap(one-1u);image.reset();
  Require(name,"single oversize retirement bypasses cache",
      NativeImageRecyclerRetainedBytesForTests(graphics.allocator,&count)==0u&&count==0u,
      "an allocation larger than the cap remained cached");
  set_cap(2u*one);image=std::make_unique<Image>(graphics,scheduler,info);
  roundtrip(*image,0x11223344u);const auto recycled_handle=image->backing.image;image.reset();
  Require(name,"actual retired allocation eligible for reuse",
      NativeImageRecyclerRetainedBytesForTests(graphics.allocator,&count)==one&&count==1u,
      "tiny completed allocation did not enter the idle pool");
  image=std::make_unique<Image>(graphics,scheduler,info);
  Require(name,"same-info reuse consumes retained bytes",image->backing.image==recycled_handle&&
      NativeImageRecyclerRetainedBytesForTests(graphics.allocator,&count)==0u&&count==0u,
      "reuse retained an idle byte charge or failed exact-info matching");
  roundtrip(*image,0xa95371e3u);image.reset();
  auto other_info=info;other_info.pixel_format=vk::Format::eR8G8B8A8Unorm;
  other_info.guest_format=Prospero::BufferFormat::k8_8_8_8UNorm;
  auto other=std::make_unique<Image>(graphics,scheduler,other_info);
  Require(name,"mismatched format does not borrow cached handle",other->backing.image!=recycled_handle,
      "different native format reused a UINT native allocation without a compatible creation contract");
  roundtrip(*other,0x5971a3e9u);other.reset();
  image=std::make_unique<Image>(graphics,scheduler,info);roundtrip(*image,0x37c195a3u);
  set_cap(0u);image.reset();
  Require(name,"zero cap trims all already-retired allocations",
      NativeImageRecyclerRetainedBytesForTests(graphics.allocator,&count)==0u&&count==0u,
      "a lowered zero cap left idle allocations retained");
  set_cap(64u*one);
  std::array<std::unique_ptr<Image>,26> tiny;
  for(auto& current:tiny)current=std::make_unique<Image>(graphics,scheduler,info);
  // These fresh images have no recorded GPU uses. The count-bound test does not
  // fake completion of an in-flight owner; native-use lifetime was checked above.
  for(auto& current:tiny)current.reset();
  Require(name,"existing count24 stays bounded",
      NativeImageRecyclerRetainedBytesForTests(graphics.allocator,&count)==24u*one&&count==24u,
      "the new byte cap increased or broke the existing image count bound");
  NativeImageRecyclerClearForTests(graphics.allocator);
  for(const char* invalid:{"-1","1x","18446744073709551616"}) {
    Require(name,"invalid planning input stimulus",_putenv_s("KYTY_IMAGE_RECYCLE_MAX_BYTES",invalid)==0,
        "invalid environment stimulus could not be installed");
    image=std::make_unique<Image>(graphics,scheduler,info);roundtrip(*image,0x7139c5a3u);image.reset();
    Require(name,"invalid/overflowed decimal input keeps default",
        NativeImageRecyclerRetainedBytesForTests(graphics.allocator,&count)==one&&count==1u,
        "invalid planning input accidentally disabled retention or changed native image semantics");
    NativeImageRecyclerClearForTests(graphics.allocator);
  }
  Require(name,"owned idle/env cleanup",NativeImageRecyclerRetainedBytesForTests(graphics.allocator,&count)==0u&&count==0u&&
      _putenv_s("KYTY_IMAGE_RECYCLE_MAX_BYTES",previous.c_str())==0,
      "native idle allocations or process-only planning input remained");
  std::printf("[retired-image-neighbors] nativeBits=PASS guards=PASS ticks=PASS oversize=PASS reuse=PASS "
      "formatMismatch=PASS zeroCap=PASS count24=PASS invalidParse=PASS cleanup=PASS oneAllocation=%llu\n",
      static_cast<unsigned long long>(one));
}
} // namespace
} // namespace Libs::Graphics
