// Ignored CPU/SPIR-V regression draft; include after GenericBufferCodegenTests.inc.
// No native execution by this agent. Root must run genuine structural RED before proposal.
// Literal independent public-format metadata oracle (enum widths/types),
// not full ISA formatted-load correctness proof.
#include <map>
namespace Libs::Graphics { namespace {
static constexpr std::array<std::array<uint32_t,4>,128> FormatLookupGolden {{
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000209u,0x00000008u,0x00000000u,0x00000003u},
  {0x00000209u,0x00000008u,0x00000000u,0x00000004u},
  {0x00000209u,0x00000008u,0x00000000u,0x00000005u},
  {0x00000209u,0x00000008u,0x00000000u,0x00000006u},
  {0x00000209u,0x00000008u,0x00000000u,0x00000001u},
  {0x00000209u,0x00000008u,0x00000000u,0x00000002u},
  {0x00000211u,0x00000010u,0x00000000u,0x00000003u},
  {0x00000211u,0x00000010u,0x00000000u,0x00000004u},
  {0x00000211u,0x00000010u,0x00000000u,0x00000005u},
  {0x00000211u,0x00000010u,0x00000000u,0x00000006u},
  {0x00000211u,0x00000010u,0x00000000u,0x00000001u},
  {0x00000211u,0x00000010u,0x00000000u,0x00000002u},
  {0x00000211u,0x00000010u,0x00000000u,0x00000007u},
  {0x00000212u,0x00000208u,0x00000400u,0x00000003u},
  {0x00000212u,0x00000208u,0x00000400u,0x00000004u},
  {0x00000212u,0x00000208u,0x00000400u,0x00000005u},
  {0x00000212u,0x00000208u,0x00000400u,0x00000006u},
  {0x00000212u,0x00000208u,0x00000400u,0x00000001u},
  {0x00000212u,0x00000208u,0x00000400u,0x00000002u},
  {0x00000221u,0x00000020u,0x00000000u,0x00000001u},
  {0x00000221u,0x00000020u,0x00000000u,0x00000002u},
  {0x00000221u,0x00000020u,0x00000000u,0x00000007u},
  {0x00000222u,0x00000410u,0x00000800u,0x00000003u},
  {0x00000222u,0x00000410u,0x00000800u,0x00000004u},
  {0x00000222u,0x00000410u,0x00000800u,0x00000005u},
  {0x00000222u,0x00000410u,0x00000800u,0x00000006u},
  {0x00000222u,0x00000410u,0x00000800u,0x00000001u},
  {0x00000222u,0x00000410u,0x00000800u,0x00000002u},
  {0x00000222u,0x00000410u,0x00000800u,0x00000007u},
  {0x00000323u,0x0000a2cbu,0x00058580u,0x00000003u},
  {0x00000323u,0x0000a2cbu,0x00058580u,0x00000004u},
  {0x00000323u,0x0000a2cbu,0x00058580u,0x00000005u},
  {0x00000323u,0x0000a2cbu,0x00058580u,0x00000006u},
  {0x00000323u,0x0000a2cbu,0x00058580u,0x00000001u},
  {0x00000323u,0x0000a2cbu,0x00058580u,0x00000002u},
  {0x00000323u,0x0000a2cbu,0x00058580u,0x00000007u},
  {0x00000323u,0x0000b2cau,0x00054500u,0x00000003u},
  {0x00000323u,0x0000b2cau,0x00054500u,0x00000004u},
  {0x00000323u,0x0000b2cau,0x00054500u,0x00000005u},
  {0x00000323u,0x0000b2cau,0x00054500u,0x00000006u},
  {0x00000323u,0x0000b2cau,0x00054500u,0x00000001u},
  {0x00000323u,0x0000b2cau,0x00054500u,0x00000002u},
  {0x00000323u,0x0000b2cau,0x00054500u,0x00000007u},
  {0x00000324u,0x0028a282u,0x02c30100u,0x00000003u},
  {0x00000324u,0x0028a282u,0x02c30100u,0x00000004u},
  {0x00000324u,0x0028a282u,0x02c30100u,0x00000005u},
  {0x00000324u,0x0028a282u,0x02c30100u,0x00000006u},
  {0x00000324u,0x0028a282u,0x02c30100u,0x00000001u},
  {0x00000324u,0x0028a282u,0x02c30100u,0x00000002u},
  {0x00000324u,0x0008a28au,0x03c50500u,0x00000003u},
  {0x00000324u,0x0008a28au,0x03c50500u,0x00000004u},
  {0x00000324u,0x0008a28au,0x03c50500u,0x00000005u},
  {0x00000324u,0x0008a28au,0x03c50500u,0x00000006u},
  {0x00000324u,0x0008a28au,0x03c50500u,0x00000001u},
  {0x00000324u,0x0008a28au,0x03c50500u,0x00000002u},
  {0x00000224u,0x00208208u,0x03040400u,0x00000003u},
  {0x00000224u,0x00208208u,0x03040400u,0x00000004u},
  {0x00000224u,0x00208208u,0x03040400u,0x00000005u},
  {0x00000224u,0x00208208u,0x03040400u,0x00000006u},
  {0x00000224u,0x00208208u,0x03040400u,0x00000001u},
  {0x00000224u,0x00208208u,0x03040400u,0x00000002u},
  {0x00000242u,0x00000820u,0x00001000u,0x00000001u},
  {0x00000242u,0x00000820u,0x00001000u,0x00000002u},
  {0x00000242u,0x00000820u,0x00001000u,0x00000007u},
  {0x00000244u,0x00410410u,0x06080800u,0x00000003u},
  {0x00000244u,0x00410410u,0x06080800u,0x00000004u},
  {0x00000244u,0x00410410u,0x06080800u,0x00000005u},
  {0x00000244u,0x00410410u,0x06080800u,0x00000006u},
  {0x00000244u,0x00410410u,0x06080800u,0x00000001u},
  {0x00000244u,0x00410410u,0x06080800u,0x00000002u},
  {0x00000244u,0x00410410u,0x06080800u,0x00000007u},
  {0x00000263u,0x00020820u,0x00101000u,0x00000001u},
  {0x00000263u,0x00020820u,0x00101000u,0x00000002u},
  {0x00000263u,0x00020820u,0x00101000u,0x00000007u},
  {0x00000284u,0x00820820u,0x0c101000u,0x00000001u},
  {0x00000284u,0x00820820u,0x0c101000u,0x00000002u},
  {0x00000284u,0x00820820u,0x0c101000u,0x00000007u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
  {0x00000001u,0x00000020u,0x00000000u,0x00000000u},
}};

void CheckDynamicFormatLookupSpirvCost(VulkanHarness&) {
  constexpr const char* name="DynamicFormatLookupSpirvCost";
  GenericCompilerProfile();
  FetchGpuGuest guest;
  GenericCompilation shader(1u,32u);
  const auto& words=shader.compiled.spirv;
  using Operands=std::vector<uint32_t>;
  std::map<uint32_t,Operands> types,constants,composites;
  std::vector<Operands> variables;
  uint32_t lookup_function=0u,active_function=0u;
  Operands lookup_switch,lookup_phi;
  const auto text=[&](size_t p,size_t n) {
    std::string value;
    for(size_t i=0;i<n;++i)for(uint32_t shift=0;shift<32u;shift+=8u) {
      const char c=static_cast<char>(words[p+i]>>shift);if(c==0)return value;value+=c;
    }
    return value;
  };
  for(size_t p=5;p<words.size();) {
    const auto n=words[p]>>16u,op=words[p]&65535u;
    Require(name,"module framing",n>0u&&n<=words.size()-p,"invalid existing module; not RED");
    Operands a(words.begin()+p+1u,words.begin()+p+n);
    if(op==spv::OpName && text(p+2u,n-2u)=="dynamic_buffer_format_lookup")lookup_function=a[0];
    if(op==spv::OpTypeInt||op==spv::OpTypeVector||op==spv::OpTypeArray||op==spv::OpTypePointer)
      types[a[0]]=a;
    if(op==spv::OpConstant)constants[a[1]]=a;
    if(op==spv::OpConstantComposite)composites[a[1]]=a;
    if(op==spv::OpVariable)variables.push_back(a);
    if(op==spv::OpFunction)active_function=a[1];
    if(active_function==lookup_function&&lookup_function!=0u) {
      if(op==spv::OpSwitch)lookup_switch=a;
      if(op==spv::OpPhi)lookup_phi=a;
    }
    if(op==spv::OpFunctionEnd)active_function=0u;
    p+=n;
  }
  std::vector<std::array<uint32_t,128>> legacy;
  uint64_t private_bytes=0u;
  for(const auto& v:variables) {
    if(v.size()<4u||v[2]!=spv::StorageClassPrivate)continue;
    const auto pointer=types.find(v[0]);if(pointer==types.end()||pointer->second.size()!=3u)continue;
    const auto array=types.find(pointer->second[2]);if(array==types.end()||array->second.size()!=3u)continue;
    const auto count=constants.find(array->second[2]);if(count==constants.end()||count->second.size()!=3u||count->second[2]!=128u)continue;
    const auto init=composites.find(v[3]);if(init==composites.end()||init->second.size()!=130u)continue;
    const auto element=types.find(array->second[1]);if(element==types.end()||element->second.size()!=3u||element->second[1]!=32u)continue;
    std::array<uint32_t,128> table{};
    for(uint32_t k=0;k<128u;++k) {
      const auto scalar=constants.find(init->second[k+2u]);
      Require(name,"literal table initializer",scalar!=constants.end()&&scalar->second.size()==3u,"nonliteral table unsupported by bounded preservation fixture; not RED");
      table[k]=scalar->second[2];
    }
    legacy.push_back(table);private_bytes+=128u*4u;
  }
  const auto lookup=[&](uint32_t format) {
    std::array<uint32_t,4> result{};
    if(legacy.size()==4u) {
      for(uint32_t field=0;field<4u;++field)result[field]=legacy[field][format&127u];
      return result;
    }
    Require(name,"bounded pure lookup witness",legacy.empty()&&lookup_function!=0u&&lookup_switch.size()>=2u&&lookup_phi.size()>=4u,
        "fixture needs current immutable table or proposed pure constant lookup; unsupported setup is not RED");
    uint32_t selected=lookup_switch[1];
    for(size_t k=2;k+1u<lookup_switch.size();k+=2u)if(lookup_switch[k]==(format&127u))selected=lookup_switch[k+1u];
    uint32_t value=0u;
    for(size_t k=2;k+1u<lookup_phi.size();k+=2u)if(lookup_phi[k+1u]==selected)value=lookup_phi[k];
    const auto tuple=composites.find(value);
    Require(name,"constant selected metadata",tuple!=composites.end()&&tuple->second.size()==6u,"pure lookup must return four literal words; not a cost RED");
    for(uint32_t field=0;field<4u;++field) {
      const auto scalar=constants.find(tuple->second[field+2u]);
      Require(name,"scalar metadata",scalar!=constants.end()&&scalar->second.size()==3u,"nonliteral metadata; not a cost RED");
      result[field]=scalar->second[2];
    }
    return result;
  };
  for(uint32_t code=0;code<128u;++code) {
    Require(name,"all128 metadata preservation",lookup(code)==FormatLookupGolden[code],"format metadata changed");
    Require(name,"sevenbit wrap preservation",lookup(code+128u)==FormatLookupGolden[code]&&lookup(code|0xffffff80u)==FormatLookupGolden[code],"format index must preserve existing sevenbit masking");
  }
  std::printf("[cost] format lookup all128/wrapped metadata PASS readonlyPrivateBytes=%llu\n",static_cast<unsigned long long>(private_bytes));
  Require(name,"immutable perinvocation storage budget",private_bytes==0u,
      "readonly format lookup retains2048 perinvocation declared Private bytes; use semantically equivalent bounded constant selection without perinvocation tables");
}
} } // namespace Libs::Graphics

