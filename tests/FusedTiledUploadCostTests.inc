// Include after BcAliasOwnershipTests.inc / EmbeddedFetchGpuTests.inc.
// Real public cache upload, completed native readback, independent CPU tiled data.
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/commandHooks.h"
#include <utility>

namespace Libs::Graphics {
namespace {
void ConfigureFusedTiledUploadTests() {
  Require("FusedTiledUpload", "baseline recording profile setup",
      _putenv_s("KYTY_RECORD_THREAD", "0") == 0,
      "pin direct recording before first getter; setup failure is not RED");
}

// Test-only observer of actual native calls. Delegates bytes/handles unchanged.
// No new production method, fake upload plan or replacement Vulkan implementation.
class FusedUploadCopyAudit {
public:
  FusedUploadCopyAudit() {
    Require("FusedTiledUpload", "scoped actual dispatcher setup",
        upstream == nullptr && VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdCopyBufferToImage != nullptr,
        "nested/missing native copy dispatcher is not the intended cost RED");
    upstream = VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdCopyBufferToImage;
    upstream2 = VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdCopyBufferToImage2;
    upstream2khr = VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdCopyBufferToImage2KHR;
    local = this;
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdCopyBufferToImage = Observe;
    if(upstream2 != nullptr)
      VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdCopyBufferToImage2 = Observe2;
    if(upstream2khr != nullptr)
      VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdCopyBufferToImage2KHR = Observe2KHR;
  }
  ~FusedUploadCopyAudit() {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdCopyBufferToImage = upstream;
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdCopyBufferToImage2 = upstream2;
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdCopyBufferToImage2KHR = upstream2khr;
    local = nullptr;
    upstream = nullptr;
    upstream2 = nullptr;
    upstream2khr = nullptr;
  }
  FusedUploadCopyAudit(const FusedUploadCopyAudit&) = delete;
  FusedUploadCopyAudit& operator=(const FusedUploadCopyAudit&) = delete;
  std::vector<VkImage> targets;
private:
  inline static PFN_vkCmdCopyBufferToImage upstream = nullptr;
  inline static PFN_vkCmdCopyBufferToImage2 upstream2 = nullptr;
  inline static PFN_vkCmdCopyBufferToImage2KHR upstream2khr = nullptr;
  inline static thread_local FusedUploadCopyAudit* local = nullptr;
  static void VKAPI_PTR Observe(VkCommandBuffer command, VkBuffer source,
      VkImage target, VkImageLayout layout, uint32_t count,
      const VkBufferImageCopy* regions) {
    if(local != nullptr) local->targets.push_back(target);
    upstream(command, source, target, layout, count, regions);
  }
  static void VKAPI_PTR Observe2(VkCommandBuffer command,const VkCopyBufferToImageInfo2* info) {
    if(local != nullptr) local->targets.push_back(info->dstImage);
    upstream2(command,info);
  }
  static void VKAPI_PTR Observe2KHR(VkCommandBuffer command,const VkCopyBufferToImageInfo2KHR* info) {
    if(local != nullptr) local->targets.push_back(info->dstImage);
    upstream2khr(command,info);
  }
};

struct FusedUploadObserved {
  uint32_t bytes_per_pixel = 0;
  uint32_t intermediate_copies = 0;
  double gpu_ns = 0;
};

FusedUploadObserved RunFusedTiledUploadCase(VulkanHarness& vulkan, uint32_t bpe,
                                         vk::QueryPool timestamps = nullptr) {
  constexpr const char* name = "FusedTiledUpload";
  constexpr uint32_t width = 128, height = 128;
  constexpr uint64_t base = 0x00000002c0900000ull, allocation_bytes = 0x40000;
  constexpr uint64_t source_address = base + 0x10000, guard = 256;
  constexpr uint32_t sentinel = 0x5a5a5a5a;
  Require(name, "bounded real GPU worker setup", GuestGpu::IsGpuThread() && (bpe==4 || bpe==8),
      "use the existing GuestGpu SendCommandSync wrapper; no missing-worker workaround");
  Require(name,"actual tiler descriptor capability setup",
      vulkan.RuntimeContext().max_push_descriptors>=3,
      "instantiate existing VulkanHarness with require_push_descriptors=true before the RED");
  Require(name,"standalone direct recording setup",!CommandRecordingDeferred(),
      "global test dispatcher observer requires standalone direct recording; setup failure");
  const auto guest_format = bpe==4 ? Prospero::BufferFormat::k8_8_8_8UNorm :
                                    Prospero::BufferFormat::k16_16_16_16Float;
  const auto native_format = bpe==4 ? vk::Format::eR8G8B8A8Unorm :
                                     vk::Format::eR16G16B16A16Sfloat;
  const auto raw_format = bpe==4 ? vk::Format::eR32Uint :
                                  vk::Format::eR16G16B16A16Uint;
  constexpr auto tile_mode = Prospero::TileMode::kRenderTarget;
  TileSizeAlign total{};
  TileGetTextureTotalSize(guest_format,width,height,1,1,tile_mode,false,total);
  const auto upload_layout = TextureCalcUploadLayout(guest_format,width,height,1,1,
      tile_mode,total.size,false,name);
  const auto& block = upload_layout.surface.texture.block;
  Require(name, "actual non-tail render-target layout setup", total.size>0 &&
      block.family==TileBlockFamily::RenderTarget64KB && block.bytes_per_element==bpe &&
      upload_layout.surface.first_tail_level==1 &&
      source_address+total.size+guard<=base+allocation_bytes,
      "fixture must be a complete bounded non-tail4/8byte render-target upload");

  const uint64_t linear_bytes = uint64_t{width}*height*bpe;
  std::vector<uint32_t> expected(static_cast<size_t>(linear_bytes/4));
  constexpr std::array<uint16_t,12> halves{0x0000,0x8000,0x0001,0x03ff,0x3c00,0x7c00,
                                         0xfc00,0x7e01,0x7d55,0x3555,0xbc00,0xffff};
  for(uint32_t pixel=0;pixel<width*height;++pixel) {
    if(bpe==4) expected[pixel]=0x93a60000u^(pixel*0x01010103u);
    else for(uint32_t word=0;word<2;++word)
      expected[pixel*2+word]=uint32_t{halves[(pixel*3+word*2)%halves.size()]} |
          (uint32_t{halves[(pixel*3+word*2+1)%halves.size()]}<<16);
  }
  std::vector<uint8_t> tiled(total.size,0x5a);
  const uint64_t columns = (upload_layout.surface.mips[0].padded_width+block.block_width-1)/block.block_width;
  for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x) {
    const uint32_t bx=x/block.block_width, by=y/block.block_height;
    uint32_t local=0, block_xor=0;
    Require(name, "independent CPU tile address setup",
        TileGetBlockOffset(block,x%block.block_width,y%block.block_height,0,local) &&
        TileGetBlockXor(block,bx,by,0,block_xor),
        "CPU ISA layout rejected fixture address; not the copy-cost RED");
    const uint64_t offset=upload_layout.surface.mips[0].offset+
        (by*columns+bx)*block.block_size+(local^block_xor);
    Require(name,"independent source bounds setup",offset+bpe<=tiled.size(),
        "CPU tiled source escaped real allocation; not a cost RED");
    std::memcpy(tiled.data()+offset,expected.data()+(uint64_t{y}*width+x)*(bpe/4),bpe);
  }

