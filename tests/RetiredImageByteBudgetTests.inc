namespace Libs::Graphics {
namespace {
void CheckRetiredImageByteBudget(VulkanHarness& vulkan) {
  constexpr const char* name="RetiredImageByteBudget";
  constexpr uint32_t width=64,height=64,word=0xe3a95317u,sentinel=0xdeadbeefu;
  constexpr uint64_t bytes=uint64_t{width}*height*4u,guard=256u;
  auto& graphics=vulkan.RuntimeContext();auto& scheduler=vulkan.RuntimeRenderer().GetCommandScheduler();
  Require(name,"standalone real GPU worker",GuestGpu::IsGpuThread()&&!CommandRecordingDeferred(),
      "real recording worker required; configuration failure is not a byte-budget RED");
  size_t count=0;
  Require(name,"fresh native recycler setup",NativeImageRecyclerRetainedBytesForTests(graphics.allocator,&count)==0u&&count==0u,
      "standalone fixture must start with an empty native recycler");
  HW::Context registers{};HW::UserConfig user_config{};HW::Shader shaders{};
  scheduler.Begin(registers,user_config,shaders);
  ImageInfo info{};info.pixel_format=vk::Format::eR8G8B8A8Uint;
  info.guest_format=Prospero::BufferFormat::k8_8_8_8UInt;info.type=Prospero::ImageType::kColor2D;
  info.extent={width,height,1};info.resources={1,1};info.pitch=width;info.bytes_per_block=4;
  info.samples=1;info.tile_mode=Prospero::TileMode::kLinear;
  std::array<std::unique_ptr<Image>,6> images;
  for(auto& image:images)image=std::make_unique<Image>(graphics,scheduler,info);
  VmaAllocationInfo allocation{};vmaGetAllocationInfo(graphics.allocator,images[0]->backing.allocation,&allocation);
  const uint64_t one=allocation.size;
  Require(name,"bounded actual native allocations",one!=0u&&one<4u*1024u*1024u&&
      std::ranges::all_of(images,[&](const auto& image){
        VmaAllocationInfo other{};vmaGetAllocationInfo(graphics.allocator,image->backing.allocation,&other);
        return image->backing.recyclable&&other.size==one;
      }),"fixture must own6 tiny equal real reusable allocations, not a huge VRAM stress request");
  const uint64_t limit=2u*one;
  const char* previous_raw=std::getenv("KYTY_IMAGE_RECYCLE_MAX_BYTES");
  const std::string previous=previous_raw!=nullptr?previous_raw:"";
  const auto requested=std::to_string(limit);
  Require(name,"process-only byte planning input",_putenv_s("KYTY_IMAGE_RECYCLE_MAX_BYTES",requested.c_str())==0,
      "set a bound for retired unused cache bytes only; native device capacity is untouched");
  FetchGpuBuffer seed(graphics,bytes,vk::BufferUsageFlagBits::eTransferSrc);
  std::fill_n(static_cast<uint32_t*>(seed.data),bytes/4u,word);seed.Flush();
  FetchGpuBuffer output(graphics,images.size()*(bytes+2u*guard),vk::BufferUsageFlagBits::eTransferDst);
  std::fill_n(static_cast<uint32_t*>(output.data),output.size/4u,sentinel);output.Flush();
  vk::BufferImageCopy copy{};copy.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1};
  copy.imageExtent={width,height,1};
  for(size_t index=0;index<images.size();++index) {
    copy.bufferOffset=0;
    images[index]->Upload(std::span{&copy,size_t{1}},seed.buffer,0,bytes,bytes);
    copy.bufferOffset=index*(bytes+2u*guard)+guard;
    images[index]->Download(std::span{&copy,size_t{1}},output.buffer,0,output.size,output.size);
  }
  BcAliasHostReadBarrier(scheduler,output.buffer,output.size);
  const auto used_tick=scheduler.CurrentTick();scheduler.Finish();scheduler.WaitPriorityOperations(used_tick);
  output.Invalidate();
  const auto* actual=static_cast<const uint32_t*>(output.data);
  bool bits=true,guards=true;
  for(size_t index=0;index<images.size();++index) {
    const auto base=index*(bytes+2u*guard)/4u;
    guards&=std::all_of(actual+base,actual+base+guard/4u,[](uint32_t value){return value==sentinel;})&&
        std::all_of(actual+base+(bytes+guard)/4u,actual+base+(bytes+2u*guard)/4u,
                    [](uint32_t value){return value==sentinel;});
    bits&=std::all_of(actual+base+guard/4u,actual+base+(bytes+guard)/4u,
                     [](uint32_t value){return value==word;});
  }
  Require(name,"completed real uploads/readbacks before retirement",bits&&guards&&scheduler.IsFree(used_tick),
      "real current bytes/canaries/native tick failed before byte-budget cost");
  const auto held_handle=images.back()->backing.image;
  for(size_t index=0;index+1u<images.size();++index)images[index].reset();
  const uint64_t retained=NativeImageRecyclerRetainedBytesForTests(graphics.allocator,&count);
  const size_t retained_count=count;
  FetchGpuBuffer held_output(graphics,bytes+2u*guard,vk::BufferUsageFlagBits::eTransferDst);
  std::fill_n(static_cast<uint32_t*>(held_output.data),held_output.size/4u,sentinel);held_output.Flush();
  copy.bufferOffset=guard;
  images.back()->Download(std::span{&copy,size_t{1}},held_output.buffer,0,held_output.size,held_output.size);
  BcAliasHostReadBarrier(scheduler,held_output.buffer,held_output.size);
  const auto held_tick=scheduler.CurrentTick();scheduler.Finish();scheduler.WaitPriorityOperations(held_tick);
  held_output.Invalidate();const auto* held=static_cast<const uint32_t*>(held_output.data);
  const bool held_ok=images.back()->backing.image==held_handle&&scheduler.IsFree(held_tick)&&
      std::all_of(held,held+guard/4u,[](uint32_t value){return value==sentinel;})&&
      std::all_of(held+guard/4u,held+(bytes+guard)/4u,[](uint32_t value){return value==word;})&&
      std::all_of(held+(bytes+guard)/4u,held+held_output.size/4u,[](uint32_t value){return value==sentinel;});
  Require(name,"live image preserved under retired-pool budget",held_ok,
      "a live image was evicted, lost contents or lost native retirement ordering");
  images.back().reset();
  NativeImageRecyclerClearForTests(graphics.allocator); // Only the already-retired idle pool.
  const uint64_t after=NativeImageRecyclerRetainedBytesForTests(graphics.allocator,&count);
  Require(name,"actual idle allocation/env cleanup",after==0u&&count==0u&&
      _putenv_s("KYTY_IMAGE_RECYCLE_MAX_BYTES",previous.c_str())==0,
      "test-owned idle allocations or process-only planning option remained");
  std::printf("[retired-image-budget] realNativeBits=PASS guards=PASS ticks=PASS live=PASS cleanup=PASS "
      "oneAllocation=%llu cap=%llu retiredBytes=%llu retiredCount=%zu\n",
      static_cast<unsigned long long>(one),static_cast<unsigned long long>(limit),
      static_cast<unsigned long long>(retained),retained_count);
  Require(name,"retired byte budget",retained<=limit,
      "actual unused native image cache exceeds its planned byte budget despite all live/fence controls");
}
} // namespace
} // namespace Libs::Graphics
