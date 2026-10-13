// Ignored actual loader regression, include AFTER DriverCacheNativeRoundTripTests.inc.
// Real EMPTY Vulkan cache payload; no shader dispatch, fake native bytes, or retagged inputs.
#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"
namespace Libs::Graphics { namespace {
void ConfigureFormatLookupCacheAdmissionNative() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  Require("FormatLookupCacheAdmission","profile setup",_putenv_s("KYTY_FORMAT_LOOKUP_SWITCH","1")==0,"profile setup failed; not RED");
#else
  Require("FormatLookupCacheAdmission","profile setup",setenv("KYTY_FORMAT_LOOKUP_SWITCH","1",1)==0,"profile setup failed; not RED");
#endif
}
struct FormatLookupCacheAdmissionFixture {
  VulkanHarness& vulkan;
  DriverCachePolicy::HostIdentity host{};
  std::filesystem::path directory;
  std::vector<uint8_t> payload;
  std::string off_signature,on_signature;
  std::vector<std::string> files;
  explicit FormatLookupCacheAdmissionFixture(VulkanHarness& value):vulkan(value) {
    constexpr const char* name="FormatLookupCacheAdmission";
    const auto& props=vulkan.RuntimeContext().GetPhysicalDeviceProperties();
    host={.vendor=props.vendorID,.device=props.deviceID,.driver=props.driverVersion,
      .optimization=static_cast<uint32_t>(Config::GetShaderOptimizationType())};
    std::copy_n(props.pipelineCacheUUID.data(),host.uuid.size(),host.uuid.begin());
    std::string diagnostic;
    if(ShaderRecompiler::IR::VertexExplodeCheck().enabled)diagnostic+="vtxexplode=1\n";
    diagnostic+=ShaderRecompiler::IR::VertexTraceCacheSignature();
    off_signature=DriverCachePolicy::BlobSignature(host,KYTY_SHADER_CACHE_KEY)+diagnostic;
    on_signature=off_signature+"fmtLookupShared1=1\n";
    vk::PipelineCacheCreateInfo info{};vk::PipelineCache seed=nullptr;
    RequireVulkanSuccess(vulkan.Device().createPipelineCache(&info,nullptr,&seed),"real empty native cache");
    size_t size=0u;
    RequireVulkanSuccess(vulkan.Device().getPipelineCacheData(seed,&size,nullptr),"empty native cache payload size");
    Require(name,"bounded native payload",size>=32u&&size<=1024u*1024u,"unsupported EMPTY native cache payload extent; not RED");
    payload.resize(size);
    RequireVulkanSuccess(vulkan.Device().getPipelineCacheData(seed,&size,payload.data()),"empty native cache payload bytes");
    payload.resize(size);vulkan.Device().destroyPipelineCache(seed,nullptr);
    Require(name,"actual native host header",DriverCachePolicy::PayloadHostHeaderMatches(payload,host),"actual empty payload host header mismatch; not RED");
    directory=std::filesystem::temp_directory_path()/
      ("KytyFormatCacheAdmission-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Require(name,"unique owned directory",std::filesystem::create_directory(directory),"fixture directory creation failed; not RED");
  }
  void Write(const char* filename,const std::string& signature,const std::vector<uint8_t>& data,uint64_t checksum) {
    files.emplace_back(filename);
    std::ofstream f(directory/filename,std::ios::binary);
    f.write(signature.data(),static_cast<std::streamsize>(signature.size()));
    f.write(reinterpret_cast<const char*>(&checksum),sizeof(checksum));
    f.write(reinterpret_cast<const char*>(data.data()),static_cast<std::streamsize>(data.size()));
    f.close();Require("FormatLookupCacheAdmission","owned envelope write",bool(f),"fixture file write failed; not RED");
  }
  std::vector<char> Read(const char* filename) const {
    std::ifstream f(directory/filename,std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(f),{});
  }
  size_t Load(const char* filename) {
    size_t loaded=0u;
    const auto cache=DriverCacheNativeTestAccess::Load(vulkan.RuntimeRenderer().GetPipelineCache(),directory/filename,&loaded);
    Require("FormatLookupCacheAdmission","actual cache object",cache!=nullptr,"actual loader cache creation failed; not the policy RED");
    vulkan.Device().destroyPipelineCache(cache,nullptr);return loaded;
  }
  void Cleanup() {
    for(const auto& filename:files) {
      std::error_code ec;std::filesystem::remove(directory/filename,ec);
      Require("FormatLookupCacheAdmission","owned file cleanup",!ec,"owned fixture file cleanup failed; not RED");
    }
    std::error_code ec;std::filesystem::remove(directory,ec);
    Require("FormatLookupCacheAdmission","owned directory cleanup",!ec,"owned fixture directory cleanup failed; not RED");
  }
};
void CheckFormatLookupCacheExistingOnIsolation(VulkanHarness& vulkan) {
  constexpr const char* name="FormatLookupCacheExistingOnIsolation";
  bool off_correct=false,checksum_rejected=false,host_rejected=false,sources_unchanged=false;
  size_t wrong_variant_loaded=0u,bytes=0u;
  {
    FormatLookupCacheAdmissionFixture f(vulkan);bytes=f.payload.size();
    const auto checksum=XXH3_64bits(f.payload.data(),f.payload.size());
    f.Write("ordinary.bin",f.off_signature,f.payload,checksum);
    f.Write("existing.fmtLookupShared1.bin",f.off_signature,f.payload,checksum);
    auto bad=f.payload;bad.back()^=1u;
    f.Write("bad-checksum.bin",f.off_signature,bad,checksum);
    bad=f.payload;bad[16]^=1u;
    f.Write("bad-native-host.bin",f.off_signature,bad,XXH3_64bits(bad.data(),bad.size()));
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
    Require(name,"OFF control setup",_putenv_s("KYTY_FORMAT_LOOKUP_SWITCH","0")==0,"profile setup failed; not RED");
#else
    setenv("KYTY_FORMAT_LOOKUP_SWITCH","0",1);
#endif
    off_correct=f.Load("ordinary.bin")==bytes;
    checksum_rejected=f.Load("bad-checksum.bin")==0u;
    host_rejected=f.Load("bad-native-host.bin")==0u;
    const auto ordinary_before=f.Read("ordinary.bin"),on_before=f.Read("existing.fmtLookupShared1.bin");
    ConfigureFormatLookupCacheAdmissionNative();
    Require(name,"actual ON compiler flag",ShaderRecompiler::Spirv::DynamicFormatLookupSwitchEnabled(),"compiler opt-in not enabled; not RED");
    wrong_variant_loaded=f.Load("existing.fmtLookupShared1.bin");
    sources_unchanged=ordinary_before==f.Read("ordinary.bin")&&on_before==f.Read("existing.fmtLookupShared1.bin");
    f.Cleanup();
  }
  Require(name,"OFF native payload and boundary controls",off_correct&&checksum_rejected&&host_rejected&&sources_unchanged,
      "OFF/checksum/native-host/source-preservation controls failed; not the intended variant RED");
  std::printf("[cache] existingON offExact1 checksumRejected1 hostRejected1 sourceUnchanged1 cleanup1 bytes=%zu wrongVariantLoaded=%zu expected0\n",bytes,wrong_variant_loaded);
  Require(name,"existing ON namespace admission",wrong_variant_loaded==0u,
      "existing ON-path wrongly accepts OFF signature instead of requiring fmtLookupShared1 identity");
}
void CheckFormatLookupCacheMissingOnWarmNeighbor(VulkanHarness& vulkan) {
  constexpr const char* name="FormatLookupCacheMissingOnWarmNeighbor";
  bool source_unchanged=false,candidate_absent=false,bad_rejected=false;
  size_t loaded=0u,bytes=0u;
  {
    FormatLookupCacheAdmissionFixture f(vulkan);bytes=f.payload.size();
    const auto checksum=XXH3_64bits(f.payload.data(),f.payload.size());
    f.Write("warm.bin",f.off_signature,f.payload,checksum);
    auto bad=f.payload;bad.back()^=1u;f.Write("bad.bin",f.off_signature,bad,checksum);
    const auto before=f.Read("warm.bin");
    ConfigureFormatLookupCacheAdmissionNative();
    Require(name,"ON compiler flag",ShaderRecompiler::Spirv::DynamicFormatLookupSwitchEnabled(),"compiler opt-in not enabled; not a native neighbor");
    loaded=f.Load("warm.fmtLookupShared1.bin");
    bad_rejected=f.Load("bad.fmtLookupShared1.bin")==0u;
    source_unchanged=before==f.Read("warm.bin");
    candidate_absent=!std::filesystem::exists(f.directory/"warm.fmtLookupShared1.bin")&&
                     !std::filesystem::exists(f.directory/"bad.fmtLookupShared1.bin");
    f.Cleanup();
  }
  Require(name,"validated opaque seed preservation",bad_rejected&&source_unchanged&&candidate_absent,
      "corrupt seed accepted/input changed/read-only loader wrote files");
  std::printf("[cache] missingON actualOpaqueLoaded=%zu expected=%zu checksumRejected1 sourceUnchanged1 noRetag1 cleanup1\n",loaded,bytes);
  Require(name,"unchanged real opaque warming",loaded==bytes,"missing ON cache did not reuse host-compatible actual opaque payload");
}
} } // namespace Libs::Graphics
