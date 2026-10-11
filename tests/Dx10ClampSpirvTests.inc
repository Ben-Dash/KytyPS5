#include <bit>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <spirv-tools/libspirv.hpp>
#ifndef SPV_ENABLE_UTILITY_CODE
#define SPV_ENABLE_UTILITY_CODE
#define KYTY_DX10_TEST_SPIRV_UTILITY
#endif
#include <spirv/unified1/spirv.hpp>
#include <spirv/unified1/GLSL.std.450.h>
#ifdef KYTY_DX10_TEST_SPIRV_UTILITY
#undef SPV_ENABLE_UTILITY_CODE
#undef KYTY_DX10_TEST_SPIRV_UTILITY
#endif

namespace Libs::Graphics {
namespace {

// AMD RDNA2 ALU CLAMP / MODE.DX10_CLAMP and RDNA3's explicit CLAMP restriction:
// https://docs.amd.com/api/khub/documents/Et~wpu9g~Ffl7d9q0QZ~Og/content
// https://docs.amd.com/api/khub/documents/UkT_UPQL21KfKAMUBFnZTw/content
// FMin/FMax may select a NaN operand; FClamp inherits that permitted behavior:
// https://registry.khronos.org/SPIR-V/specs/unified1/GLSL.std.450.html
// This is a CPU portability oracle, not a report of NVIDIA/GPU execution.
class Dx10SpirvEvaluator {
  struct Instruction { spv::Op op; std::vector<uint32_t> words; };
  using Value = std::vector<uint32_t>;
  std::vector<Instruction> instructions;
  std::map<uint32_t, size_t> definitions, labels;
  std::map<uint32_t, Value> values, memory;
  std::map<uint32_t, uint32_t> builtins;
  std::set<uint32_t> evaluating;
  uint32_t entry = 0, output = 0, glsl = 0, predecessor = 0;

