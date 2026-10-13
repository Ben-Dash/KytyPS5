// Ignored CPU-only audit controls. Include INSIDE ShaderBatchAudit.cpp unnamed namespace,
// AFTER RestoreSettings. Include SpirvEmitter.h GLOBALLY. No Vulkan/device/game/record needed.
struct FormatAuditEnvironmentScope {
  struct Saved {std::string name,value;bool present;};
  std::vector<Saved> saved;
  bool restored=false;
  FormatAuditEnvironmentScope() {
    bool has_format=false;
    for(const char* name:EnvironmentNames) {
      const char* value=std::getenv(name);saved.push_back({name,value?value:"",value!=nullptr});
      has_format|=std::string_view(name)=="KYTY_FORMAT_LOOKUP_SWITCH";
    }
    if(!has_format) {
      const char* value=std::getenv("KYTY_FORMAT_LOOKUP_SWITCH");
      saved.push_back({"KYTY_FORMAT_LOOKUP_SWITCH",value?value:"",value!=nullptr});
    }
  }
  static bool Set(const char* name,const char* value) {
#if defined(_WIN32)
    return _putenv_s(name,value?value:"")==0;
#else
    return value?setenv(name,value,1)==0:unsetenv(name)==0;
#endif
  }
  bool Restore() {
    bool ok=true;
    for(const auto& item:saved) {
      ok&=Set(item.name.c_str(),item.present?item.value.c_str():nullptr);
      const char* actual=std::getenv(item.name.c_str());
      ok&=item.present?(actual&&std::string_view(actual)==item.value):actual==nullptr;
    }
    restored=ok;return ok;
  }
  ~FormatAuditEnvironmentScope() {if(!restored)(void)Restore();}
};
Json MakeLegacyFormatAuditProfile() {
  Json environment=Json::object();
  for(const char* name:EnvironmentNames)if(std::string_view(name)!="KYTY_FORMAT_LOOKUP_SWITCH")environment[name]=nullptr;
  return Json{{"schema_version",1},{"profile_label","synthetic-format-lookup"},
    {"environment",environment},{"hardware_storage_buffer_bounds",false},
    {"native_float16",false},{"table_mode",false},{"bindless_images",false}};
}
int CheckFormatAuditLegacyControls() {
  FormatAuditEnvironmentScope scope;
  const auto legacy=MakeLegacyFormatAuditProfile();
  const auto rejected=[](const Json& value) {
    try {RestoreSettings(value);}catch(const std::exception&) {return true;}
    return false;
  };
  auto missing=legacy;missing["environment"].erase("KYTY_BUFFER_SPECIALIZE");
  auto typed=legacy;typed["environment"]["KYTY_HALF_UNIFORM"]=7;
  auto extra=legacy;extra["environment"]["UNSUPPORTED_TEST_SWITCH"]="1";
  const bool strict_missing=rejected(missing),strict_type=rejected(typed),strict_extra=rejected(extra);
  if(!FormatAuditEnvironmentScope::Set("KYTY_FORMAT_LOOKUP_SWITCH","1"))throw std::runtime_error("synthetic environment setup failed; not RED");
  bool legacy_accepted=true;
  try {RestoreSettings(legacy);}catch(const std::exception&) {legacy_accepted=false;}
  const char* raw=std::getenv("KYTY_FORMAT_LOOKUP_SWITCH");
  const bool cleared=raw==nullptr&&!ShaderRecompiler::Spirv::DynamicFormatLookupSwitchEnabled();
  const bool cleanup=scope.Restore();
  std::printf("[audit-profile] legacyAccepted=%d strictMissing=%d strictType=%d strictExtra=%d cleanup=%d missingFormatCleared=%d expected1\n",
      legacy_accepted,strict_missing,strict_type,strict_extra,cleanup,cleared);
  if(!(legacy_accepted&&strict_missing&&strict_type&&strict_extra&&cleanup))
    throw std::runtime_error("legacy/type/missing/extra/environment-cleanup controls failed; not intended RED");
  if(!cleared) {std::fprintf(stderr,"legacy manifest inherited caller format-lookup flag despite missing defaultOFF field\n");return 1;}
  return 0;
}
int CheckFormatAuditExplicitProfiles() {
  FormatAuditEnvironmentScope scope;
  auto profile=MakeLegacyFormatAuditProfile();
  constexpr std::array<const char*,3> values{"0","1","0"};
  std::array<bool,3> actual{};
  for(size_t i=0;i<values.size();++i) {
    profile["environment"]["KYTY_FORMAT_LOOKUP_SWITCH"]=values[i];
    RestoreSettings(profile);
    actual[i]=ShaderRecompiler::Spirv::DynamicFormatLookupSwitchEnabled();
  }
  auto typed=profile;typed["environment"]["KYTY_FORMAT_LOOKUP_SWITCH"]=true;
  bool typed_rejected=false;try {RestoreSettings(typed);}catch(const std::exception&) {typed_rejected=true;}
  const bool cleanup=scope.Restore();
  std::printf("[audit-profile] explicit0/1/0=%d/%d/%d newFieldTypeRejected=%d cleanup=%d\n",
      actual[0],actual[1],actual[2],typed_rejected,cleanup);
  if(actual!=std::array<bool,3>{false,true,false}||!typed_rejected||!cleanup)
    throw std::runtime_error("explicit format profile restoration/type/cleanup mismatch");
  return 0;
}
