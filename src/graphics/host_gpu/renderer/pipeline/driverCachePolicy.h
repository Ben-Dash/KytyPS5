#ifndef KYTY_DRIVER_CACHE_POLICY_H_
#define KYTY_DRIVER_CACHE_POLICY_H_
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <fmt/format.h>
#include <xxhash.h>

namespace Libs::Graphics::DriverCachePolicy {
struct HostIdentity {
  uint32_t vendor=0,device=0,driver=0,optimization=0;
  std::array<uint8_t,16> uuid{};
};
inline std::string HostSuffix(const HostIdentity& host) {
  constexpr char hex[]="0123456789abcdef";
  std::string uuid(32,'0');
  for(size_t i=0;i<host.uuid.size();++i) {
    uuid[2*i]=hex[host.uuid[i]>>4u]; uuid[2*i+1]=hex[host.uuid[i]&15u];
  }
  return fmt::format("{:08x}:{:08x}:{:08x}:{}:opt={}\n",
      host.vendor,host.device,host.driver,uuid,host.optimization);
}
// Journal metadata is compiler-owned; keep the exact source/version contract.
inline std::string JournalSignature(const HostIdentity& host,std::string_view source) {
  return fmt::format("KytyPC3:{}:{}",source,HostSuffix(host));
}
// The driver matches actual SPIR-V/layout/create-info, independent of our source.
inline std::string BlobSignature(const HostIdentity& host,[[maybe_unused]] std::string_view source) {
  return "KytyPC4:"+HostSuffix(host);
}
inline size_t LegacyBlobHeaderBytes(const HostIdentity& host,std::string_view diagnostic) {
  return std::string_view("KytyPC3:").size()+64u+1u+HostSuffix(host).size()+diagnostic.size();
}
inline bool BlobHeaderCompatible(std::string_view stored,const HostIdentity& host,
    std::string_view source,std::string_view diagnostic) {
  if(stored==BlobSignature(host,source)+std::string(diagnostic))return true;
  constexpr std::string_view legacy="KytyPC3:";
  if(stored.size()!=LegacyBlobHeaderBytes(host,diagnostic) || !stored.starts_with(legacy) ||
      stored[legacy.size()+64u]!=':')return false;
  const auto compiler_token=stored.substr(legacy.size(),64u);
  if(!std::ranges::all_of(compiler_token,[](unsigned char c) {
      return (c>='0'&&c<='9') || (c>='a'&&c<='f') || (c>='A'&&c<='F');
    }))return false;
  // Accept an old wrapper only under the SAME complete host/config/diagnostic
  // tail. The legacy compiler token is not edited or used to open any journal.
  return stored.substr(legacy.size()+64u+1u)==HostSuffix(host)+std::string(diagnostic);
}
inline size_t MaxBlobHeaderBytes(const HostIdentity& host,std::string_view source,
    std::string_view diagnostic) {
  return std::max(BlobSignature(host,source).size()+diagnostic.size(),
      LegacyBlobHeaderBytes(host,diagnostic));
}
inline size_t FindBlobHeaderBytes(std::span<const char> probe,const HostIdentity& host,
    std::string_view source,std::string_view diagnostic) {
  const auto current=BlobSignature(host,source)+std::string(diagnostic);
  if(probe.size()>=current.size() &&
      BlobHeaderCompatible(std::string_view(probe.data(),current.size()),host,source,diagnostic))
    return current.size();
  const auto legacy=LegacyBlobHeaderBytes(host,diagnostic);
  return probe.size()>=legacy &&
      BlobHeaderCompatible(std::string_view(probe.data(),legacy),host,source,diagnostic)
      ? legacy:0u;
}
inline bool EnvelopeSizeValid(uint64_t size,size_t header) {
  return header!=0u && header<=UINT32_MAX-sizeof(uint64_t) &&
      size>=header+sizeof(uint64_t) && size<=UINT32_MAX;
}
inline bool PayloadChecksumMatches(std::span<const uint8_t> payload,uint64_t expected) {
  return XXH3_64bits(payload.data(),payload.size())==expected;
}
inline bool PayloadHostHeaderMatches(std::span<const uint8_t> bytes,const HostIdentity& host) {
  if(bytes.size()<32u)return false;
  const auto u32=[&](size_t offset) {
    return uint32_t{bytes[offset]} | uint32_t{bytes[offset+1]}<<8u |
        uint32_t{bytes[offset+2]}<<16u | uint32_t{bytes[offset+3]}<<24u;
  };
  return u32(0)==32u && u32(4)==1u && u32(8)==host.vendor && u32(12)==host.device &&
      std::equal(host.uuid.begin(),host.uuid.end(),bytes.begin()+16u);
}
} // namespace Libs::Graphics::DriverCachePolicy
#endif