  [[noreturn]] static void Unsupported(const std::string& reason) {
    throw std::runtime_error("SPIR-V fixture/evaluator unsupported: " + reason);
  }
  static bool IsNan(uint32_t bits) {
    return (bits & 0x7fffffffu) > 0x7f800000u;
  }
  static uint32_t FloatBits(float value) { return std::bit_cast<uint32_t>(value); }
  static float Float(uint32_t bits) { return std::bit_cast<float>(bits); }
  void CheckFloat32Type(uint32_t type_id) const {
    const auto& type = instructions.at(definitions.at(type_id));
    if (type.op == spv::OpTypeVector) {
      if (type.words.at(2) < 2u || type.words.at(2) > 4u) Unsupported("float vector width");
      CheckFloat32Type(type.words.at(1));
    } else if (type.op != spv::OpTypeFloat || type.words.at(1) != 32u)
      Unsupported("floating-point operation is not FP32");
  }
  static uint32_t MinMax(uint32_t a, uint32_t b, bool maximum, bool numeric) {
    // One explicitly permitted conforming host: ordinary FMin/FMax propagate
    // NaNs. NMin/NMax instead prefer the numeric operand as their spec requires.
    if (IsNan(a)) return numeric && !IsNan(b) ? b : a;
    if (IsNan(b)) return numeric ? a : b;
    if ((a & 0x7fffffffu) == 0 && (b & 0x7fffffffu) == 0)
      return maximum ? a & b : a | b;
    return (maximum ? Float(a) < Float(b) : Float(b) < Float(a)) ? b : a;
  }
  Value Evaluate(uint32_t id, unsigned depth = 0) {
    if (depth > 96u) Unsupported("value depth bound");
    if (const auto found = values.find(id); found != values.end()) return found->second;
    if (!evaluating.insert(id).second) Unsupported("cyclic value graph");
    const auto found = definitions.find(id);
    if (found == definitions.end()) Unsupported("missing value id " + std::to_string(id));
    const auto& inst = instructions.at(found->second);
    const auto& w = inst.words;
    const auto arg = [&](size_t i) { return Evaluate(w.at(i), depth + 1u); };
    const auto scalar = [&](size_t i) {
      const auto value = arg(i);
      if (value.size() != 1u) Unsupported("non-scalar control/index value");
      return value[0];
    };
    const auto binary = [&](auto operation) {
      const auto a = arg(2), b = arg(3);
      if (a.size() != b.size()) Unsupported("binary component count");
      Value result;
      for (size_t i = 0; i < a.size(); ++i) result.push_back(operation(a[i], b[i]));
      return result;
    };
    Value result;
    switch (inst.op) {
    case spv::OpConstant: result.assign(w.begin() + 2, w.end()); break;
    case spv::OpConstantTrue: result = {1u}; break;
    case spv::OpConstantFalse: result = {0u}; break;
    case spv::OpConstantNull: {
      const auto& type = instructions.at(definitions.at(w.at(0)));
      if (type.op == spv::OpTypeVector) result.assign(type.words.at(2), 0u);
      else if (type.op == spv::OpTypeBool || type.op == spv::OpTypeInt ||
               type.op == spv::OpTypeFloat) result = {0u};
      else Unsupported("null value type");
      break;
    }
    case spv::OpConstantComposite:
    case spv::OpCompositeConstruct:
      for (size_t i = 2; i < w.size(); ++i) {
        const auto component = arg(i);
        result.insert(result.end(), component.begin(), component.end());
      }
      break;
    case spv::OpCompositeExtract: {
      const auto composite = arg(2);
      if (w.size() != 4u || w[3] >= composite.size()) Unsupported("nested composite extract");
      result = {composite[w[3]]}; break;
    }
    case spv::OpVectorShuffle: {
      result = arg(2);
      const auto other = arg(3);
      result.insert(result.end(), other.begin(), other.end());
      const auto source = result;
      result.clear();
      for (size_t i = 4; i < w.size(); ++i) result.push_back(source.at(w[i]));
      break;
    }
    case spv::OpBitcast:
    case spv::OpCopyObject: result = arg(2); break;
    case spv::OpLoad: {
      const auto variable = w.at(2);
      if (const auto stored = memory.find(variable); stored != memory.end()) result = stored->second;
      else if (const auto builtin = builtins.find(variable); builtin != builtins.end() &&
               (builtin->second == spv::BuiltInSubgroupLocalInvocationId ||
                builtin->second == spv::BuiltInVertexIndex ||
                builtin->second == spv::BuiltInInstanceIndex)) result = {0u}; // explicit invocation0
      else Unsupported("uninitialized or unsupported load");
      break;
    }
    case spv::OpPhi:
      for (size_t i = 2; i + 1u < w.size(); i += 2u)
        if (w[i + 1u] == predecessor) { result = arg(i); break; }
      if (result.empty()) Unsupported("unselected Phi predecessor");
      break;
    case spv::OpSelect: {
      const auto condition = arg(2);
      if (condition.size() == 1u) result = arg(condition[0] ? 3u : 4u);
      else {
        const auto yes = arg(3), no = arg(4);
        if (yes.size() != condition.size() || no.size() != condition.size())
          Unsupported("vector select component count");
        for (size_t i = 0; i < condition.size(); ++i)
          result.push_back(condition[i] ? yes[i] : no[i]);
      }
      break;
    }
    case spv::OpIsNan:
      result = arg(2); for (auto& component : result) component = IsNan(component); break;
    case spv::OpIsInf:
      result = arg(2); for (auto& component : result) component = (component & 0x7fffffffu) == 0x7f800000u; break;
    case spv::OpLogicalNot:
      result = arg(2); for (auto& component : result) component = !component; break;
    case spv::OpAny:
    case spv::OpAll: {
      const auto components = arg(2);
      bool value = inst.op == spv::OpAll;
      for (const auto component : components)
        value = inst.op == spv::OpAll ? value && component : value || component;
      result = {static_cast<uint32_t>(value)}; break;
    }
    case spv::OpFAdd:
      CheckFloat32Type(w.at(0)); result = binary([](auto a, auto b) { return FloatBits(Float(a) + Float(b)); }); break;
    case spv::OpFSub:
      CheckFloat32Type(w.at(0)); result = binary([](auto a, auto b) { return FloatBits(Float(a) - Float(b)); }); break;
    case spv::OpFMul:
      CheckFloat32Type(w.at(0)); result = binary([](auto a, auto b) { return FloatBits(Float(a) * Float(b)); }); break;
    case spv::OpFNegate:
      result = arg(2); for (auto& component : result) component ^= 0x80000000u; break;
    case spv::OpIAdd: result = binary([](auto a, auto b) { return a + b; }); break;
    case spv::OpBitwiseAnd: result = binary([](auto a, auto b) { return a & b; }); break;
    case spv::OpBitwiseOr: result = binary([](auto a, auto b) { return a | b; }); break;
    case spv::OpBitwiseXor: result = binary([](auto a, auto b) { return a ^ b; }); break;
    case spv::OpShiftRightLogical: result = binary([](auto a, auto b) { return a >> (b & 31u); }); break;
    case spv::OpShiftLeftLogical: result = binary([](auto a, auto b) { return a << (b & 31u); }); break;
    case spv::OpIEqual:
    case spv::OpLogicalEqual: result = binary([](auto a, auto b) { return uint32_t(a == b); }); break;
    case spv::OpINotEqual:
    case spv::OpLogicalNotEqual: result = binary([](auto a, auto b) { return uint32_t(a != b); }); break;
    case spv::OpLogicalAnd: result = binary([](auto a, auto b) { return uint32_t(a && b); }); break;
    case spv::OpLogicalOr: result = binary([](auto a, auto b) { return uint32_t(a || b); }); break;
    case spv::OpFOrdEqual: result = binary([](auto a, auto b) { return uint32_t(Float(a) == Float(b)); }); break;
    case spv::OpFOrdLessThan: result = binary([](auto a, auto b) { return uint32_t(Float(a) < Float(b)); }); break;
    case spv::OpFOrdGreaterThan: result = binary([](auto a, auto b) { return uint32_t(Float(a) > Float(b)); }); break;
    case spv::OpExtInst: {
      if (w.at(2) != glsl) Unsupported("non-GLSL extended instruction set");
      CheckFloat32Type(w.at(0));
      const auto operation = w.at(3);
      result = arg(4);
      if (operation == GLSLstd450FAbs) {
        for (auto& component : result) component &= 0x7fffffffu;
        break;
      }
      const auto b = arg(5);
      if (b.size() != result.size()) Unsupported("extended binary component count");
      const bool numeric = operation == GLSLstd450NMin || operation == GLSLstd450NMax ||
                           operation == GLSLstd450NClamp;
      for (size_t i = 0; i < result.size(); ++i) {
        if (operation == GLSLstd450FMin || operation == GLSLstd450NMin)
          result[i] = MinMax(result[i], b[i], false, numeric);
        else if (operation == GLSLstd450FMax || operation == GLSLstd450NMax)
          result[i] = MinMax(result[i], b[i], true, numeric);
        else if (operation == GLSLstd450FClamp || operation == GLSLstd450NClamp) {
          const auto c = arg(6);
          if (c.size() != result.size()) Unsupported("clamp component count");
          result[i] = MinMax(MinMax(result[i], b[i], true, numeric), c[i], false, numeric);
        } else Unsupported("extended opcode " + std::to_string(operation));
      }
      break;
    }
    default: Unsupported("value opcode " + std::to_string(static_cast<uint32_t>(inst.op)));
    }
    evaluating.erase(id);
    values[id] = result;
    return result;
  }

public:
  explicit Dx10SpirvEvaluator(std::span<const uint32_t> words) {
    if (words.size() < 5u || words.size() > 200000u || words[0] != spv::MagicNumber ||
        words[3] > 100000u) Unsupported("module header/bounds");
    for (size_t cursor = 5; cursor < words.size();) {
      const auto count = words[cursor] >> 16u;
      if (count == 0 || count > words.size() - cursor) Unsupported("instruction framing");
      Instruction inst{static_cast<spv::Op>(words[cursor] & 0xffffu),
          {words.begin() + cursor + 1u, words.begin() + cursor + count}};
      bool has_result = false, has_type = false;
      spv::HasResultAndType(inst.op, &has_result, &has_type);
      if (has_result && !definitions.emplace(inst.words.at(has_type ? 1u : 0u), instructions.size()).second)
        Unsupported("duplicate result id");
      if (inst.op == spv::OpLabel) labels[inst.words.at(0)] = instructions.size();
      if (inst.op == spv::OpEntryPoint) {
        if (entry != 0 || inst.words.at(0) != spv::ExecutionModelVertex)
          Unsupported("entry point profile");
        entry = inst.words.at(1);
      }
      if (inst.op == spv::OpExtInstImport) {
        const auto* name = reinterpret_cast<const char*>(inst.words.data() + 1u);
        if (std::string_view(name) == "GLSL.std.450") glsl = inst.words.at(0);
      }
      if (inst.op == spv::OpDecorate && inst.words.at(1) == spv::DecorationLocation &&
          inst.words.at(2) == 0u) output = inst.words.at(0);
      if (inst.op == spv::OpDecorate && inst.words.at(1) == spv::DecorationBuiltIn)
        builtins[inst.words.at(0)] = inst.words.at(2);
      instructions.push_back(std::move(inst));
      cursor += count;
    }
    if (entry == 0 || output == 0) Unsupported("required vertex/output identity");
    const auto& variable = instructions.at(definitions.at(output));
    if (variable.op != spv::OpVariable || variable.words.at(2) != spv::StorageClassOutput)
      Unsupported("Location0 is not the exported output");
  }

