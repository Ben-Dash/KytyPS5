// Include globally AFTER GenericBufferCodegenTests.inc in BindlessLifecycleTests.cpp.
// Existing real compiler fixture validates SPIR-V and preserves all distinct guest reads/stores.
#include <map>
#include <tuple>
#include <unordered_map>

namespace Libs::Graphics {
namespace {
struct StaticBufferLengthCost {
  std::map<std::tuple<uint32_t,uint32_t,uint32_t>, uint32_t> by_descriptor;
  uint32_t queries = 0, checked_lengths = 0;
};

StaticBufferLengthCost CountStaticBufferLengthQueries(const std::vector<uint32_t>& words) {
  constexpr const char* name = "ArrayLengthDescriptorCost";
  struct Pointer {uint32_t base=0, index=0; bool static_index=false;};
  std::unordered_map<uint32_t,uint32_t> constants;
  std::unordered_map<uint32_t,Pointer> pointers;
  std::unordered_map<uint32_t,std::tuple<uint32_t,uint32_t,uint32_t>> lengths;
  std::set<uint32_t> checked;
  StaticBufferLengthCost cost;
  for (size_t p=5;p<words.size();) {
    const auto count=words[p]>>16;
    Require(name,"actual module framing setup",count>0 && count<=words.size()-p,
            "invalid SPIR-V module; not an ArrayLength cost RED");
    const auto op=static_cast<spv::Op>(words[p]&0xffffu);
    if(op==spv::OpConstant && count==4) constants[words[p+2]]=words[p+3];
    if(op==spv::OpAccessChain && count==5) {
      const auto found=constants.find(words[p+4]);
      pointers[words[p+2]]={words[p+3],found==constants.end()?0u:found->second,
                          found!=constants.end()};
    }
    if(op==spv::OpArrayLength) {
      Require(name,"SSBO length framing setup",count==5,
              "unexpected ArrayLength framing; not a cost RED");
      const auto pointer=pointers.find(words[p+3]);
      Require(name,"actual static descriptor selection setup",pointer!=pointers.end() &&
                pointer->second.static_index && words[p+4]==0,
              "fixture requires the real constant-slot buffer structure, not a dynamic heap");
      const auto key=std::tuple{pointer->second.base,pointer->second.index,words[p+4]};
      ++cost.by_descriptor[key]; ++cost.queries;
      lengths[words[p+2]]=key;
    }
    if(op==spv::OpULessThan && count==5 && lengths.contains(words[p+4]))
      checked.insert(words[p+4]);
    p+=count;
  }
  Require(name,"real bound resource identities setup",cost.by_descriptor.size()==2,
          "synthetic guest must retain distinct real input/output descriptor slots");
  Require(name,"native length remains consumed by bounds checks setup",
          checked.size()==lengths.size() && !checked.empty(),
          "length queries disappeared from actual bounds comparisons; not a cost RED");
  cost.checked_lengths=static_cast<uint32_t>(checked.size());
  return cost;
}

void CheckArrayLengthDescriptorCost(VulkanHarness&) {
  GenericCompilerProfile();
  FetchGpuGuest guest;
  std::vector<StaticBufferLengthCost> measured;
  for(uint32_t wave : {32u,64u}) {
    for(uint32_t reads : {1u,8u,32u}) {
      GenericCompilation shader(reads,wave);
      const auto cost=CountStaticBufferLengthQueries(shader.compiled.spirv);
      std::printf("[length-cost] actual wave%u reads%u queries=%u descriptorViews=%zu checked=%u\n",
                  wave,reads,cost.queries,cost.by_descriptor.size(),cost.checked_lengths);
      measured.push_back(cost);
    }
  }
  // All modules compiled/validated with reads and stores still live before the cost RED.
  for(const auto& cost : measured)
    for(const auto& [descriptor,count] : cost.by_descriptor)
      Require("ArrayLengthDescriptorCost","one native range query per static descriptor/view",
              count==1,
              "equivalent accesses repeat an invariant runtime SSBO length query");
}
} // namespace
} // namespace Libs::Graphics
