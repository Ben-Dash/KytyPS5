// Include after GenericBufferCodegenTests.inc in BindlessLifecycleTests.cpp.
// Fresh real generic compilation plus independent native channel/bounds goldens.
// This observer does not launch tests or alter the unchanged 4800-lane matrix.
#include <set>

namespace Libs::Graphics {
namespace {
void CheckGenericChannelRepetitionNeighbors(VulkanHarness& vulkan) {
  constexpr const char* name="GenericChannelRepetition";
  auto& graphics=vulkan.RuntimeContext();
  vk::PhysicalDeviceSubgroupProperties subgroup{};
  vk::PhysicalDeviceProperties2 properties{};
  properties.pNext=&subgroup;
  graphics.physical_device.getProperties2(&properties);
  Require(name,"actual subgroup setup",subgroup.subgroupSize==32u &&
      static_cast<bool>(subgroup.supportedStages&vk::ShaderStageFlagBits::eCompute) &&
      static_cast<bool>(subgroup.supportedOperations&vk::SubgroupFeatureFlagBits::eBallot),
      "requires actual wave32 host ballot support; unsupported is not a numerical pass");
  GenericCompilerProfile();
  FetchGpuGuest guest;
  using F=Prospero::BufferFormat;
  // Literal expected values deliberately do not call ResolveFormattedSource or
  // repeat the replacement formula. Different source channels expose count3/W.
  const std::vector<GenericNumericCase> cases{
    {"one-XYZW",F::k8UInt,{0x44u,0u,0u,0u},{0x44u,0x44u,0x44u,0x44u}},
    {"two-XYZW",F::k8_8UInt,{0x2244u,0u,0u,0u},{0x44u,0x22u,0x44u,0x22u}},
    {"three-XYZW",F::k32_32_32UInt,{0x11u,0x22u,0x33u,0u},{0x11u,0x22u,0x33u,0x11u}},
    {"four-XYZW",F::k8_8_8_8UInt,{0x44332211u,0u,0u,0u},{0x11u,0x22u,0x33u,0x44u}},
    {"one-WZYX",F::k8UInt,{0x44u,0u,0u,0u},{0x44u,0x44u,0x44u,0x44u},DstSel(7,6,5,4)},
    {"two-WZYX",F::k8_8UInt,{0x2244u,0u,0u,0u},{0x22u,0x44u,0x22u,0x44u},DstSel(7,6,5,4)},
    {"three-WZYX",F::k32_32_32UInt,{0x11u,0x22u,0x33u,0u},{0x11u,0x33u,0x22u,0x11u},DstSel(7,6,5,4)},
    {"four-WZYX",F::k8_8_8_8UInt,{0x44332211u,0u,0u,0u},{0x44u,0x33u,0x22u,0x11u},DstSel(7,6,5,4)},
    {"two-constants",F::k8_8UInt,{0x2244u,0u,0u,0u},{0u,1u,0x44u,0x22u},DstSel(0,1,4,7)},
    {"three-constants",F::k32_32_32UInt,{0x11u,0x22u,0x33u,0u},{0u,1u,0x11u,0x11u},DstSel(0,1,4,7)},
    {"three-exact-end",F::k32_32_32UInt,{0x77u,0x11u,0x22u,0x33u},{0x11u,0x22u,0x33u,0x11u},
      DstSel(4,5,6,7),4u,0u,52u,64u,48u},
    {"three-partial-OOB",F::k32_32_32UInt,{0x11u,0x22u,0x33u,0u},{0u,1u,0u,0u},
      DstSel(4,1,6,7),4u,0u,56u},
    {"three-prefix-carry",F::k32_32_32UInt,{0x11u,0x22u,0x33u,0u},{0u,0u,0u,0u},
      DstSel(4,5,6,7),4u,0u,0xfffffff0u}
  };
  FetchGpuBuffer input(graphics,GenericPrefixBytes+GenericInputBytes,vk::BufferUsageFlagBits::eStorageBuffer);
  HW::Context registers{}; HW::UserConfig user_config{}; HW::Shader shaders{};
  vulkan.RuntimeRenderer().GetCommandScheduler().Begin(registers,user_config,shaders);
  uint32_t requests=0;
  for(const uint32_t wave:{32u,64u}) {
    GenericCompilation shader(1u,wave);
    FetchGpuBuffer output(graphics,wave*16u+16u,vk::BufferUsageFlagBits::eStorageBuffer);
    GenericGpuPipeline pipeline(vulkan,shader,input,output);
    for(const auto& test:cases) {
      pipeline.Run(test,UINT32_MAX,UINT32_MAX); ++requests;
      pipeline.Run(test,0xaaaaaaaau,0x55555555u); ++requests;
    }
  }
  std::printf("[channel-repeat-neighbors] requests=%u all literal goldens/canaries pass; wave32/64 and asymmetric EXEC retained\n",requests);
}

struct GenericChannelDivisionCost { uint32_t dynamic_divisions=0, outside_entry=0; };
GenericChannelDivisionCost GenericChannelEntryCost(const std::vector<uint32_t>& words) {
  constexpr const char* name="GenericChannelEntryCost";
  std::set<uint32_t> constants;
  uint32_t entry=0,function=0;
  GenericChannelDivisionCost cost;
  for(size_t p=5;p<words.size();) {
    const uint32_t count=words[p]>>16;
    const auto op=static_cast<spv::Op>(words[p]&0xffffu);
    Require(name,"real module framing setup",count!=0u && count<=words.size()-p,
        "invalid emitted SPIR-V; not a channel cost RED");
    if(op==spv::OpEntryPoint) entry=words.at(p+2);
    else if(op==spv::OpConstant || op==spv::OpConstantTrue || op==spv::OpConstantFalse)
      constants.insert(words.at(p+2));
    else if(op==spv::OpFunction) function=words.at(p+2);
    else if(op==spv::OpFunctionEnd) function=0;
    else if((op==spv::OpUMod || op==spv::OpUDiv) && !constants.contains(words.at(p+4))) {
      ++cost.dynamic_divisions;
      cost.outside_entry+=function!=entry;
    }
    p+=count;
  }
  Require(name,"real entrypoint setup",entry!=0u,
      "entry function missing; not a channel cost RED");
  return cost;
}
void CheckGenericChannelRepetitionCost(VulkanHarness& vulkan) {
  // Prove actual supported1/2/3/4-channel bytes and failure boundaries BEFORE
  // intended cost failure, retaining a useful diagnostic on the baseline.
  CheckGenericChannelRepetitionNeighbors(vulkan);
  GenericCompilerProfile();
  FetchGpuGuest guest;
  std::array<GenericChannelDivisionCost,2> costs{};
  constexpr std::array<uint32_t,2> waves{32u,64u};
  for(size_t i=0;i<waves.size();++i) {
    GenericCompilation shader(1u,waves[i]);
    costs[i]=GenericChannelEntryCost(shader.compiled.spirv);
    Require("GenericChannelCost","synthetic operation attribution setup",costs[i].outside_entry==0u,
        "unexpected variable division in a helper; not the intended metadata cost RED");
    std::printf("[channel-entry-cost] wave=%u genericSlots=1 XYZW output live dynamicDivisions=%u\n",
        waves[i],costs[i].dynamic_divisions);
  }
  for(size_t i=0;i<waves.size();++i)
    Require("GenericChannelCost","bounded channel selection work",costs[i].dynamic_divisions==0u,
        "small runtime channel domain invokes general variable-divisor division at invocation entry");
}
} // namespace
} // namespace Libs::Graphics
