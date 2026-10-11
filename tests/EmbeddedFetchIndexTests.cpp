#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace Libs::Graphics {
namespace {
namespace IR = ShaderRecompiler::IR;
using u32 = uint32_t;

void Require(const char* name, const char* stage, bool ok, const char* reason) {
  if (!ok) {
    std::fprintf(stderr, "EmbeddedFetchIndexTests: %s failed at %s: %s\n",
                 name, stage, reason);
    std::abort();
  }
}

constexpr u32 Vgpr(u32 reg) { return 256u + reg; }
constexpr u32 Inline(u32 value) { return 128u + value; }
constexpr u32 Vop2(u32 opcode, u32 dst, u32 src0, u32 src1) {
  return (opcode << 25u) | (dst << 17u) | (src1 << 9u) | src0;
}
constexpr u32 Vmov(u32 dst, u32 src) {
  return (0x3fu << 25u) | (dst << 17u) | (1u << 9u) | src;
}
void Vop3(std::vector<u32>& code, u32 opcode, u32 dst,
          u32 src0, u32 src1, u32 src2 = 0) {
  code.push_back((0x35u << 26u) | (opcode << 16u) | dst);
  code.push_back(src0 | (src1 << 9u) | (src2 << 18u));
}
void Export(std::vector<u32>& code, u32 target,
            u32 x, u32 y, u32 z, u32 w) {
  code.push_back((0x3eu << 26u) | (target << 4u) | 0xfu | (1u << 11u));
  code.push_back(x | (y << 8u) | (z << 16u) | (w << 24u));
}

enum class Case {
  Canonical, VertexOffset, InstanceOffset, Local,
  MaskDestination, Overwrite, NoncanonicalCndmask, AbiInstanceShift, AbiVertexMask
};
struct Fixture {
  std::vector<u32> code;
  u32 fetch_pc = 0;
  ShaderType stage = ShaderType::Vertex;
};

// Wholly synthetic ISA, independent of proprietary shader captures. The guest
// fetch and raw/high VertexID exports remain live through public translation.
Fixture MakeFixture(Case which) {
  Fixture fixture;
  const bool local = which == Case::Local;
  fixture.stage = local ? ShaderType::Local : ShaderType::Vertex;
  const u32 vertex = local ? 2u : 5u;
  const u32 instance = local ? 5u : 8u;
  auto& code = fixture.code;
  code.push_back(0xbe800380u); // s_mov_b32 s0,0: SMEM offset
  code.push_back((0x3du << 26u) | (2u << 18u) | (20u << 6u) | 4u);
  code.push_back(0u); // s_load_dwordx4 s[20:23],s[8:9],0
  code.push_back(Vmov(30, Vgpr(vertex)));
  code.push_back(Vop2(0x16u, 31, Inline(16), vertex));
  if (which == Case::VertexOffset)
    Vop3(code, 0x15du, vertex, 18u, Inline(0), Vgpr(vertex));
  if (which == Case::InstanceOffset)
    Vop3(code, 0x15du, instance, 18u, Inline(0), Vgpr(instance));
  if (which == Case::AbiInstanceShift)
    code.push_back(Vop2(0x16u, instance, Inline(16), vertex));
  if (which == Case::AbiVertexMask) {
    code.push_back(Vop2(0x1bu, vertex, 255u, vertex));
    code.push_back(0xffffu);
  }
  // All-one literal mask selects raw vertex; zero selects the modified ABI
  // instance for the high-half case. Detector sees the canonical v8/v5 pair.
  Vop3(code, 0x101u, 0, Vgpr(instance), Vgpr(vertex),
       which == Case::AbiInstanceShift ? Inline(0) : 193u);
  if (which == Case::MaskDestination) {
    code.push_back(Vop2(0x1bu, 0, 255u, 0));
    code.push_back(0xffffu);
  } else if (which == Case::Overwrite) {
    code.push_back(Vmov(0, Inline(3)));
  } else if (which == Case::NoncanonicalCndmask) {
    code.push_back(Vmov(32, Inline(3)));
    Vop3(code, 0x101u, 0, Vgpr(instance), Vgpr(32), 193u);
  }
  fixture.fetch_pc = static_cast<u32>(code.size() * sizeof(u32));
  code.push_back((0x38u << 26u) | (3u << 18u) | (1u << 13u));
  code.push_back((128u << 24u) | (5u << 16u) | (9u << 8u));
  // buffer_load_format_xyzw v[9:12],v0,s[20:23],0 idxen
  Export(code, 0x20u, 9, 10, 11, 12);
  Export(code, 0x21u, 30, 31, 30, 31);
  code.push_back(local ? 0xbe802006u : 0xbf810000u);
  return fixture;
}

ShaderRecompiler::TranslateResult Translate(const char* name, const Fixture& fixture) {
  ShaderVertexInputInfo vertex{};
  vertex.logical_stage = fixture.stage;
  vertex.fetch_embedded = true;
  vertex.fetch_buffer_reg = 0;
  vertex.fetch_attrib_reg = 2;
  vertex.resources_num = 1;
  vertex.resources[0].fields[3] =
      (static_cast<u32>(Prospero::BufferFormat::k32_32_32_32UInt) << 12u) |
      4u | (5u << 3u) | (6u << 6u) | (7u << 9u);
  vertex.resources_dst[0].attr_id = 0;
  vertex.resources_dst[0].register_start = 9;
  vertex.resources_dst[0].registers_num = 4;
  std::array<u32, 11> userdata{};
  userdata[10] = 7u;
  ShaderRecompiler::CompileOptions options;
  options.stage = fixture.stage;
  options.wave_size = 32;
  options.user_data_base = 8;
  options.user_data = userdata;
  options.input_info.vertex = &vertex;
  options.dump_ir = false;
  auto result = ShaderRecompiler::TranslateProgram(fixture.code, options);
  Require(name, "public translation", !result.skip_dispatch,
          "synthetic ISA/resource setup must translate before checking eligibility");
  return result;
}

// Evaluate only scalar expressions actually emitted into the public IR, with
// independent input values. This does not reproduce the fetch detector or
// resource allocator; memory/attribute reads are never fabricated here.
u32 Evaluate(const char* name, IR::Value value, u32 raw_vertex, unsigned depth = 0) {
  Require(name, "IR expression", depth < 128u, "unexpected cyclic expression");
  value = value.Resolve();
  if (value.IsImmediate()) {
    if (value.GetType() == IR::Type::U1) return value.U1() ? 1u : 0u;
    Require(name, "IR expression", value.GetType() == IR::Type::U32,
            "address/VertexID must use integer bits");
    return value.U32();
  }
  const auto* inst = value.Instruction();
  const auto arg = [&](size_t index) {
    return Evaluate(name, inst->Arg(index), raw_vertex, depth + 1u);
  };
  switch (inst->GetOpcode()) {
  case IR::ValueOpcode::GetBuiltin:
    if (inst->Arg(0).U32() == static_cast<u32>(IR::StageInputKind::VertexIndex))
      return raw_vertex;
    if (inst->Arg(0).U32() == static_cast<u32>(IR::StageInputKind::InstanceIndex))
      return 7u;
    break;
  case IR::ValueOpcode::LaneId: return 0u;
  case IR::ValueOpcode::BitwiseAnd32: return arg(0) & arg(1);
  case IR::ValueOpcode::BitwiseOr32: return arg(0) | arg(1);
  case IR::ValueOpcode::ShiftRightLogical32: return arg(0) >> (arg(1) & 31u);
  case IR::ValueOpcode::ShiftLeftLogical32: return arg(0) << (arg(1) & 31u);
  case IR::ValueOpcode::IAdd32: return arg(0) + arg(1);
  case IR::ValueOpcode::INotEqual32: return arg(0) != arg(1);
  case IR::ValueOpcode::IEqual32: return arg(0) == arg(1);
  case IR::ValueOpcode::ULessThan32: return arg(0) < arg(1);
  case IR::ValueOpcode::SelectU1:
  case IR::ValueOpcode::SelectU32: return arg(0) ? arg(1) : arg(2);
  default: break;
  }
  std::fprintf(stderr, "EmbeddedFetchIndexTests: unevaluable actual IR opcode %.*s\n",
               static_cast<int>(IR::ValueOpcodeName(inst->GetOpcode()).size()),
               IR::ValueOpcodeName(inst->GetOpcode()).data());
  Require(name, "IR expression", false, "unexpected address/VertexID expression");
  return 0;
}

IR::Value ExportComponent(const char* name, const IR::Program& program,
                         u32 parameter, u32 component) {
  for (const auto* block : program.blocks) {
    for (const auto& inst : *block) {
      if (inst.GetOpcode() != IR::ValueOpcode::SetAttribute) continue;
      const auto flags = inst.Flags<IR::ExportFlags>();
      Require(name, "export setup", flags.index < program.export_info.size(),
              "export metadata is missing");
      const auto& info = program.export_info[flags.index];
      if (info.kind != IR::ExportTargetKind::Parameter || info.index != parameter) continue;
      const auto* vector = inst.Arg(0).ResolveInstruction();
      Require(name, "export setup", vector != nullptr &&
                  vector->GetOpcode() == IR::ValueOpcode::CompositeConstructU32x4,
              "expected live four-component guest export");
      return vector->Arg(component);
    }
  }
  Require(name, "export setup", false, "required guest export was eliminated");
  return {};
}

void CheckCase(const char* name, Case which, bool native, u32 expected_index = 0) {
  const auto fixture = MakeFixture(which);
  auto result = Translate(name, fixture);
  const auto& program = result.program;
  constexpr u32 raw = 0x001d0001u;
  Require(name, "raw VertexID", Evaluate(name, ExportComponent(name, program, 1, 0), raw) == raw &&
              Evaluate(name, ExportComponent(name, program, 1, 1), raw) == 29u,
          "raw VertexID/high metadata must retain all guest bits independently of fetch");
  size_t attributes = 0;
  const IR::Inst* fetch = nullptr;
  for (const auto* block : program.blocks) {
    for (const auto& inst : *block) {
      if (inst.GetOpcode() == IR::ValueOpcode::GetAttribute) ++attributes;
      if (IR::BufferAccessOf(inst.GetOpcode()) != IR::BufferAccess::Read) continue;
      if (inst.Flags<IR::MemoryFlags>().pc == fixture.fetch_pc) fetch = &inst;
    }
  }
  std::printf("EmbeddedFetchIndexTests: %s raw=%08x high=%u native_attributes=%zu shader_fetch=%u\n",
              name, raw, 29u, attributes, fetch != nullptr ? 1u : 0u);
  if (native) {
    Require(name, "canonical positive control", attributes == 4u && fetch == nullptr,
            "unmodified supported fetch must remain native-attribute eligible");
    if (which == Case::VertexOffset)
      Require(name, "draw vertex offset", program.info.vertex_offset_sgpr == 18,
              "supported vertex-offset prolog must retain its draw metadata");
    if (which == Case::InstanceOffset)
      Require(name, "draw instance offset", program.info.instance_offset_sgpr == 18,
              "supported instance-offset prolog must retain its draw metadata");
  } else {
    Require(name, "transformed fetch eligibility", attributes == 0u && fetch != nullptr,
            "native fixed-function input cannot represent this guest index expression");
    Require(name, "guest address", Evaluate(name, fetch->Arg(1), raw) == expected_index,
            "retained shader buffer load must consume the guest-computed index");
    // Independent numerical checks across high-bit and ordinary raw indices.
    if (which == Case::MaskDestination || which == Case::AbiVertexMask) {
      for (const u32 input : {1u, 0x00010001u, 0x001d0001u, 0x7fff0001u})
        Require(name, "mask input variations", Evaluate(name, fetch->Arg(1), input) == 1u,
                "masking must apply to the fetch expression while raw ABI remains intact");
    }
  }
  std::printf("EmbeddedFetchIndexTests: %s PASS\n", name);
}

void Controls() {
  CheckCase("canonical", Case::Canonical, true);
  CheckCase("vertex-offset", Case::VertexOffset, true);
  CheckCase("instance-offset", Case::InstanceOffset, true);
  CheckCase("local", Case::Local, true);
}
} // namespace
} // namespace Libs::Graphics