  uint32_t ExportedValue() {
    size_t cursor = definitions.at(entry) + 1u;
    uint32_t current_label = 0;
    std::set<uint32_t> visited;
    for (size_t steps = 0; steps < 4096u && cursor < instructions.size(); ++steps, ++cursor) {
      const auto& inst = instructions[cursor];
      const auto& w = inst.words;
      if (inst.op == spv::OpLabel) {
        current_label = w.at(0);
        if (!visited.insert(current_label).second) Unsupported("loop/backedge");
      } else if (inst.op == spv::OpBranch || inst.op == spv::OpBranchConditional) {
        uint32_t target = w.at(0);
        if (inst.op == spv::OpBranchConditional) {
          const auto condition = Evaluate(w.at(0));
          if (condition.size() != 1u) Unsupported("branch condition shape");
          target = w.at(condition[0] ? 1u : 2u);
        }
        predecessor = current_label;
        cursor = labels.at(target) - 1u;
      } else if (inst.op == spv::OpLoad || inst.op == spv::OpPhi) {
        (void)Evaluate(w.at(1));
      } else if (inst.op == spv::OpStore) {
        memory[w.at(0)] = Evaluate(w.at(1));
      } else if (inst.op == spv::OpVariable && w.size() > 3u) {
        memory[w.at(1)] = Evaluate(w.at(3));
      } else if (inst.op == spv::OpReturn) {
        const auto found = memory.find(output);
        if (found == memory.end() || found->second.size() != 4u)
          Unsupported("export not executed or not vec4");
        return found->second[0];
      } else if (inst.op == spv::OpLoopMerge || inst.op == spv::OpSwitch ||
                 inst.op == spv::OpFunctionCall || inst.op == spv::OpReturnValue ||
                 inst.op == spv::OpKill || inst.op == spv::OpUnreachable ||
                 inst.op == spv::OpFunctionEnd) Unsupported("unsupported entry control flow");
    }
    Unsupported("entry instruction bound/no return");
  }
};

uint32_t CompileDx10ClampExport(uint32_t opcode, uint32_t a, uint32_t b, bool clamp) {
  std::vector<uint32_t> code{Vmov(0u, 255u), a, Vmov(1u, 255u), b,
      (0x35u << 26u) | (opcode << 16u) | (clamp ? 1u << 15u : 0u) | 2u,
      Vgpr(0u) | (Vgpr(1u) << 9u)};
  Export(code, 0x20u, 2u, 2u, 2u, 2u);
  code.push_back(0xbf810000u);
  ShaderVertexInputInfo vertex{};
  vertex.logical_stage = ShaderType::Vertex;
  ShaderRecompiler::CompileOptions options;
  options.stage = ShaderType::Vertex;
  options.wave_size = 32u;
  options.input_info.vertex = &vertex;
  options.dump_ir = false;
  auto translated = ShaderRecompiler::TranslateProgram(code, options);
  if (translated.skip_dispatch) throw std::runtime_error("synthetic guest fixture did not translate");
  const auto plan = IR::ExtractResourcePlan(translated.program);
  IR::ResourceSnapshot resources;
  IR::ResourceSpecialization specialization;
  if (!IR::MaterializeResources(plan, {}, resources, specialization))
    throw std::runtime_error("resource-free guest fixture did not materialize");
  auto compiled = ShaderRecompiler::CompileProgram(std::move(translated), options, specialization);
  spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_3);
  validator.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t& pos,
                                  const char* message) {
    std::fprintf(stderr, "Dx10ClampSpirvTests: validation word%zu: %s\n", pos.index, message);
  });
  if (!validator.Validate(compiled.spirv)) throw std::runtime_error("actual compiled SPIR-V is invalid");
  return Dx10SpirvEvaluator(compiled.spirv).ExportedValue();
}

