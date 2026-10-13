// Include after GenericBufferCodegenTests.inc. Uses the actual production shard
// loader and a tiny finite native pipeline; no game, fake opaque machine code,
// legacy-header edits, shader-key forcing or GPU unbounded loop.
#include "graphics/host_gpu/renderer/pipeline/driverCachePolicy.h"
#include "graphics/shader/recompiler/ir/VertexExplodeLog.h"
#include "graphics/shader/recompiler/ir/VertexValueTrace.h"
#include "kytyGitVersion.h"
#include <chrono>
#include <filesystem>
#include <fstream>

namespace Libs::Graphics {
struct DriverCacheNativeTestAccess {
  static vk::PipelineCache Load(PipelineCache& cache,const std::filesystem::path& path,size_t* loaded) {
    return cache.LoadDriverCacheShard(path,loaded);
  }
};
namespace {
void CheckDriverCacheNativeRoundTrip(VulkanHarness& vulkan) {
  constexpr const char* name="DriverCacheNativeRoundTrip";
  namespace Policy=DriverCachePolicy;
  auto& graphics=vulkan.RuntimeContext();
  const auto device=vulkan.Device();
  const auto& properties=graphics.GetPhysicalDeviceProperties();
  Policy::HostIdentity host{.vendor=properties.vendorID,.device=properties.deviceID,
      .driver=properties.driverVersion,.optimization=static_cast<uint32_t>(Config::GetShaderOptimizationType())};
  std::copy_n(properties.pipelineCacheUUID.data(),host.uuid.size(),host.uuid.begin());
  std::string diagnostic;
  if(ShaderRecompiler::IR::VertexExplodeCheck().enabled)diagnostic+="vtxexplode=1\n";
  diagnostic+=ShaderRecompiler::IR::VertexTraceCacheSignature();
  const std::string old_source(64,'a');
  Require(name,"independent legacy revision setup",std::string_view(KYTY_SHADER_CACHE_KEY)!=old_source,
      "synthetic old source equals actual source; not a source-change control");
  GenericCompilerProfile(); FetchGpuGuest guest;
  GenericCompilation shader(1u,32u);
  FetchGpuBuffer input(graphics,GenericPrefixBytes+GenericInputBytes,vk::BufferUsageFlagBits::eStorageBuffer);
  FetchGpuBuffer output(graphics,32u*16u+16u,vk::BufferUsageFlagBits::eStorageBuffer);
  HW::Context registers{};HW::UserConfig user_config{};HW::Shader shaders{};
  vulkan.RuntimeRenderer().GetCommandScheduler().Begin(registers,user_config,shaders);
  const GenericNumericCase test{"OpaqueVkCacheOutput",Prospero::BufferFormat::k32UInt,
      {0x13579bdfu,0u,0u,0u},{0x13579bdfu,0x13579bdfu,0x13579bdfu,0x13579bdfu}};
  vk::PipelineCacheCreateInfo create{};vk::PipelineCache seed=nullptr;
  RequireVulkanSuccess(device.createPipelineCache(&create,nullptr,&seed),"real empty cache seed");
  vk::PipelineCreationFeedback seed_feedback{},cached_feedback{};
  std::vector<uint8_t> payload;
  {
    GenericGpuPipeline pipeline(vulkan,shader,input,output,seed,&seed_feedback);
    pipeline.Run(test,UINT32_MAX,UINT32_MAX);
    size_t size=0;
    RequireVulkanSuccess(device.getPipelineCacheData(seed,&size,nullptr),"real driver cache size");
    Require(name,"bounded actual opaque payload",size>=32u && size<=UINT32_MAX,
        "real cache returned unsupported payload size");
    payload.resize(size);
    RequireVulkanSuccess(device.getPipelineCacheData(seed,&size,payload.data()),"real driver cache bytes");
    payload.resize(size);
  }
  device.destroyPipelineCache(seed,nullptr);
  Require(name,"actual native header",Policy::PayloadHostHeaderMatches(payload,host),
      "driver bytes do not match actual device header");
  const auto directory=std::filesystem::temp_directory_path()/
      ("KytyDriverCacheNative-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Require(name,"private fixture directory",std::filesystem::create_directory(directory),
      "cannot create unique cache fixture directory");
  const std::array filenames{"legacy.bin","wrong-host.bin","corrupt.bin","wrong-native-header.bin"};
  const auto write=[&](const char* filename,const std::string& signature,
      const std::vector<uint8_t>& bytes,uint64_t checksum) {
    std::ofstream stream(directory/filename,std::ios::binary);
    stream.write(signature.data(),static_cast<std::streamsize>(signature.size()));
    stream.write(reinterpret_cast<const char*>(&checksum),sizeof(checksum));
    stream.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    stream.close();Require(name,"private envelope write",bool(stream),"fixture file write failed");
  };
  const auto checksum=XXH3_64bits(payload.data(),payload.size());
  const auto legacy_signature=Policy::JournalSignature(host,old_source)+diagnostic;
  write(filenames[0],legacy_signature,payload,checksum);
  auto wrong_host=host;wrong_host.device^=1u;
  write(filenames[1],Policy::JournalSignature(wrong_host,old_source)+diagnostic,payload,checksum);
  auto corrupt=payload;corrupt.back()^=1u;
  write(filenames[2],legacy_signature,corrupt,checksum);
  auto wrong_native=payload;wrong_native[16]^=1u;
  write(filenames[3],legacy_signature,wrong_native,XXH3_64bits(wrong_native.data(),wrong_native.size()));
  auto& cache=vulkan.RuntimeRenderer().GetPipelineCache();
  size_t loaded=0;
  const auto restored=DriverCacheNativeTestAccess::Load(cache,directory/filenames[0],&loaded);
  Require(name,"actual legacy shard load",restored!=nullptr && loaded==payload.size(),
      "production loader rejected genuine same-host opaque data after source change");
  {
    GenericGpuPipeline pipeline(vulkan,shader,input,output,restored,&cached_feedback);
    pipeline.Run(test,UINT32_MAX,UINT32_MAX);
    pipeline.Run(test,0xaaaaaaaau,0x55555555u);
  }
  device.destroyPipelineCache(restored,nullptr);
  for(size_t i=1;i<filenames.size();++i) {
    size_t rejected=UINT64_MAX;
    const auto empty=DriverCacheNativeTestAccess::Load(cache,directory/filenames[i],&rejected);
    Require(name,"actual rejected shard becomes empty",empty!=nullptr && rejected==0u,
        "wrong host/checksum/native UUID reached the driver as restored data");
    device.destroyPipelineCache(empty,nullptr);
  }
  const bool feedback_valid=static_cast<bool>(cached_feedback.flags&vk::PipelineCreationFeedbackFlagBits::eValid);
  const bool cache_hit=static_cast<bool>(cached_feedback.flags&vk::PipelineCreationFeedbackFlagBits::eApplicationPipelineCacheHit);
  std::printf("[native-cache] realPayload=%zu legacyHeader=%zu actualLoaded=%zu nativeOutputs/full+asymmetric/canaries=PASS guards=PASS feedbackValid=%u cacheHit=%u\n",
      payload.size(),legacy_signature.size(),loaded,feedback_valid,cache_hit);
  // Only task-owned named files; remove the unique directory only when empty.
  std::error_code ec;for(const auto* filename:filenames)std::filesystem::remove(directory/filename,ec);
  std::filesystem::remove(directory,ec);
  Require(name,"driver application cache reuse when feedback available",!feedback_valid || cache_hit,
      "valid driver feedback reports no application-cache reuse for the exact restored pipeline");
  if(!feedback_valid)std::printf("[native-cache] cache feedback unavailable; restored bytes/native correctness proved, hit not claimed\n");
}
} // namespace
} // namespace Libs::Graphics