  int64_t direct=-1;
  Require(name,"tracked guest allocation setup",LibKernel::Memory::KernelAllocateDirectMemory(
      0,LibKernel::Memory::KernelGetDirectMemorySize(),allocation_bytes,0x10000,0,&direct)==0,
      "actual synthetic direct allocation failed; not RED");
  void* mapped=reinterpret_cast<void*>(base);
  Require(name,"tracked guest mapping setup",LibKernel::Memory::KernelMapDirectMemory(
      &mapped,allocation_bytes,0x3,0x10,direct,0x10000)==0 && mapped==reinterpret_cast<void*>(base),
      "task-owned native guest mapping failed; not RED");
  std::memset(mapped,0x5a,allocation_bytes);
  std::memcpy(reinterpret_cast<void*>(source_address),tiled.data(),tiled.size());

  auto& graphics=vulkan.RuntimeContext();
  auto& context=vulkan.RuntimeRenderer();
  auto& scheduler=context.GetCommandScheduler();
  auto& cache=context.GetTextureCache();
  HW::Context registers{}; HW::UserConfig user_config{}; HW::Shader shaders{};
  scheduler.Begin(registers,user_config,shaders);
  LibKernel::Memory::InstallGpuResources(&context);
  context.MapMemory(base,allocation_bytes);
  TextureCache::ImageDesc desc{};
  desc.type=TextureCache::BindingType::Texture;
  desc.info.data={source_address,total.size};
  desc.info.pixel_format=native_format; desc.info.guest_format=guest_format;
  desc.info.extent={width,height,1}; desc.info.resources={1,1};
  desc.info.pitch=upload_layout.pitch; desc.info.bytes_per_block=bpe;
  desc.info.samples=1; desc.info.tile_mode=tile_mode;
  const auto& mip=upload_layout.surface.mips[0];
  desc.info.mip_layout[0]={mip.offset,mip.size,mip.padded_width,mip.padded_height};
  desc.view_info.format=native_format; desc.view_info.type=vk::ImageViewType::e2D;
  desc.view_info.aspect=vk::ImageAspectFlagBits::eColor;
  desc.view_info.usage=vk::ImageUsageFlagBits::eSampled;
  desc.view_info.level_count=desc.view_info.layer_count=1;
  Require(name,"identity color bytes setup",desc.info.GetColorTransform()==ColorTransform::None,
      "fixture must not bypass a required guest-to-host transform");

