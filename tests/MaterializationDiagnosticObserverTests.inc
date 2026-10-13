// Include inside ResourceMaterializationTests.cpp's anonymous namespace after MemoFixture.
// Root draft only: no native compile/run by child. Existing MemoPlan/TableMemory controls.
namespace MatDiagnostic = Libs::Graphics::ShaderRecompiler::IR::MaterializationDiagnostics;
struct MaterializationDiagnosticFixture {
  std::array<uint64_t,MatDiagnostic::CounterCount> counters{};
  std::array<uint32_t,static_cast<uint32_t>(MatDiagnostic::Phase::Count)> begins{},ends{};
  std::array<MatDiagnostic::Phase,8> stack{};
  uint32_t depth=0;bool balanced=true;
  static void Count(void* userdata,MatDiagnostic::Counter c,uint64_t value) {
    static_cast<MaterializationDiagnosticFixture*>(userdata)->counters[static_cast<uint32_t>(c)]+=value;
  }
  static void Phase(void* userdata,MatDiagnostic::Phase p,bool begin) {
    auto& self=*static_cast<MaterializationDiagnosticFixture*>(userdata);
    const auto id=static_cast<uint32_t>(p);
    if(begin){++self.begins[id];if(self.depth>=self.stack.size()){self.balanced=false;return;}self.stack[self.depth++]=p;}
    else{++self.ends[id];if(self.depth==0||self.stack[self.depth-1u]!=p){self.balanced=false;return;}--self.depth;}
  }
  uint64_t Value(MatDiagnostic::Counter c) const {return counters[static_cast<uint32_t>(c)];}
  uint32_t Visits(MatDiagnostic::Phase p) const {return begins[static_cast<uint32_t>(p)];}
  bool Good() const {return balanced&&depth==0&&begins==ends;}
  struct Bind {
    MatDiagnostic::Observer observer;
    const MatDiagnostic::Observer* previous;
    explicit Bind(MaterializationDiagnosticFixture& fixture)
        :observer{&fixture,Phase,Count},previous(MatDiagnostic::observer){MatDiagnostic::observer=&observer;}
    ~Bind(){MatDiagnostic::observer=previous;}
  };
};
void TestMaterializationDiagnosticObserverPreservesActualRefresh() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  const auto plan=MemoPlan(MemoPlanExtra::None,false);
  MemoFixture control,observed;
  for(uint32_t stage=0;stage<4;++stage) {
    if(stage==2u){control.user_data[2]=0x3333u;observed.user_data[2]=0x3333u;}
    if(stage==3u){control.memory.words[1]=0xabcdu;observed.memory.words[1]=0xabcdu;}
    Check(control.Refresh(plan),"unobserved control refresh failed");
    MaterializationDiagnosticFixture diagnostic;bool ok=false;
    {MaterializationDiagnosticFixture::Bind bind(diagnostic);ok=observed.Refresh(plan);}
    Check(ok&&MatDiagnostic::observer==nullptr&&diagnostic.Good(),"observer changed result/state or phase stack is unbalanced");
    Check(control.snapshot.buffers==observed.snapshot.buffers&&control.snapshot.images==observed.snapshot.images&&
      control.snapshot.samplers==observed.snapshot.samplers&&control.snapshot.flattened_srt==observed.snapshot.flattened_srt&&
      control.snapshot.user_data==observed.snapshot.user_data&&control.specialization==observed.specialization&&
      control.memo.valid==observed.memo.valid&&control.memo.reused==observed.memo.reused,
      "diagnostic observer changed actual descriptors/flat slots/user data/specialization/memo");
    ResourceSnapshot reference;ResourceSpecialization reference_specialization;
    const SrtRuntime reference_runtime{.user_data=control.user_data,.read_memory=TableMemory::DirectRead,
      .userdata=&control.memory,.read_specialization_memory=TableMemory::StrictRead};
    Check(MaterializeResourcesReference(plan,reference_runtime,reference,reference_specialization)&&
      reference.buffers==observed.snapshot.buffers&&reference.flattened_srt==observed.snapshot.flattened_srt&&
      reference_specialization==observed.specialization&&
      observed.snapshot.buffers[0].dwords[1]==(stage==3u?0xabcdu:0x1001u)&&
      observed.snapshot.buffers[1].dwords[0]==(stage>=2u?0x3333u:0x2222u),
      "observer refresh disagrees with independent reference and explicit changed-input oracle");
    Check(diagnostic.Value(MatDiagnostic::Counter::CompiledCalls)==1u&&
      diagnostic.Value(MatDiagnostic::Counter::CompiledSuccess)==1u&&
      diagnostic.Value(MatDiagnostic::Counter::CompiledFailure)==0u&&
      diagnostic.Value(MatDiagnostic::Counter::ReferenceFallback)==0u&&
      diagnostic.Visits(MatDiagnostic::Phase::Setup)==1u&&diagnostic.Visits(MatDiagnostic::Phase::Active)==1u&&
      diagnostic.Visits(MatDiagnostic::Phase::Flat)==1u&&diagnostic.Visits(MatDiagnostic::Phase::Memo)==1u,
      "compiled branch or finite phase counts are incorrect");
    const bool reused=stage==1u;
    Check(diagnostic.Value(MatDiagnostic::Counter::MemoReused)==(reused?1u:0u)&&
      diagnostic.Value(MatDiagnostic::Counter::MemoMiss)==(reused?0u:1u)&&
      diagnostic.Visits(MatDiagnostic::Phase::Decode)==(reused?0u:1u)&&
      diagnostic.Visits(MatDiagnostic::Phase::Specialization)==(reused?0u:1u),
      "memo branch or decode/specialization phase is misreported");
  }
  // Real failed whole strict-block callback falls back to exact reads; offset0x30 is
  // already independently covered by TestCleanBlockReads. No fabricated success.
  const auto strict_plan=MemoPlan(MemoPlanExtra::MemoryPredicate,true);
  MemoFixture strict_control,strict_observed;
  for(auto* fixture:{&strict_control,&strict_observed}) {
    fixture->block_reads=true;fixture->memory.base=TableMemory::Base+0x30u;
    fixture->user_data[0]=static_cast<uint32_t>(fixture->memory.base);fixture->memory.words[4]=0u;
  }
  Check(strict_control.Refresh(strict_plan),"strict-block control refresh failed");
  MaterializationDiagnosticFixture strict_diagnostic;bool strict_ok=false;
  {MaterializationDiagnosticFixture::Bind bind(strict_diagnostic);strict_ok=strict_observed.Refresh(strict_plan);}
  Check(strict_ok&&strict_diagnostic.Good()&&strict_control.snapshot.buffers==strict_observed.snapshot.buffers&&
    strict_control.snapshot.flattened_srt==strict_observed.snapshot.flattened_srt&&
    strict_control.specialization==strict_observed.specialization&&
    strict_diagnostic.Value(MatDiagnostic::Counter::StrictBlocksEnabled)==1u&&
    strict_diagnostic.Value(MatDiagnostic::Counter::StrictBlockFailures)>0u&&
    strict_diagnostic.Value(MatDiagnostic::Counter::StrictExactFallbacks)>0u,
    "real strict whole-block failure/fallback changed outputs or was not counted");
  // An existing genuinely bad address fails; every opened phase still closes.
  MemoFixture invalid;invalid.user_data[0]=0x40u;
  MaterializationDiagnosticFixture failure;bool rejected=false;
  {MaterializationDiagnosticFixture::Bind bind(failure);rejected=!invalid.Refresh(plan);}
  Check(rejected&&!invalid.memo.valid&&failure.Good()&&MatDiagnostic::observer==nullptr&&
      failure.Value(MatDiagnostic::Counter::CompiledFailure)==1u&&
      failure.Value(MatDiagnostic::Counter::ReferenceFallback)==1u&&
      failure.Value(MatDiagnostic::Counter::ReferenceCalls)==1u,
      "real failing refresh/fallback/early-return phase accounting changed");
}
