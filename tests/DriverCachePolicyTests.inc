// Include inside ShaderPrecompileRecordTests.cpp's unnamed namespace, with
// driverCachePolicy.h included globally. Uses existing CPU-only fmt/xxhash target.
void TestDriverOpaqueCachePolicy() {
  namespace Policy=Libs::Graphics::DriverCachePolicy;
  Policy::HostIdentity host{.vendor=0x11112222u,.device=0x33334444u,.driver=0x55556666u,.optimization=2u};
  for(size_t i=0;i<host.uuid.size();++i)host.uuid[i]=static_cast<uint8_t>(i*7u+3u);
  const std::string source_a(64,'a'),source_b(64,'b'),diagnostic="trace=isolated\n";
  const auto legacy=Policy::JournalSignature(host,source_a)+diagnostic;
  const auto current=Policy::BlobSignature(host,source_a)+diagnostic;
  Check(Policy::BlobHeaderCompatible(current,host,source_a,diagnostic),"opaque policy same-source positive");
  Check(Policy::JournalSignature(host,source_a)!=Policy::JournalSignature(host,source_b),
      "journal still rejects a changed compiler source");
  const auto legacy_expected="KytyPC3:"+source_a+":11112222:33334444:55556666:"+
      std::string("030a11181f262d343b424950575e656c:opt=2\n")+diagnostic;
  Check(legacy==legacy_expected,"exact extraction preserves old serialized signature bytes");
  for(uint32_t variation=0;variation<5;++variation) {
    auto other=host;
    if(variation==0)++other.vendor; else if(variation==1)++other.device;
    else if(variation==2)++other.driver; else if(variation==3)other.uuid[9]^=0x80u;
    else ++other.optimization;
    Check(!Policy::BlobHeaderCompatible(current,other,source_a,diagnostic),
        "opaque policy retains real vendor/device/driver/UUID/config rejection");
  }
  Check(!Policy::BlobHeaderCompatible(current,host,source_a,"other diagnostic\n"),
      "diagnostic namespace remains isolated");
  Check(!Policy::BlobHeaderCompatible(current.substr(0,current.size()-1),host,source_a,diagnostic),
      "truncated header rejected");
  std::vector<char> probe(current.begin(),current.end()); probe.resize(probe.size()+32u,'\0');
  Check(Policy::FindBlobHeaderBytes(probe,host,source_a,diagnostic)==current.size(),
      "actual serialized prefix located without consuming payload");
  Check(!Policy::EnvelopeSizeValid(current.size()+7u,current.size()) &&
      !Policy::EnvelopeSizeValid(uint64_t{UINT32_MAX}+1u,current.size()) &&
      Policy::EnvelopeSizeValid(current.size()+8u+48u,current.size()),
      "original envelope length/minimum/uint32 bound retained");
  std::vector<uint8_t> payload(48u,0xcdu);
  const auto put32=[&](size_t at,uint32_t value) {
    for(size_t i=0;i<4u;++i)payload[at+i]=static_cast<uint8_t>(value>>(8u*i));
  };
  put32(0,32u); put32(4,1u); put32(8,host.vendor); put32(12,host.device);
  std::copy(host.uuid.begin(),host.uuid.end(),payload.begin()+16u);
  const auto checksum=XXH3_64bits(payload.data(),payload.size());
  Check(Policy::PayloadHostHeaderMatches(payload,host) && Policy::PayloadChecksumMatches(payload,checksum),
      "independent little-endian Vulkan VERSION_ONE header and checksum controls");
  auto corrupt=payload; corrupt.back()^=1u;
  Check(!Policy::PayloadChecksumMatches(corrupt,checksum),"opaque payload corruption rejected");
  corrupt=payload; corrupt[16]^=1u;
  Check(!Policy::PayloadHostHeaderMatches(corrupt,host),"native payload UUID mismatch rejected");
  corrupt=payload; corrupt[0]=31u;
  Check(!Policy::PayloadHostHeaderMatches(corrupt,host),"native payload header size rejected");
  corrupt=payload; corrupt[4]=2u;
  Check(!Policy::PayloadHostHeaderMatches(corrupt,host),"unsupported native payload header version rejected");
  std::printf("[policy] journal strict source; host/config/diagnostic/header/checksum controls passed\n");
  // Intended RED on the exact old factory, after all supported/error controls:
  // an opaque host blob must survive a compiler-only source change.
  Check(Policy::BlobHeaderCompatible(legacy,host,source_b,diagnostic),
      "compiler-only source change unnecessarily rejects a host-compatible opaque Vulkan cache");
  Check(Policy::BlobSignature(host,source_a)==Policy::BlobSignature(host,source_b),
      "new opaque header still depends on compiler source");
  std::vector<char> legacy_probe(legacy.begin(),legacy.end());legacy_probe.resize(legacy_probe.size()+16u,'\0');
  Check(Policy::FindBlobHeaderBytes(legacy_probe,host,source_b,diagnostic)==legacy.size(),
      "legacy header compatibility lost payload offset");
  auto malformed=legacy;malformed[8]='g';
  Check(!Policy::BlobHeaderCompatible(malformed,host,source_b,diagnostic),
      "malformed legacy source token is not a compatible envelope");
  std::printf("[policy] opaque host blob survives source change; legacy payload offset preserved\n");
}
