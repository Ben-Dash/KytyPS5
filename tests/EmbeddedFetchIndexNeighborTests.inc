#include <algorithm>
#include <optional>

namespace Libs::Graphics {
namespace {

void InsertNeighbor(Fixture& fixture, size_t word, const std::vector<u32>& added) {
  fixture.code.insert(fixture.code.begin() + static_cast<std::ptrdiff_t>(word),
                      added.begin(), added.end());
  if (word * sizeof(u32) <= fixture.fetch_pc)
    fixture.fetch_pc += static_cast<u32>(added.size() * sizeof(u32));
}

u32 EvaluateNeighbor(const char* name, IR::Value value, u32 raw, unsigned depth = 0) {
  Require(name, "neighbor IR expression", depth < 128u, "unexpected cyclic expression");
  value = value.Resolve();
  if (value.IsImmediate()) return Evaluate(name, value, raw);
  const auto* inst = value.Instruction();
  const auto arg = [&](size_t index) {
    return EvaluateNeighbor(name, inst->Arg(index), raw, depth + 1u);
  };
  switch (inst->GetOpcode()) {
  case IR::ValueOpcode::GetUserData:
    Require(name, "offset input", IR::RegIndex(inst->Arg(0).ScalarRegister()) == 18u,
            "only the fixture's actual s18=7 offset input is expected");
    return 7u;
  case IR::ValueOpcode::GetBuiltin:
    if (inst->Arg(0).U32() == static_cast<u32>(IR::StageInputKind::VertexIndex)) return raw;
    if (inst->Arg(0).U32() == static_cast<u32>(IR::StageInputKind::InstanceIndex)) return 7u;
    break;
  case IR::ValueOpcode::LaneId: return 0u;
  case IR::ValueOpcode::CompositeExtractU32x2: {
    const auto component = arg(1);
    const auto* pair = inst->Arg(0).ResolveInstruction();
    Require(name, "actual pair expression", component < 2u && pair != nullptr,
            "expected a two-DWORD actual IR producer");
    if (pair->GetOpcode() == IR::ValueOpcode::CompositeConstructU32x2)
      return EvaluateNeighbor(name, pair->Arg(component), raw, depth + 1u);
    Require(name, "actual pair expression", pair->GetOpcode() == IR::ValueOpcode::IAddCarry32,
            "unsupported pair producer; no memory/attribute value may be fabricated");
    // Addition's actual pair holds low32 sum and high32 carry. Follow each
    // producer argument, including earlier adds, instead of substituting the
    // fixture's expected final raw+14 value.
    const auto lhs = EvaluateNeighbor(name, pair->Arg(0), raw, depth + 1u);
    const auto rhs = EvaluateNeighbor(name, pair->Arg(1), raw, depth + 1u);
    const auto sum = static_cast<uint64_t>(lhs) + rhs;
    return component == 0u ? static_cast<u32>(sum) : static_cast<u32>(sum >> 32u);
  }
  case IR::ValueOpcode::IAdd32: return arg(0) + arg(1);
  case IR::ValueOpcode::ISub32: return arg(0) - arg(1);
  case IR::ValueOpcode::UMin32: return std::min(arg(0), arg(1));
  case IR::ValueOpcode::UMax32: return std::max(arg(0), arg(1));
  case IR::ValueOpcode::BitwiseAnd32: return arg(0) & arg(1);
  case IR::ValueOpcode::BitwiseOr32: return arg(0) | arg(1);
  case IR::ValueOpcode::ShiftRightLogical32: return arg(0) >> (arg(1) & 31u);
  case IR::ValueOpcode::ShiftLeftLogical32: return arg(0) << (arg(1) & 31u);
  case IR::ValueOpcode::IEqual32: return arg(0) == arg(1);
  case IR::ValueOpcode::INotEqual32: return arg(0) != arg(1);
  case IR::ValueOpcode::ULessThan32: return arg(0) < arg(1);
  case IR::ValueOpcode::LogicalAnd: return arg(0) && arg(1);
  case IR::ValueOpcode::SelectU1:
  case IR::ValueOpcode::SelectU32: return arg(0) ? arg(1) : arg(2);
  default: break;
  }
  return Evaluate(name, value, raw, depth);
}

bool HasWideHighWord(IR::Value value, unsigned depth = 0) {
  value = value.Resolve();
  if (value.IsImmediate() || depth >= 128u) return false;
  const auto* inst = value.Instruction();
  if (inst->GetOpcode() == IR::ValueOpcode::CompositeExtractU64 &&
      inst->Arg(1).IsImmediate() && inst->Arg(1).U32() == 1u) return true;
  for (size_t i = 0; i < inst->NumArgs(); ++i)
    if (HasWideHighWord(inst->Arg(i), depth + 1u)) return true;
  return false;
}

bool DependsOnNeighborInst(IR::Value value, const IR::Inst* wanted, unsigned depth = 0) {
  value = value.Resolve();
  if (value.IsImmediate() || depth >= 128u) return false;
  const auto* inst = value.Instruction();
  if (inst == wanted) return true;
  for (size_t i = 0; i < inst->NumArgs(); ++i)
    if (DependsOnNeighborInst(inst->Arg(i), wanted, depth + 1u)) return true;
  return false;
}

void CheckTwoFetchOffsetNeighbor(const Fixture& fixture, u32 second_pc) {
  constexpr const char* name = "two-fetch-offset-reset";
  auto result = Translate(name, fixture);
  const auto& program = result.program;
  constexpr u32 raw = 0x001d0001u;
  const auto selected = [&](u32 parameter) {
    return EvaluateNeighbor(name, ExportComponent(name, program, parameter, 0), raw);
  };
  Require(name, "independent selected guest indices", selected(2) == raw + 7u && selected(4) == raw,
          "first guest index includes7; canonical reselection resets second index to raw");
  Require(name, "raw VertexID", selected(1) == raw &&
              EvaluateNeighbor(name, ExportComponent(name, program, 1, 1), raw) == 29u,
          "both consumers must retain independent raw/high VertexID exports");
  const IR::Inst* first = nullptr;
  const IR::Inst* second = nullptr;
  size_t attributes = 0;
  for (const auto* block : program.blocks) {
    for (const auto& inst : *block) {
      if (inst.GetOpcode() == IR::ValueOpcode::GetAttribute) ++attributes;
      if (IR::BufferAccessOf(inst.GetOpcode()) != IR::BufferAccess::Read) continue;
      const auto pc = inst.Flags<IR::MemoryFlags>().pc;
      if (pc == fixture.fetch_pc) first = &inst;
      if (pc == second_pc) second = &inst;
    }
  }
  std::printf("EmbeddedFetchIndexNeighbors: %s attributes=%zu first_fetch=%u second_fetch=%u offset=%d\n",
              name, attributes, first != nullptr ? 1u : 0u, second != nullptr ? 1u : 0u,
              program.info.vertex_offset_sgpr);
  Require(name, "second consumer ownership", second != nullptr &&
              DependsOnNeighborInst(ExportComponent(name, program, 3, 0), second) &&
              EvaluateNeighbor(name, second->Arg(1), raw) == raw,
          "one global native draw offset cannot represent both guest fetch indices");
  if (first != nullptr) {
    Require(name, "fully manual representation", attributes == 0u &&
                program.info.vertex_offset_sgpr == -1 &&
                DependsOnNeighborInst(ExportComponent(name, program, 0, 0), first) &&
                EvaluateNeighbor(name, first->Arg(1), raw) == raw + 7u,
            "manual loads must preserve both guest expressions with no native draw offset");
  } else {
    Require(name, "mixed correct representation", attributes == 4u &&
                program.info.vertex_offset_sgpr == 18 &&
                ExportComponent(name, program, 0, 0).ResolveInstruction() != nullptr &&
                ExportComponent(name, program, 0, 0).ResolveInstruction()->GetOpcode() ==
                    IR::ValueOpcode::GetAttribute,
            "first native fetch may use7 only while second remains an actual raw-index shader load");
  }
  std::printf("EmbeddedFetchIndexNeighbors: %s PASS\n", name);
}

// Native policy is optional for exact identities: either optimization is
// correct, provided the actual guest index and raw-ID outputs are preserved.
void CheckNeighbor(const char* name, const Fixture& fixture,
                   std::optional<bool> native, std::optional<u32> expected_index,
                   int vertex_offset = -1, bool wide_high_word = false,
                   bool exact_negative_offset = false) {
  if (std::strcmp(name, "branch-index-join") == 0 ||
      std::strcmp(name, "backedge-index-use") == 0) {
    ShaderRecompiler::Decoder::Program decoded;
    ShaderRecompiler::Decoder::DecodeProgram(fixture.code, decoded);
    size_t branches = 0;
    for (const auto& inst : decoded.instructions) {
      if (inst.opcode != ShaderRecompiler::Decoder::Opcode::S_CBRANCH_SCC1) continue;
      ++branches;
      Require(name, "real decoded CFG edge", inst.branch_target == fixture.fetch_pc,
              "synthetic conditional branch must target the live guest fetch");
    }
    Require(name, "real decoded CFG edge", branches == 1u &&
                decoded.instructions.back().opcode == ShaderRecompiler::Decoder::Opcode::S_ENDPGM,
            "CFG neighbor must contain a real SOPP conditional branch and terminal instruction");
  }
  auto result = Translate(name, fixture);
  const auto& program = result.program;
  constexpr u32 raw = 0x001d0001u;
  Require(name, "raw VertexID", Evaluate(name, ExportComponent(name, program, 1, 0), raw) == raw &&
              Evaluate(name, ExportComponent(name, program, 1, 1), raw) == 29u,
          "neighbor must retain independent raw/high VertexID exports");
  size_t attributes = 0;
  const IR::Inst* fetch = nullptr;
  for (const auto* block : program.blocks) {
    for (const auto& inst : *block) {
      if (inst.GetOpcode() == IR::ValueOpcode::GetAttribute) ++attributes;
      if (IR::BufferAccessOf(inst.GetOpcode()) == IR::BufferAccess::Read &&
          inst.Flags<IR::MemoryFlags>().pc == fixture.fetch_pc) fetch = &inst;
    }
  }
  std::printf("EmbeddedFetchIndexNeighbors: %s attributes=%zu fetch=%u offset=%d\n",
              name, attributes, fetch != nullptr ? 1u : 0u, program.info.vertex_offset_sgpr);
  if (attributes == 0u && fetch == nullptr) {
    const auto dump = IR::ProgramToString(program);
    std::fprintf(stderr, "EmbeddedFetchIndexNeighbors: missing guest fetch pc=%08x; actual IR:\n%s\n",
                 fixture.fetch_pc, dump.c_str());
  }
  if (native == true) {
    Require(name, "supported native control", attributes == 4u && fetch == nullptr,
            "supported once-only offset/Local fetch must retain native input");
  } else if (native == false) {
    Require(name, "neighbor eligibility", attributes == 0u && fetch != nullptr,
            "guest index mutation/control flow is not represented by native input");
  } else {
    Require(name, "identity representation", (attributes == 4u && fetch == nullptr) ||
                (attributes == 0u && fetch != nullptr), "identity fetch was lost or mixed");
  }
  if (fetch != nullptr && expected_index)
    Require(name, "actual guest index", EvaluateNeighbor(name, fetch->Arg(1), raw) == *expected_index,
            "retained actual IR must preserve the independently expected guest index");
  if (wide_high_word)
    Require(name, "wide high-word address", fetch != nullptr && HasWideHighWord(fetch->Arg(1)),
            "actual fetch index must depend on the second DWORD of the wide result");
  if (vertex_offset >= 0 || exact_negative_offset)
    Require(name, "offset metadata", program.info.vertex_offset_sgpr == vertex_offset,
            "supported once-only draw offset metadata changed");
  std::printf("EmbeddedFetchIndexNeighbors: %s PASS\n", name);
}

void CheckEmbeddedFetchIndexNeighbors(std::string_view selected) {
  bool matched = false;
  const auto enabled = [&](std::string_view name) {
    if (selected == "neighbors" || selected == name) { matched = true; return true; }
    return false;
  };
  if (enabled("neighbor-single-offset")) {
    auto fixture = MakeFixture(Case::Canonical);
    std::vector<u32> add;
    Vop3(add, 0x30fu, 0, 18u, Vgpr(0));
    InsertNeighbor(fixture, fixture.fetch_pc / 4u, add);
    CheckNeighbor("single-v0-offset", fixture, true, {}, 18);
  }
  if (enabled("neighbor-local-offset")) {
    auto fixture = MakeFixture(Case::Local);
    std::vector<u32> add;
    Vop3(add, 0x30fu, 0, 18u, Vgpr(0));
    InsertNeighbor(fixture, fixture.fetch_pc / 4u, add);
    CheckNeighbor("local-v0-offset", fixture, true, {}, 18);
  }
  if (enabled("neighbor-repeated-offset")) {
    auto fixture = MakeFixture(Case::Canonical);
    std::vector<u32> adds;
    Vop3(adds, 0x30fu, 0, 18u, Vgpr(0));
    Vop3(adds, 0x30fu, 0, 18u, Vgpr(0));
    InsertNeighbor(fixture, fixture.fetch_pc / 4u, adds);
    // Two guest adds need raw+14; a single draw-offset SGPR cannot encode it.
    CheckNeighbor("repeated-same-v0-offset", fixture, false, 0x001d0001u + 14u);
  }
  if (enabled("neighbor-wide-abi")) {
    auto fixture = MakeFixture(Case::Canonical);
    std::vector<u32> wide;
    Vop3(wide, 0x164u, 4, Inline(0), Inline(0));
    // V_ADD_F64 writes v[4:5]; its SECOND dword overwrites ABI vertex v5.
    // Inspect the actual high-DWORD dependency; FP64 arithmetic is not modeled
    // by this CPU fixture and no memory/attribute value is fabricated.
    InsertNeighbor(fixture, 5u, wide);
    CheckNeighbor("wide-second-dword-abi", fixture, false, {}, -1, true);
  }
  if (enabled("neighbor-relative-write")) {
    auto fixture = MakeFixture(Case::Canonical);
    const std::vector<u32> write{
        0xbefc0380u, // s_mov_b32 m0,0
        (0x3fu << 25u) | (0x42u << 9u) | Inline(3)}; // v_movreld v0,3
    InsertNeighbor(fixture, fixture.fetch_pc / 4u, write);
    CheckNeighbor("relative-index-write", fixture, false, 3u);
  }
  if (enabled("neighbor-identity")) {
    auto selected_copy = MakeFixture(Case::Canonical);
    InsertNeighbor(selected_copy, selected_copy.fetch_pc / 4u, {Vmov(0, Vgpr(0))});
    CheckNeighbor("identity-selected-index", selected_copy, {}, 0x001d0001u);
    auto source_copy = MakeFixture(Case::Canonical);
    InsertNeighbor(source_copy, 5u, {Vmov(5, Vgpr(5))});
    CheckNeighbor("identity-abi-source", source_copy, {}, 0x001d0001u);
  }
  if (enabled("neighbor-join")) {
    auto fixture = MakeFixture(Case::Canonical);
    const std::vector<u32> branch{
        0xbf850002u, // SOPP s_cbranch_scc1 skips the two-word mask
        Vop2(0x1bu, 0, 255u, 0), 0xffffu};
    InsertNeighbor(fixture, fixture.fetch_pc / 4u, branch);
    // A scalar CFG join has both raw and masked reaching indices. This CPU
    // membership test claims no numerical GPU execution of either path.
    CheckNeighbor("branch-index-join", fixture, false, {});
  }
  if (enabled("neighbor-backedge")) {
    auto fixture = MakeFixture(Case::Canonical);
    // Re-enter the actual live fetch/export consumer, rather than depending on
    // an unrelated after-loop EXEC/reachability edge. This CPU test dispatches
    // no shader. The synthetic scalar counter bounds the guest loop to2.
    InsertNeighbor(fixture, fixture.fetch_pc / 4u, {0xbea00382u}); // s_mov s32,2
    fixture.code.pop_back(); // move terminal S_ENDPGM after the finite loop
    fixture.code.push_back(0x8120c120u); // s_add_i32 s32,s32,-1
    fixture.code.push_back(0xbf088020u); // s_cmp_gt_u32 s32,0
    const auto branch_word = static_cast<int32_t>(fixture.code.size());
    const auto target_word = static_cast<int32_t>(fixture.fetch_pc / 4u);
    const auto delta = static_cast<u32>(target_word - branch_word - 1);
    fixture.code.push_back(0xbf850000u | (delta & 0xffffu)); // SOPP, not SOPC
    fixture.code.push_back(0xbf810000u);
    CheckNeighbor("backedge-index-use", fixture, false, {});
  }
  if (enabled("neighbor-discarded-offset")) {
    auto fixture = MakeFixture(Case::Canonical);
    std::vector<u32> discarded;
    Vop3(discarded, 0x30fu, 0, 18u, Vgpr(0));
    // v0 has not selected a guest index yet. The following canonical CNDMASK
    // overwrites this sum; it cannot contribute to native draw-offset metadata.
    InsertNeighbor(fixture, 5u, discarded);
    std::vector<u32> selected_export;
    Export(selected_export, 0x22u, 0, 0, 0, 0);
    InsertNeighbor(fixture, fixture.fetch_pc / 4u, selected_export);
    // Independently observe the actual live guest expression after overwrite,
    // before checking whether its discarded predecessor poisoned host metadata.
    auto selected_result = Translate("discarded-preselect-offset-expression", fixture);
    constexpr u32 raw = 0x001d0001u;
    Require("discarded-preselect-offset", "actual selected guest index",
            EvaluateNeighbor("discarded-preselect-offset",
                ExportComponent("discarded-preselect-offset", selected_result.program, 2, 0), raw) == raw,
            "canonical selection must overwrite the earlier v0 sum with raw VertexID");
    CheckNeighbor("discarded-preselect-offset", fixture, true, {}, -1, false, true);
  }
  if (enabled("neighbor-discarded-selected-offset")) {
    auto fixture = MakeFixture(Case::Canonical);
    std::vector<u32> reselection;
    Vop3(reselection, 0x30fu, 0, 18u, Vgpr(0));
    Vop3(reselection, 0x101u, 0, Vgpr(8), Vgpr(5), 193u);
    Export(reselection, 0x22u, 0, 0, 0, 0);
    InsertNeighbor(fixture, fixture.fetch_pc / 4u, reselection);
    auto selected_result = Translate("discarded-selected-offset-expression", fixture);
    constexpr u32 raw = 0x001d0001u;
    Require("discarded-selected-offset", "actual final selected guest index",
            EvaluateNeighbor("discarded-selected-offset",
                ExportComponent("discarded-selected-offset", selected_result.program, 2, 0), raw) == raw,
            "second canonical selection must discard the formerly useful offset");
    // Exact identity can use native input or a shader load. Both must forget
    // the discarded host draw-offset metadata and retain the final raw index.
    CheckNeighbor("discarded-selected-offset", fixture, {}, raw, -1, false, true);
  }
  if (enabled("neighbor-two-fetch-offset")) {
    auto fixture = MakeFixture(Case::Canonical);
    std::vector<u32> first_offset;
    Vop3(first_offset, 0x30fu, 0, 18u, Vgpr(0));
    Export(first_offset, 0x22u, 0, 0, 0, 0);
    InsertNeighbor(fixture, fixture.fetch_pc / 4u, first_offset);
    fixture.code.pop_back();
    Vop3(fixture.code, 0x101u, 0, Vgpr(8), Vgpr(5), 193u);
    Export(fixture.code, 0x24u, 0, 0, 0, 0);
    const auto second_pc = static_cast<u32>(fixture.code.size() * sizeof(u32));
    fixture.code.push_back((0x38u << 26u) | (3u << 18u) | (1u << 13u));
    fixture.code.push_back((128u << 24u) | (5u << 16u) | (13u << 8u));
    Export(fixture.code, 0x23u, 13, 14, 15, 16);
    fixture.code.push_back(0xbf810000u);
    CheckTwoFetchOffsetNeighbor(fixture, second_pc);
  }
  Require("neighbors", "arguments", matched, "unknown neighbor case");
}
} // namespace
} // namespace Libs::Graphics