  ImageId id{};
  std::vector<VkImage> uploaded_targets;
  if(timestamps != nullptr) {
    scheduler.EndRendering();
    const auto command=scheduler.Current().Handle();
    command.resetQueryPool(timestamps,0,2);
    command.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe,timestamps,0);
  }
  {
    FusedUploadCopyAudit audit;
    id=cache.FindImage(desc);
    Require(name,"actual public cache upload setup",id && cache.FindTexture(id,desc)!=nullptr,
        "real cache upload/view failed; not the intended RED");
    uploaded_targets=audit.targets;
  }
  if(timestamps != nullptr)
    scheduler.Current().Handle().writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,timestamps,1);
  auto& image=cache.GetImage(id);
  Require(name,"actual view feature setup",ImageViewOps::FormatsCompatible(image.backing.format,raw_format) &&
      bool(graphics.GetFormatProperties(raw_format).optimalTilingFeatures&vk::FormatFeatureFlagBits::eStorageImage) &&
      bool(image.backing.flags&vk::ImageCreateFlagBits::eMutableFormat) &&
      bool(image.backing.usage&vk::ImageUsageFlagBits::eStorage),
      "raw storage view is unsupported; setup failure is not cost RED");
  auto raw_view=desc.view_info; raw_view.format=raw_format;
  raw_view.usage=vk::ImageUsageFlagBits::eStorage;
  Require(name,"legal real raw view setup",image.FindView(raw_view)!=nullptr,
      "raw integer image view creation failed before the cost oracle");
  Require(name,"copy observer source identity setup",std::all_of(uploaded_targets.begin(),uploaded_targets.end(),
      [&](VkImage target){return target==static_cast<VkImage>(image.backing.image);}),
      "observer captured an unrelated image; not a valid copy-cost reproduction");

  FetchGpuBuffer readback(graphics,linear_bytes+2*guard,vk::BufferUsageFlagBits::eTransferDst);
  std::fill_n(static_cast<uint32_t*>(readback.data),readback.size/4,sentinel); readback.Flush();
  vk::BufferImageCopy copy{}; copy.bufferOffset=guard;
  copy.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1}; copy.imageExtent={width,height,1};
  image.Download(std::span{&copy,size_t{1}},readback.buffer,0,readback.size,readback.size);
  BcAliasHostReadBarrier(scheduler,readback.buffer,readback.size);
  scheduler.Finish(); readback.Invalidate();
  const auto* actual=reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(readback.data)+guard);
  Require(name,"all completed native image bits",std::equal(expected.begin(),expected.end(),actual),
      "native bytes differ from independent tiled-source golden before intended cost RED");
  const auto* all=static_cast<const uint32_t*>(readback.data);
  Require(name,"native readback prefix and tail canaries",
      std::all_of(all,all+guard/4,[](uint32_t word){return word==sentinel;}) &&
      std::all_of(actual+expected.size(),actual+expected.size()+guard/4,
                  [](uint32_t word){return word==sentinel;}),
      "native copy escaped the exact output region before cost RED");
  std::vector<uint8_t> source_after(total.size+2*guard);
  Require(name,"source bytes and guest guards unchanged",
      LibKernel::Memory::TryReadBacking(source_address-guard,source_after.data(),source_after.size()) &&
      std::equal(tiled.begin(),tiled.end(),source_after.begin()+guard) &&
      std::all_of(source_after.begin(),source_after.begin()+guard,[](uint8_t b){return b==0x5a;}) &&
      std::all_of(source_after.end()-guard,source_after.end(),[](uint8_t b){return b==0x5a;}),
      "upload or cache publication changed input or neighboring guest bytes");
  scheduler.Finish();
  double gpu_ns=0;
  if(timestamps != nullptr) {
    const auto families=graphics.physical_device.getQueueFamilyProperties();
    Require(name,"actual query capability setup",graphics.queue_family<families.size() &&
        families[graphics.queue_family].timestampValidBits>0,
        "completed real upload has no queue timestamp capability; setup failure");
    const auto valid_bits=families[graphics.queue_family].timestampValidBits;
    const auto mask=valid_bits>=64 ? UINT64_MAX : (uint64_t{1}<<valid_bits)-1;
    std::array<uint64_t,2> ticks{};
    RequireVulkanSuccess(vulkan.Device().getQueryPoolResults(timestamps,0,2,sizeof(ticks),ticks.data(),
        sizeof(uint64_t),vk::QueryResultFlagBits::e64),"completed upload GPU timestamps without WAIT");
    gpu_ns=static_cast<double>((ticks[1]-ticks[0])&mask)*
        graphics.physical_device_properties.limits.timestampPeriod;
    Require(name,"single bounded upload GPU time",gpu_ns>0 && gpu_ns<25000000.0,
        "real single upload reached25ms safety bound; do not grow or reset GPU");
  }
  context.UnmapMemory(base,allocation_bytes);
  LibKernel::Memory::InstallGpuResources(nullptr);
  Require(name,"task-owned mapping cleanup",LibKernel::Memory::KernelMunmap(base,allocation_bytes)==0 &&
      LibKernel::Memory::KernelReleaseDirectMemory(direct,allocation_bytes)==0,
      "fixture guest allocation was not released");
  std::printf("[fused-upload-cost] bpe=%u128x128 all_bits=PASS guards=PASS raw_view=PASS copyBufferToImage=%zu\n",
      bpe,uploaded_targets.size());
  return {bpe,static_cast<uint32_t>(uploaded_targets.size()),gpu_ns};
}

void CheckFusedTiledUploadCost(VulkanHarness& vulkan) {
  const std::array observations{RunFusedTiledUploadCase(vulkan,4),RunFusedTiledUploadCase(vulkan,8)};
  // Both real native fixtures, exact bits, guards and cleanup pass BEFORE intended cost RED.
  for(const auto& result:observations)
    Require("FusedTiledUpload","no full intermediate linear-to-image transfer",
        result.intermediate_copies==0,
        "eligible tiled upload still records the avoidable full copyBufferToImage stage");
}
} // namespace
} // namespace Libs::Graphics
