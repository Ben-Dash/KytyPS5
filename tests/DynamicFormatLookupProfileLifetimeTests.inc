// Ignored CPU/SPIR-V option lifetime regression, no native actions by author.
// Include AFTER DynamicFormatLookupSpirvCostTests.inc; reuse its independent FormatLookupGolden.
namespace Libs::Graphics { namespace {
uint64_t VerifyFormatLookupLifetimeMetadata(const std::vector<uint32_t>& words) {
  constexpr const char* name="DynamicFormatLookupProfileLifetime";
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
  return private_bytes;
}

void CheckDynamicFormatLookupProfileLifetime(VulkanHarness&) {
  constexpr const char* name="DynamicFormatLookupProfileLifetime";
  const char* old_text=std::getenv("KYTY_FORMAT_LOOKUP_SWITCH");
  const bool old_present=old_text!=nullptr;
  const std::string old_value=old_present?old_text:"";
  const auto set_profile=[&](const char* text) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
    Require(name,"profile setup",_putenv_s("KYTY_FORMAT_LOOKUP_SWITCH",text)==0,"cannot restore fixture environment; not RED");
#else
    Require(name,"profile setup",setenv("KYTY_FORMAT_LOOKUP_SWITCH",text,1)==0,"cannot restore fixture environment; not RED");
#endif
  };
  std::array<uint64_t,3> costs{};
  {
    GenericCompilerProfile();
    FetchGpuGuest guest;
    constexpr std::array<const char*,3> profiles{"0","1","0"};
    for(uint32_t phase=0;phase<profiles.size();++phase) {
      set_profile(profiles[phase]);
      // Fresh actual production compilation; no permutation or SPIR-V reuse.
      GenericCompilation shader(1u,32u);
      costs[phase]=VerifyFormatLookupLifetimeMetadata(shader.compiled.spirv);
    }
  } // All guest fixture state is cleaned before the final intended policy failure.
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  Require(name,"environment cleanup",_putenv_s("KYTY_FORMAT_LOOKUP_SWITCH",old_value.c_str())==0,"environment cleanup failed; not RED");
#else
  Require(name,"environment cleanup",old_present?setenv("KYTY_FORMAT_LOOKUP_SWITCH",old_value.c_str(),1)==0:unsetenv("KYTY_FORMAT_LOOKUP_SWITCH")==0,"environment cleanup failed; not RED");
#endif
  std::printf("[profile] format lookup SAMEprocess0/1/0 all128/wrappedmetadataPASS PrivateBytes=%llu/%llu/%llu\n",
      static_cast<unsigned long long>(costs[0]),static_cast<unsigned long long>(costs[1]),static_cast<unsigned long long>(costs[2]));
  Require(name,"default profile boundaries",costs[0]==2048u&&costs[2]==2048u,"default profile changed; not the intended stale-profile RED");
  Require(name,"percompilation option lifetime",costs[1]==0u,
      "middle enabled profile inherited previous compilation's cachedOFF option despite explicit restore");
}
} } // namespace Libs::Graphics
