// Include after FusedTiledUploadCostTests.inc. SAME executable and GPU resources for both routes.
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "common/alignment.h"
#include "kernel/memory.h"
namespace Libs::Graphics {
namespace {
// Test-only real guest metadata owner. Declare before native buffers/image so
// its actual mapping outlives every completed GPU resource and both routes.
class FusedUpload1080GuestMapping {
public:
  static constexpr uint64_t Base = 0x00000002c1800000ull;
  FusedUpload1080GuestMapping(RenderContext& context, uint64_t requested)
      : context(context) {
    Require("FusedUpload1080Pair", "bounded real guest mapping size", requested != 0 &&
        requested <= 32ull * 1024u * 1024u + 516u,
        "fixture request exceeds established complete tiled-source bound; setup failure, not RED");
    bytes = Common::AlignUp<uint64_t>(requested, 0x10000u);
    Require("FusedUpload1080Pair", "real guest direct allocation",
        LibKernel::Memory::KernelAllocateDirectMemory(0,
            LibKernel::Memory::KernelGetDirectMemorySize(), bytes, 0x10000u, 0, &direct) == 0,
        "bounded synthetic direct allocation failed; not RED");
    void* mapped = reinterpret_cast<void*>(Base);
    Require("FusedUpload1080Pair", "real guest direct mapping",
        LibKernel::Memory::KernelMapDirectMemory(&mapped, bytes, 0x3, 0x10, direct, 0x10000u) == 0 &&
            mapped == reinterpret_cast<void*>(Base),
        "fixed synthetic guest mapping failed; no VMA pointer or fabricated guest range fallback");
    std::memset(mapped, 0x5a, static_cast<size_t>(bytes));
    LibKernel::Memory::InstallGpuResources(&context);
    context.MapMemory(Base, bytes);
    Require("FusedUpload1080Pair", "actual guest range registry",
        GuestRange{Base, requested}.Valid() && context.IsMapped(Base, bytes),
        "actual guest metadata range is not wholly mapped");
  }
  ~FusedUpload1080GuestMapping() {
    context.UnmapMemory(Base, bytes);
    Require("FusedUpload1080Pair", "guest mapping retirement",
        !context.IsMapped(Base, bytes) && LibKernel::Memory::KernelMunmap(Base, bytes) == 0 &&
            LibKernel::Memory::KernelReleaseDirectMemory(direct, bytes) == 0,
        "task-owned actual mapping/direct allocation was not released");
    LibKernel::Memory::InstallGpuResources(nullptr);
  }
  FusedUpload1080GuestMapping(const FusedUpload1080GuestMapping&) = delete;
  FusedUpload1080GuestMapping& operator=(const FusedUpload1080GuestMapping&) = delete;
  RenderContext& context;
  uint64_t bytes = 0;
  int64_t direct = -1;
};

void RunFusedUpload1080Pair(VulkanHarness& vulkan,uint32_t bpe) {
  constexpr const char* name="FusedUpload1080Pair";
  constexpr uint32_t width=1920,height=1080;
  constexpr uint64_t prefix=260,guard=256;
  constexpr uint32_t sentinel=0xdeadbeefu;
  auto& graphics=vulkan.RuntimeContext();auto& scheduler=vulkan.RuntimeRenderer().GetCommandScheduler();
  Require(name,"real GPU worker and direct recorder setup",GuestGpu::IsGpuThread() &&
      !CommandRecordingDeferred() && graphics.max_push_descriptors>=3 && (bpe==4 || bpe==8),
      "run standalone on actual GPU worker/direct recorder with the existing push-capability harness");
  HW::Context registers{};HW::UserConfig user_config{};HW::Shader shaders{};
  scheduler.Begin(registers,user_config,shaders);
  const auto format=bpe==4 ? Prospero::BufferFormat::k8_8_8_8UNorm : Prospero::BufferFormat::k16_16_16_16Float;
  const auto native_format=bpe==4 ? vk::Format::eR8G8B8A8Unorm : vk::Format::eR16G16B16A16Sfloat;
  constexpr auto tile_mode=Prospero::TileMode::kRenderTarget;
  TileSizeAlign total{};TileGetTextureTotalSize(format,width,height,1,1,tile_mode,false,total);
  const auto layout=TextureCalcUploadLayout(format,width,height,1,1,tile_mode,total.size,false,name);
  const auto regions=TextureBuildImageCopies(layout);
  std::vector<GpuTileInfo> infos;
  Require(name,"bounded complete1080 layout setup",total.size>0 && total.size<=32u*1024u*1024u &&
      regions.size()==1 && TextureBuildGpuTileInfos(total.size,regions,layout,1,infos) && infos.size()==1 &&
      !infos[0].tail && infos[0].width==width && infos[0].height==height &&
      infos[0].family==TileBlockFamily::RenderTarget64KB,
      "existing public tile layout did not represent one complete1080color subresource");

  // One host seed and one actual device-local input are reused by BOTH routes.
  FusedUpload1080GuestMapping guest_mapping(vulkan.RuntimeRenderer(), prefix+total.size+guard);
  Buffer host_source(graphics,scheduler,MemoryUsage::Upload,0,AllFlags,prefix+total.size+guard);
  auto mapped=host_source.Mapped();
  Require(name,"real host seed mapping setup",mapped.size()>=host_source.Size(),"upload seed mapping is incomplete");
  std::fill(mapped.begin(),mapped.end(),uint8_t{0x5a});
  const auto cpu_address=FusedUpload1080GuestMapping::Base;
  Buffer device_source(graphics,scheduler,MemoryUsage::DeviceLocal,cpu_address,AllFlags,host_source.Size());
  Require(name,"actual device-local source setup",
      (device_source.MemoryProperties()&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)!=0,
      "device-local allocation unavailable; host-visible input cannot stand in for this throughput screen");
  const uint64_t live_bytes=uint64_t{width}*height*bpe;
  std::vector<uint32_t> expected(static_cast<size_t>(live_bytes/4));
  constexpr std::array<uint16_t,12> halves{0x0000,0x8000,0x0001,0x03ff,0x3c00,0x7c00,
                                         0xfc00,0x7e01,0x7d55,0x3555,0xbc00,0xffff};
  const auto& block=layout.surface.texture.block;
  const uint64_t columns=(layout.surface.mips[0].padded_width+block.block_width-1)/block.block_width;
  bool source_valid=true;
  for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x) {
    const uint32_t pixel=y*width+x;
    if(bpe==4)expected[pixel]=0x93a60000u^(pixel*0x01010103u);
    else for(uint32_t word=0;word<2;++word)
      expected[pixel*2+word]=uint32_t{halves[(pixel*3+word*2)%halves.size()]} |
          (uint32_t{halves[(pixel*3+word*2+1)%halves.size()]}<<16);
    uint32_t local=0,xor_value=0;
    const uint32_t bx=x/block.block_width,by=y/block.block_height;
    const bool valid=TileGetBlockOffset(block,x%block.block_width,y%block.block_height,0,local) &&
        TileGetBlockXor(block,bx,by,0,xor_value);
    const uint64_t offset=layout.surface.mips[0].offset+
        (by*columns+bx)*block.block_size+(local^xor_value);
    source_valid&=valid && offset+bpe<=total.size;
    if(valid && offset+bpe<=total.size)
      std::memcpy(mapped.data()+prefix+offset,expected.data()+uint64_t{pixel}*(bpe/4),bpe);
  }
  Require(name,"independent full source addresses setup",source_valid,"CPU source tile address escaped complete allocation");
  // Same seed/guards in genuine guest backing; preparation stays outside GPU timing.
  std::memcpy(reinterpret_cast<void*>(cpu_address), mapped.data(), static_cast<size_t>(host_source.Size()));
  host_source.Flush(0,host_source.Size());
  device_source.CopyFrom(scheduler.Current(),host_source,0,0,host_source.Size(),
                        vk::AccessFlagBits::eHostWrite,vk::AccessFlagBits::eMemoryWrite);
  scheduler.Finish();