int RunDx10ClampSpirvTests() {
  struct Case { const char* name; uint32_t opcode, a, b; bool clamp; uint32_t expected; bool nan; };
  // The accepted graphics path is DX10_CLAMP1/IEEE0. Unclamped special
  // arithmetic keeps NaNs; only an actual destination CLAMP must turn them0.
  constexpr Case cases[] {
      {"add-clamp-zero", 0x103u, 0u, 0u, true, 0u, false},
      {"mul-clamp-half", 0x108u, 0x3f000000u, 0x3f800000u, true, 0x3f000000u, false},
      {"mul-clamp-one", 0x108u, 0x3f800000u, 0x3f800000u, true, 0x3f800000u, false},
      {"add-clamp-above-one", 0x103u, 0x3f800000u, 0x3f800000u, true, 0x3f800000u, false},
      {"add-clamp-negative", 0x103u, 0xbf800000u, 0u, true, 0u, false},
      {"add-clamp-positive-inf", 0x103u, 0x7f800000u, 0u, true, 0x3f800000u, false},
      {"add-clamp-negative-inf", 0x103u, 0xff800000u, 0u, true, 0u, false},
      {"mul-unclamped-qnan", 0x108u, 0x7fc12345u, 0x3f800000u, false, 0u, true},
      {"add-unclamped-qnan", 0x103u, 0x7fc12345u, 0u, false, 0u, true},
      {"mul-unclamped-zero-inf", 0x108u, 0u, 0x7f800000u, false, 0u, true},
      {"mul-clamp-qnan", 0x108u, 0x7fc12345u, 0x3f800000u, true, 0u, false},
      {"add-clamp-qnan", 0x103u, 0x7fc12345u, 0u, true, 0u, false},
      {"mul-clamp-snan", 0x108u, 0x7f812345u, 0x3f800000u, true, 0u, false},
      {"mul-clamp-zero-inf", 0x108u, 0u, 0x7f800000u, true, 0u, false},
  };
  try {
    std::printf("Dx10ClampSpirvTests: CPU portable host model=NaN-propagating FMin/FMax/FClamp; no GPU execution\n");
    for (const auto& test : cases) {
      const auto actual = CompileDx10ClampExport(test.opcode, test.a, test.b, test.clamp);
      const bool passed = test.nan ? (actual & 0x7fffffffu) > 0x7f800000u : actual == test.expected;
      std::printf("Dx10ClampSpirvTests: %s actual=%08x expected=%s%08x %s\n", test.name,
                  actual, test.nan ? "NaN-class/" : "", test.expected, passed ? "PASS" : "FAIL");
      if (!passed) return 1; // Intended numerical mismatch, after validation/evaluation.
    }
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Dx10ClampSpirvTests: fixture_error (not semantic RED): %s\n", error.what());
    return 2;
  }
}

} // namespace
} // namespace Libs::Graphics
