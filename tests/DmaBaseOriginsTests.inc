// CPU neighboring contracts for CollectShaderInfo after resource tracking.
// Include beside FirstUseDmaBaseTests.inc after the production collector exists.
#include "graphics/shader/recompiler/ir/passes/ShaderInfoCollection.h"
namespace Libs::Graphics {
namespace {
struct DmaOriginFixture {
  FetchGpuIR::Program program;
  FetchGpuIR::Block* block=nullptr;
  DmaOriginFixture() {
    program.stage=ShaderType::Compute;
    program.user_data_count=64u;
    program.resource_tracking_complete=true;
    program.info.uses_dma=true;
    auto storage=std::make_unique<FetchGpuIR::Block>();
    block=storage.get(); program.blocks.push_back(block);
    program.block_storage.push_back(std::move(storage));
  }
  FetchGpuIR::Value Emit(FetchGpuIR::ValueOpcode op,std::initializer_list<FetchGpuIR::Value> args={},uint64_t flags=0u) {
    return FetchGpuIR::Value(&block->AppendNewInst(op,args,flags));
  }
  FetchGpuIR::Value UserData(uint32_t reg) {
    return Emit(FetchGpuIR::ValueOpcode::GetUserData,{FetchGpuIR::Value(static_cast<FetchGpuIR::ScalarReg>(reg))});
  }
  uint64_t AddMemory(FetchGpuIR::ResourceKind kind,bool full=true,bool planning_only=false) {
    FetchGpuIR::MemoryInfo memory{};
    memory.kind=kind; memory.address_is_full=full; memory.planning_only=planning_only;
    const FetchGpuIR::MemoryFlags flags{static_cast<uint32_t>(program.memory_info.size()),0x40u};
    uint64_t bits=0u; std::memcpy(&bits,&flags,sizeof(flags));
    program.memory_info.push_back(memory); return bits;
  }
  FetchGpuIR::Value Address(FetchGpuIR::Value low,FetchGpuIR::Value high,
      FetchGpuIR::ResourceKind kind=FetchGpuIR::ResourceKind::Flat,bool full=true,bool planning_only=false) {
    const auto handle=Emit(FetchGpuIR::ValueOpcode::GetAddressResource,{low,high});
    return Emit(FetchGpuIR::ValueOpcode::LoadAddressU32,
      {handle,full ? low : FetchGpuIR::Value(0u),full ? high : FetchGpuIR::Value(0u),FetchGpuIR::Value(true)},
      AddMemory(kind,full,planning_only));
  }
  void IndirectBuffer(FetchGpuIR::Value low,FetchGpuIR::Value high) {
    const auto handle=Emit(FetchGpuIR::ValueOpcode::GetBufferResource,
      {low,high,FetchGpuIR::Value(64u),FetchGpuIR::Value(0u)});
    Emit(FetchGpuIR::ValueOpcode::LoadBufferU32,
      {handle,FetchGpuIR::Value(0u),FetchGpuIR::Value(0u),FetchGpuIR::Value(0u),FetchGpuIR::Value(true)},
      AddMemory(FetchGpuIR::ResourceKind::IndirectBuffer,false));
  }
  void Expect(const char* label,std::vector<FetchGpuIR::ShaderInfo::DmaBaseRegister> expected) {
    ShaderComputeInputInfo compute{};
    FetchGpuIR::CollectShaderInfo(program,ShaderStageInputInfo{.compute=&compute});
    Require("DmaBaseOrigins",label,program.info.dma_base_registers==expected,
      "collector inferred an unsupportedbase or lost a known decodedaddress base");
  }
};

void CheckDmaBaseOrigins() {
  using Op=FetchGpuIR::ValueOpcode;
  using Value=FetchGpuIR::Value;
  using Kind=FetchGpuIR::ResourceKind;
  {
    DmaOriginFixture f;
    const auto lane=f.Emit(Op::UndefU32);
    const auto offset=f.Emit(Op::IMul32,{f.UserData(10u),f.UserData(11u)});
    const auto index=f.Emit(Op::IAdd32,{lane,offset});
    const auto sum=f.Emit(Op::IAddCarry32,{f.UserData(12u),index});
    const auto low=f.Emit(Op::CompositeExtractU32x2,{sum,Value(0u)});
    const auto carry=f.Emit(Op::CompositeExtractU32x2,{sum,Value(1u)});
    const auto high=f.Emit(Op::IAdd32,{f.UserData(13u),carry});
    f.Address(low,high);
    f.Address(f.UserData(4u),f.UserData(5u),Kind::Global,false);
    f.Expect("carry retains base but not adjacent scaledoffsetinputs",{{4u,UINT32_MAX},{12u,UINT32_MAX}});
  }
  {
    DmaOriginFixture f;
    const auto pair=f.Emit(Op::CompositeConstructU32x4,
      {f.UserData(20u),f.UserData(21u),f.UserData(22u),f.UserData(23u)});
    const auto low=f.Emit(Op::CompositeExtractU32x4,{pair,Value(0u)});
    const auto high=f.Emit(Op::CompositeExtractU32x4,{pair,Value(1u)});
    f.Address(low,high); f.Address(low,high);
    f.Expect("selected compositecomponents and repeatedaddress dedup",{{20u,UINT32_MAX}});
  }
  {
    DmaOriginFixture f;
    const auto condition=f.Emit(Op::ULessThan32,{f.UserData(2u),f.UserData(3u)});
    const auto low=f.Emit(Op::SelectU32,{condition,f.UserData(0u),f.Emit(Op::UndefU32)});
    const auto high=f.Emit(Op::SelectU32,{condition,f.UserData(1u),f.Emit(Op::UndefU32)});
    f.Address(low,high);
    f.Expect("selectcondition userdata is not pointerprovenance",{{0u,UINT32_MAX}});
  }
  {
    DmaOriginFixture f;
    const auto low=f.Emit(Op::Phi),high=f.Emit(Op::Phi);
    low.Instruction()->SetFlags(FetchGpuIR::Type::U32);
    high.Instruction()->SetFlags(FetchGpuIR::Type::U32);
    low.Instruction()->AddPhiOperand(f.block,f.UserData(16u));
    low.Instruction()->AddPhiOperand(f.block,f.Emit(Op::IAdd32,{low,Value(4u)}));
    high.Instruction()->AddPhiOperand(f.block,f.UserData(17u));
    high.Instruction()->AddPhiOperand(f.block,high);
    f.Address(low,high);
    f.Expect("bounded cyclicphi retains known additiveorigin",{{16u,UINT32_MAX}});
  }
  {
    DmaOriginFixture f;
    const auto packed=f.Emit(Op::BitwiseOr32,{f.UserData(29u),Value(0x04000000u)});
    f.IndirectBuffer(f.UserData(28u),packed);
    // The same raw64-bit expression cannot drop highaddress flags.
    f.Address(f.UserData(28u),packed);
    f.Expect("indirectbuffer addressmask preserves packeddescriptorflags",{{28u,0xffffu}});
  }
  {
    DmaOriginFixture f;
    const auto table_low=f.UserData(30u),table_high=f.UserData(31u);
    const auto loaded_low=f.Address(table_low,table_high,Kind::ScalarAddress,false,true);
    const auto loaded_high=f.Address(table_low,table_high,Kind::ScalarAddress,false,true);
    f.Address(loaded_low,loaded_high);
    f.Expect("loadedpointer never follows tableaddress operands",{});
  }
  for(uint32_t variation=0u;variation<4u;++variation) {
    DmaOriginFixture f;
    const auto low=f.UserData(8u),lane=f.Emit(Op::UndefU32);
    const auto transformed=variation==0u ? f.Emit(Op::IMul32,{low,lane}) :
      variation==1u ? f.Emit(Op::ShiftLeftLogical32,{low,Value(2u)}) :
      variation==2u ? f.Emit(Op::ISub32,{lane,low}) :
                      f.Emit(Op::BitwiseAnd32,{low,Value(0xffu)});
    f.Address(transformed,f.UserData(9u));
    f.Expect("scaled shuffled or reversedaddress does not retain rawbase",{});
  }
  {
    DmaOriginFixture f;
    f.Address(f.UserData(6u),f.UserData(9u));
    const auto low=f.Emit(Op::IAdd32,{f.UserData(6u),f.Emit(Op::IMul32,{f.UserData(7u),f.Emit(Op::UndefU32)})});
    f.Address(low,f.UserData(7u));
    f.Expect("nonadjacent pair and highdword consumedby lowoffset refused",{});
  }
  for(uint32_t adds:{254u,255u}) {
    DmaOriginFixture f;
    const auto lane=f.Emit(Op::UndefU32);
    auto low=f.UserData(34u);
    for(uint32_t i=0u;i<adds;++i)low=f.Emit(Op::IAdd32,{low,lane});
    f.Address(low,f.UserData(35u));
    f.Expect(adds==254u ? "exact traversalbudget retains base" : "overbudget refuses partialprovenance",
      adds==254u ? std::vector<FetchGpuIR::ShaderInfo::DmaBaseRegister>{{34u,UINT32_MAX}} :
                   std::vector<FetchGpuIR::ShaderInfo::DmaBaseRegister>{});
  }
  {
    DmaOriginFixture f;
    f.Address(f.UserData(36u),f.UserData(37u),Kind::Scratch);
    f.Address(f.UserData(38u),f.UserData(39u),Kind::FlatLocal);
    f.UserData(40u); f.UserData(41u);
    f.Expect("scratch localflat and unuseduserdata stayexcluded",{});
  }
  std::printf("[dma-origins] 14 independent CPUprovenance neighbors PASS\n");
}
} // namespace
} // namespace Libs::Graphics