  ImageInfo image_info{};image_info.data={cpu_address+prefix,total.size};
  image_info.pixel_format=native_format;image_info.guest_format=format;
  image_info.extent={width,height,1};image_info.resources={1,1};image_info.pitch=layout.pitch;
  image_info.bytes_per_block=bpe;image_info.samples=1;image_info.tile_mode=tile_mode;
  const auto& mip=layout.surface.mips[0];image_info.mip_layout[0]={mip.offset,mip.size,mip.padded_width,mip.padded_height};
  Libs::Graphics::Image image(graphics,scheduler,image_info);
  StreamBuffer parameters(graphics,scheduler,MemoryUsage::Upload,1u<<20);
  TileManager tiler(graphics,scheduler,parameters);
  FetchGpuBuffer output(graphics,guard+live_bytes+guard,vk::BufferUsageFlagBits::eTransferDst);
  const auto families=graphics.physical_device.getQueueFamilyProperties();
  Require(name,"actual timestamp capability setup",graphics.queue_family<families.size() &&
      families[graphics.queue_family].timestampValidBits>0 && graphics.physical_device_properties.limits.timestampPeriod>0,
      "selected queue timestamps unavailable; not a passing measurement");
  const auto valid_bits=families[graphics.queue_family].timestampValidBits;
  const auto mask=valid_bits>=64 ? UINT64_MAX : (uint64_t{1}<<valid_bits)-1;
  vk::QueryPoolCreateInfo create{};create.queryType=vk::QueryType::eTimestamp;create.queryCount=2;
  vk::QueryPool timestamps=nullptr;
  RequireVulkanSuccess(vulkan.Device().createQueryPool(&create,nullptr,&timestamps),"1080upload query pool");
  const auto Sample=[&](bool fused,uint32_t sample) {
    // Finish a real zero clear first, so no old correct image can disguise an omitted upload
    // and the previous clear is not counted as part of either route's GPU interval.
    auto command=scheduler.Current().Handle();
    image.Transit(vk::ImageLayout::eTransferDstOptimal,vk::AccessFlagBits2::eTransferWrite,{},command);
    vk::ClearColorValue zero{};
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor,0,1,0,1};
    command.clearColorImage(image.backing.image,vk::ImageLayout::eTransferDstOptimal,&zero,1,&range);
    scheduler.Finish();
    command=scheduler.Current().Handle();command.resetQueryPool(timestamps,0,2);
    command.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe,timestamps,0);
    uint32_t actual_copies=0;
    TileManager::ScratchScope held(tiler);
    {
      FusedUploadCopyAudit audit;
      if(fused) {
        Require(name,"actual complete raw upload route",
          tiler.TryUploadImage(image,{device_source.Handle(),prefix,total.size,device_source.Size()},
              layout.mips[0].size,infos,regions,ColorTransform::None),
          "fused1080metadata/capability route unsupported; not a timing result");
      } else {
        const auto linear=tiler.Detile(device_source.Handle(),prefix,total.size,layout.mips[0].size,infos,ColorTransform::None);
        auto legacy_regions=regions;for(auto& copy:legacy_regions)copy.bufferOffset+=linear.offset;
        image.Upload(legacy_regions,linear.buffer,linear.offset,linear.size,linear.capacity);
      }
      actual_copies=static_cast<uint32_t>(audit.targets.size());
      Require(name,"same actual target identity",std::all_of(audit.targets.begin(),audit.targets.end(),
        [&](VkImage target){return target==static_cast<VkImage>(image.backing.image);}),
        "timing observer saw unrelated required rendering work");
    }
    scheduler.Current().Handle().writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,timestamps,1);
    std::fill_n(static_cast<uint32_t*>(output.data),output.size/4,sentinel);output.Flush();
    vk::BufferImageCopy full{};full.bufferOffset=guard;
    full.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1};full.imageExtent={width,height,1};
    image.Download(std::span{&full,size_t{1}},output.buffer,0,output.size,output.size);
    BcAliasHostReadBarrier(scheduler,output.buffer,output.size);scheduler.Finish();output.Invalidate();
    const auto* words=static_cast<const uint32_t*>(output.data);
    Require(name,"all exact1080native image bits",std::equal(expected.begin(),expected.end(),words+guard/4),
        "legacy/fused1080image disagrees with independent raw per-pixel golden");
    Require(name,"native1080output guards",std::all_of(words,words+guard/4,[](uint32_t v){return v==sentinel;}) &&
      std::all_of(words+guard/4+expected.size(),words+output.size/4,[](uint32_t v){return v==sentinel;}),
        "1080upload escaped actual output region");
    Require(name,"actual full intermediate copy count",actual_copies==(fused?0u:1u),
        "sameEXEroute did not perform the expected actual upload stages");
    std::array<uint64_t,2> ticks{};
    RequireVulkanSuccess(vulkan.Device().getQueryPoolResults(timestamps,0,2,sizeof(ticks),ticks.data(),
        sizeof(uint64_t),vk::QueryResultFlagBits::e64),"completed1080timestamps without WAIT");
    const double ns=static_cast<double>((ticks[1]-ticks[0])&mask)*graphics.physical_device_properties.limits.timestampPeriod;
    Require(name,"single1080operation safety bound",ns>0 && ns<25000000.0,
        "single actual1080upload reached25ms; stop rather than increase work or reset GPU");
    std::printf("[upload1080-sample] bpe=%u route=%s sample=%u gpu_ns=%.1f live_words=%zu actualCopies=%u allbits/guards=PASS\n",
      bpe,fused?"fused":"legacy",sample,ns,expected.size(),actual_copies);
    return ns;
  };
  // One calibration each then one additional warm;7interleaved paired samples.
  Sample(false,0);Sample(true,0);Sample(false,1);Sample(true,1);
  std::array<std::vector<double>,2> measured;
  for(uint32_t sample=0;sample<7;++sample) {
    measured[0].push_back(Sample(false,sample+2));
    measured[1].push_back(Sample(true,sample+2));
  }
  for(uint32_t route=0;route<2;++route)std::sort(measured[route].begin(),measured[route].end());
  std::printf("[upload1080-pair] bpe=%u1920x1080 legacy_median_ns=%.1f fused_median_ns=%.1f legacy_min/max=%.1f/%.1f fused_min/max=%.1f/%.1f reduction_pct=%.2f sameEXE/sameDeviceSource/sameTarget/allbits=PASS\n",
    bpe,measured[0][3],measured[1][3],measured[0].front(),measured[0].back(),measured[1].front(),measured[1].back(),
    (1.0-measured[1][3]/measured[0][3])*100.0);
  // Independent completed GPU source snapshot, including both untouched guards.
  FetchGpuBuffer source_check(graphics,host_source.Size(),vk::BufferUsageFlagBits::eTransferDst);
  const vk::BufferCopy copy{0,0,host_source.Size()};
  scheduler.Current().Handle().copyBuffer(device_source.Handle(),source_check.buffer,1,&copy);
  BcAliasHostReadBarrier(scheduler,source_check.buffer,source_check.size);scheduler.Finish();source_check.Invalidate();
  Require(name,"entire device source and guards unchanged",std::memcmp(source_check.data,mapped.data(),source_check.size)==0,
      "upload routes changed actual device-local source or its protected surrounding bytes");
  std::vector<uint8_t> guest_after(static_cast<size_t>(host_source.Size()));
  Require(name,"entire real guest source and guards unchanged",
      LibKernel::Memory::TryReadBacking(cpu_address,guest_after.data(),guest_after.size()) &&
          std::memcmp(guest_after.data(),mapped.data(),guest_after.size())==0,
      "upload routes changed guest metadata backing or source guards");
  vulkan.Device().destroyQueryPool(timestamps,nullptr);
}

void CheckFusedTiledUpload1080Bench(VulkanHarness& vulkan) {
  RunFusedUpload1080Pair(vulkan,4);RunFusedUpload1080Pair(vulkan,8);
}
} // namespace
} // namespace Libs::Graphics
