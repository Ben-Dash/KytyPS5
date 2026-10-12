// UNCOMPILED TESTONLY proposal. Include after BcAliasOwnershipTests.inc.
// Use main's existing BC GuestGpu lifecycle wrapper and VulkanHarness(true).
// ConfigureBcPartialBufferWriteTests() before the first config getter/harness.
// Minimal: CheckBcPartialBufferWriteGpu(vulkan, false): BC7 ordinary CPU-A case.
// Neighbors: CheckBcPartialBufferWriteGpu(vulkan, true): BC4/5/6HU/7,
// each with and without a prior actual GPU image C write.
// Configure(...,true) is an EXPERIMENTAL flag1 comparison, never correctness proof.
namespace Libs::Graphics {
namespace {

struct BcPartialBufferObservation {
  std::string name;
  bool prior_image = false, initial_a = false, prior_c = true;
  bool buffer_b = false, cpu_a = false, guards = false, source_lacks_base = false;
  std::vector<uint32_t> actual, expected;
  uint32_t tail_level = 0;
  uint64_t write_address = 0, array_bytes = 0;
};

void ConfigureBcPartialBufferWriteTests(bool experimental_flag = false) {
  Require("BcPartialBufferWrite", "pin original/diagnostic profile",
      _putenv_s("KYTY_RECORD_THREAD", "0") == 0 &&
      _putenv_s("KYTY_BC_BUFFER_WRITES", experimental_flag ? "1" : "0") == 0,
      "environment pin failed before first getter; not a semantic RED");
}

BcPartialBufferObservation RunBcPartialBufferWriteCase(VulkanHarness& vulkan,
                                                       uint32_t format_index,
                                                       bool prior_image) {
  constexpr std::array formats{vk::Format::eBc4UnormBlock, vk::Format::eBc5UnormBlock,
      vk::Format::eBc6HUfloatBlock, vk::Format::eBc7UnormBlock};
  constexpr std::array guest_formats{Prospero::BufferFormat::kBc4UNorm,
      Prospero::BufferFormat::kBc5UNorm, Prospero::BufferFormat::kBc6UFloat,
      Prospero::BufferFormat::kBc7UNorm};
  constexpr std::array labels{"Bc4PartialBufferWrite", "Bc5PartialBufferWrite",
      "Bc6HPartialBufferWrite", "Bc7PartialBufferWrite"};
  constexpr uint32_t width = 128u, levels = 8u, layers = 3u;
  constexpr uint64_t base = 0x00000002c0600000ull, bytes = 0x40000u;
  constexpr uint64_t address = base + 0x10000u;
  constexpr uint32_t b_word = 0x13579bdfu, c_word = 0x2468ace0u;
  const uint32_t block_bytes = format_index == 0u ? 8u : 16u;
  BcPartialBufferObservation observed;
  observed.name = labels[format_index]; observed.prior_image = prior_image;
  const char* name = observed.name.c_str();
  auto array = BcAliasDesc(name,address,formats[format_index],guest_formats[format_index],
      width,width,levels,layers,block_bytes,TextureCache::BindingType::Texture);
  const auto layout = TextureCalcUploadLayout(array.info.guest_format,width,width,levels,layers,
      array.info.tile_mode,array.info.data.size,false,name);
  const auto stride = array.info.data.size/layers;
  observed.array_bytes = array.info.data.size;
  observed.tail_level = layout.surface.first_tail_level;
  Require(name,"bounded strided array",observed.tail_level>0u && observed.tail_level+1u<levels &&
      array.info.data.size<=0x20000u && address+array.info.data.size+16u<=base+bytes &&
      stride==layout.surface.block_slice_size,"three-layer/eight-mip array must stay within128KiB");
  const auto& tail = layout.surface.mips[observed.tail_level];
  const uint64_t tail_address = address+2u*stride+tail.offset;
  uint32_t block_offset = 0u;
  Require(name,"independent packed-tail address",TileGetBlockOffset(layout.surface.texture.block,
      tail.tail_x,tail.tail_y,0u,block_offset) && block_offset<=4096u-block_bytes &&
      tail.size==4096u,"first tail mip's first compressed block needs an in-range4KiB address");
  observed.write_address = tail_address+block_offset;
  Require(name,"strict nonbase interior write",observed.write_address>address &&
      observed.write_address+block_bytes<=array.info.data.End(),"write must be inside another array layer");
  Require(name,"actual GPU command worker",GuestGpu::IsGpuThread(),
          "invoke through the existing main BC GuestGpu wrapper; no missing-GPU workaround");

  int64_t direct = -1;
  Require(name,"guest allocation",LibKernel::Memory::KernelAllocateDirectMemory(
      0,LibKernel::Memory::KernelGetDirectMemorySize(),bytes,0x10000u,0,&direct)==0,
      "direct allocation failed; not a partial-buffer RED");
  void* mapped = reinterpret_cast<void*>(base);
  Require(name,"guest mapping",LibKernel::Memory::KernelMapDirectMemory(&mapped,bytes,0x3,0x10,
      direct,0x10000u)==0 && mapped==reinterpret_cast<void*>(base),"direct mapping failed");
  std::memset(mapped,0x5a,bytes);
  std::memset(reinterpret_cast<void*>(address),0xff,array.info.data.size);
  auto& context = vulkan.RuntimeRenderer(); auto& scheduler = context.GetCommandScheduler();
  auto& textures = context.GetTextureCache(); auto& buffers = context.GetBufferCache();
  HW::Context registers{}; HW::UserConfig user_config{}; HW::Shader shaders{};
  scheduler.Begin(registers,user_config,shaders);
  LibKernel::Memory::InstallGpuResources(&context); context.MapMemory(base,bytes);

  std::vector<vk::BufferImageCopy> copies;
  for(uint32_t layer=0;layer<layers;++layer) for(uint32_t level=0;level<levels;++level) {
    const uint32_t side = std::max(width>>level,1u);
    const uint32_t blocks = ((side+3u)/4u)*((side+3u)/4u);
    vk::BufferImageCopy copy{};
    copy.bufferOffset = observed.expected.size()*sizeof(uint32_t);
    copy.imageSubresource = {vk::ImageAspectFlagBits::eColor,level,layer,1u};
    copy.imageExtent = {side,side,1u}; copies.push_back(copy);
    observed.expected.insert(observed.expected.end(),blocks*block_bytes/sizeof(uint32_t),UINT32_MAX);
  }
  const auto read_image = [&](ImageId id) {
    const auto read_bytes = observed.expected.size()*sizeof(uint32_t);
    FetchGpuBuffer download(vulkan.RuntimeContext(),read_bytes,vk::BufferUsageFlagBits::eTransferDst);
    textures.GetImage(id).Download(copies,download.buffer,0u,read_bytes,read_bytes);
    BcAliasHostReadBarrier(scheduler,download.buffer,read_bytes); scheduler.Finish(); download.Invalidate();
    std::vector<uint32_t> actual(observed.expected.size());
    std::memcpy(actual.data(),download.data,read_bytes); return actual;
  };
  const auto read_buffer = [&](Buffer& source,uint64_t source_address,uint64_t read_bytes) {
    FetchGpuBuffer download(vulkan.RuntimeContext(),read_bytes,vk::BufferUsageFlagBits::eTransferDst);
    scheduler.EndRendering(); const auto command = scheduler.Current().Handle();
    vk::BufferMemoryBarrier2 before{};
    before.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands;
    before.srcAccessMask = vk::AccessFlagBits2::eMemoryWrite;
    before.dstStageMask = vk::PipelineStageFlagBits2::eCopy;
    before.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
    before.srcQueueFamilyIndex = before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    before.buffer = source.Handle(); before.offset = source.Offset(source_address); before.size = read_bytes;
    vk::DependencyInfo dependency{}; dependency.bufferMemoryBarrierCount=1u;
    dependency.pBufferMemoryBarriers=&before; command.pipelineBarrier2(dependency);
    const vk::BufferCopy copy{source.Offset(source_address),0u,read_bytes};
    command.copyBuffer(source.Handle(),download.buffer,1u,&copy);
    BcAliasHostReadBarrier(scheduler,download.buffer,read_bytes); scheduler.Finish(); download.Invalidate();
    std::vector<uint32_t> actual(read_bytes/sizeof(uint32_t));
    std::memcpy(actual.data(),download.data,read_bytes); return actual;
  };
  const auto consumer = textures.FindImage(array);
  Require(name,"initial real BC array",textures.FindTexture(consumer,array)!=nullptr &&
      textures.GetImage(consumer).backing.format==array.info.pixel_format &&
      textures.GetImage(consumer).backing.layers==layers &&
      textures.GetImage(consumer).backing.mip_levels==levels,"native array setup failed");
  observed.initial_a = read_image(consumer)==observed.expected;

  if(prior_image) {
    // Native BC storage cannot be cleared/rendered. A legal compressed-block
    // transfer makes C genuinely GPU-owned without modifying its CPU backing.
    FetchGpuBuffer upload(vulkan.RuntimeContext(),block_bytes,vk::BufferUsageFlagBits::eTransferSrc);
    std::fill_n(static_cast<uint32_t*>(upload.data),block_bytes/sizeof(uint32_t),c_word); upload.Flush();
    vk::BufferImageCopy overwrite{};
    overwrite.imageSubresource={vk::ImageAspectFlagBits::eColor,0u,0u,1u};
    overwrite.imageExtent={4u,4u,1u};
    textures.GetImage(consumer).Upload(std::span{&overwrite,size_t{1}},upload.buffer,0u,block_bytes,block_bytes);
    textures.MarkGpuWritten(consumer); scheduler.Finish();
    std::fill_n(observed.expected.begin(),block_bytes/sizeof(uint32_t),c_word);
    observed.prior_c=read_image(consumer)==observed.expected;
  }

  // Do NOT pre-create a full allocation owner: that would let ordinary image
  // publication hide the partial-owner boundary being tested. The tail writer
  // ordinarily occupies a different16KiB page from the old BC image base.
  const auto [writer,writer_offset] = buffers.ObtainBuffer(tail_address,4096u,true);
  Require(name,"native partial writable owner",writer!=nullptr,"partial owner acquisition failed");
  observed.source_lacks_base = !writer->IsInBounds(address,1u);
  writer->Fill(writer_offset+block_offset,block_bytes,b_word); buffers.OnCommandRecorded();
  textures.InvalidateMemoryFromGPU(observed.write_address,block_bytes,TextureCache::GpuWriteSource::Fill);
  const auto actual_tail = read_buffer(*writer,tail_address,4096u);
  std::vector<uint32_t> expected_tail(4096u/sizeof(uint32_t),UINT32_MAX);
  std::fill_n(expected_tail.begin()+block_offset/sizeof(uint32_t),block_bytes/sizeof(uint32_t),b_word);
  observed.buffer_b=actual_tail==expected_tail;
  const auto b_at = copies[2u*levels+observed.tail_level].bufferOffset/sizeof(uint32_t);
  std::fill_n(observed.expected.begin()+b_at,block_bytes/sizeof(uint32_t),b_word);
  auto again = textures.FindImage(array);
  Require(name,"consumer native reacquisition",textures.FindTexture(again,array)!=nullptr,
          "native BC consumer reacquisition failed");
  observed.actual=read_image(again);
  // Inspect actual backing aliases without fault-triggered CPU publication.
  std::vector<uint32_t> cpu(array.info.data.size/sizeof(uint32_t));
  observed.cpu_a=LibKernel::Memory::TryReadBacking(address,cpu.data(),array.info.data.size) &&
      std::ranges::all_of(cpu,[](uint32_t value){return value==UINT32_MAX;});
  std::array<uint32_t,8> guard{};
  observed.guards=LibKernel::Memory::TryReadBacking(address-16u,guard.data(),16u) &&
      LibKernel::Memory::TryReadBacking(array.info.data.End(),guard.data()+4u,16u) &&
      std::ranges::all_of(guard,[](uint32_t value){return value==0x5a5a5a5au;});
  scheduler.Finish(); context.UnmapMemory(base,bytes); LibKernel::Memory::InstallGpuResources(nullptr);
  Require(name,"owned guest cleanup",LibKernel::Memory::KernelMunmap(base,bytes)==0 &&
      LibKernel::Memory::KernelReleaseDirectMemory(direct,bytes)==0,"guest cleanup failed");
  return observed;
}

void CheckBcPartialBufferWriteGpu(VulkanHarness& vulkan,bool neighbors=false) {
  const uint32_t first=neighbors?0u:3u;
  for(uint32_t format=first;format<4u;++format) {
    const uint32_t count=neighbors?2u:1u;
    for(uint32_t prior=0;prior<count;++prior) {
      const auto observed=RunBcPartialBufferWriteCase(vulkan,format,prior!=0u);
      uint64_t mismatches=0u,first_bad=UINT64_MAX;
      for(size_t i=0;i<observed.expected.size();++i) if(observed.actual[i]!=observed.expected[i]) {
        if(first_bad==UINT64_MAX) first_bad=i; ++mismatches;
      }
      std::printf("[gpu] %s priorImageC=%u initialA=%u priorC=%u bufferB=%u cpuA=%u guards=%u "
          "writerLacksBase=%u tail=%u address=%llx arrayBytes=%llu words=%zu mismatches=%llu "
          "firstBad=%llu actual=%08x expected=%08x flag=%s\n",observed.name.c_str(),observed.prior_image,
          observed.initial_a,observed.prior_c,observed.buffer_b,observed.cpu_a,observed.guards,
          observed.source_lacks_base,observed.tail_level,static_cast<unsigned long long>(observed.write_address),
          static_cast<unsigned long long>(observed.array_bytes),observed.expected.size(),
          static_cast<unsigned long long>(mismatches),static_cast<unsigned long long>(first_bad),
          first_bad==UINT64_MAX?0u:observed.actual[first_bad],first_bad==UINT64_MAX?0u:observed.expected[first_bad],
          KYTY_GETENV("KYTY_BC_BUFFER_WRITES"));
      Require(observed.name.c_str(),"actual producer/untouched controls",observed.initial_a &&
          observed.prior_c && observed.buffer_b && observed.cpu_a && observed.guards,
          "setup/producer readback/CPU staleness failed; not the intended BC buffer ownership RED");
      // The safe fix may expand the writer to preserve image C before B. That
      // is allowed; source_lacks_base is baseline evidence, not a GREEN oracle.
      Require(observed.name.c_str(),"complete BC consumer sees partial buffer write",observed.actual==observed.expected,
          "BC consumer lost interior buffer B or erased older GPU image C/untouched mip/layer data");
    }
  }
}

} // namespace
} // namespace Libs::Graphics