#include "EmbeddedFetchIndexNeighborTests.inc"

int main(int argc, char** argv) {
  using namespace Libs::Graphics;
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  Common::InitializeThreads();
  Common::Subsystems subsystems;
  subsystems.Initialize<Config::Lifecycle>();
  Config::ConfigOptions options;
  options.printf_direction = Config::LogDirection::Silent;
  Config::Load(options);
  subsystems.Initialize<Log::Lifecycle>();
  ShaderInit();
  Require("arguments", "usage", argc == 1 || argc == 2,
          "embedded_fetch_index_tests [controls|mask-dst|overwrite|cndmask-overwrite|abi-instance-shift|abi-vertex-mask]");
  Controls();
  if (argc == 2 && std::strcmp(argv[1], "controls") == 0) return 0;
  const std::string_view selected = argc == 1 ? "all" : argv[1];
  if (selected == "neighbors" || selected.starts_with("neighbor-")) {
    CheckEmbeddedFetchIndexNeighbors(selected);
    return 0;
  }
  bool matched = false;
  const auto run = [&](const char* name, Case which, u32 expected) {
    if (selected == "all" || selected == name) {
      matched = true;
      CheckCase(name, which, false, expected);
    }
  };
  run("mask-dst", Case::MaskDestination, 1u);
  run("overwrite", Case::Overwrite, 3u);
  run("cndmask-overwrite", Case::NoncanonicalCndmask, 3u);
  run("abi-instance-shift", Case::AbiInstanceShift, 29u);
  run("abi-vertex-mask", Case::AbiVertexMask, 1u);
  Require("arguments", "case", matched, "unknown regression case");
  return 0;
}
