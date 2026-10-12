// Test-only proposal. Include directly AFTER GenericBufferCodegenTests.inc
// in BindlessLifecycleTests.cpp. Uses its real public GenericCompilation path.
// No production API hooks, native execution, golden changes or tracked edits.
#include <map>
#include <optional>
#include <spirv/unified1/GLSL.std.450.h>
#include <unordered_map>

namespace Libs::Graphics {
namespace {
struct GenericPathInstruction {
  spv::Op op{};
  std::vector<uint32_t> args;
};
struct GenericPathFunction {
  std::vector<uint32_t> params;
  std::map<uint32_t, std::vector<GenericPathInstruction>> blocks;
  uint32_t entry = 0;
};
struct GenericPathCost {
  uint32_t divides = 0, unsigned_float = 0, signed_float = 0;
  uint32_t half_unpack = 0, sign_extract = 0;
};

// Bounded traversal of the ACTUAL emitted conversion helper with a legal
// format's uniform parameter values. It counts work on the taken CFG path,
// not all static opcodes: a helper may retain every format for other callers.
// Unknown branch conditions are a fixture setup error, not a successful RED.
GenericPathCost GenericConversionPathCost(const std::vector<uint32_t>& words,
                                          const std::array<uint32_t, 10>& arguments) {
  constexpr const char* name = "GenericConversionPathCost";
  std::unordered_map<uint32_t, uint32_t> constants;
  std::map<uint32_t, GenericPathFunction> functions;
  uint32_t function = 0, block = 0;
  for (size_t p = 5; p < words.size();) {
    const auto count = words[p] >> 16;
    const auto op = static_cast<spv::Op>(words[p] & 0xffffu);
    Require(name, "module framing setup", count != 0 && count <= words.size() - p,
            "invalid real SPIR-V framing; not a path-cost RED");
    std::vector<uint32_t> args(words.begin() + p + 1, words.begin() + p + count);
    if (op == spv::OpConstant && args.size() == 3)
      constants[args[1]] = args[2];
    else if ((op == spv::OpConstantTrue || op == spv::OpConstantFalse) && args.size() == 2)
      constants[args[1]] = op == spv::OpConstantTrue ? 1u : 0u;
    else if (op == spv::OpFunction) {
      Require(name, "function framing setup", args.size() == 4,
              "invalid function framing; not a path-cost RED");
      function = args[1];
      block = 0;
      functions.emplace(function, GenericPathFunction{});
    } else if (op == spv::OpFunctionParameter) {
      Require(name, "parameter framing setup", function != 0 && args.size() == 2,
              "invalid function parameter; not a path-cost RED");
      functions.at(function).params.push_back(args[1]);
    } else if (op == spv::OpLabel) {
      Require(name, "block framing setup", function != 0 && args.size() == 1,
              "invalid function block; not a path-cost RED");
      block = args[0];
      auto& fn = functions.at(function);
      if (fn.entry == 0) fn.entry = block;
      fn.blocks.emplace(block, std::vector<GenericPathInstruction>{});
    } else if (op == spv::OpFunctionEnd) {
      function = block = 0;
    } else if (function != 0 && block != 0) {
      functions.at(function).blocks.at(block).push_back({op, std::move(args)});
    }
    p += count;
  }
  const GenericPathFunction* helper = nullptr;
  uint32_t matches = 0;
  for (const auto& [id, fn] : functions) {
    (void)id;
    if (fn.params.size() != arguments.size()) continue;
    bool division = false;
    for (const auto& [label, instructions] : fn.blocks) {
      (void)label;
      for (const auto& inst : instructions) division |= inst.op == spv::OpFDiv;
    }
    if (division) { helper = &fn; ++matches; }
  }
  Require(name, "actual shared helper setup", matches == 1 && helper != nullptr,
          "fixture did not find the real ten-parameter formatted conversion helper; not a cost RED");
  auto values = constants;
  for (size_t i = 0; i < arguments.size(); ++i) values[helper->params[i]] = arguments[i];
  const auto known = [&](uint32_t id) -> std::optional<uint32_t> {
    const auto it = values.find(id);
    return it == values.end() ? std::nullopt : std::optional<uint32_t>{it->second};
  };
  GenericPathCost cost;
  uint32_t current = helper->entry, previous = 0;
  for (uint32_t steps = 0; steps < 128; ++steps) {
    const auto found = helper->blocks.find(current);
    Require(name, "reachable block setup", found != helper->blocks.end(),
            "taken path targets an absent block; not a cost RED");
    bool jumped = false;
    for (const auto& inst : found->second) {
      const auto& a = inst.args;
      cost.divides += inst.op == spv::OpFDiv;
      cost.unsigned_float += inst.op == spv::OpConvertUToF;
      cost.signed_float += inst.op == spv::OpConvertSToF;
      cost.sign_extract += inst.op == spv::OpBitFieldSExtract;
      cost.half_unpack += inst.op == spv::OpExtInst && a.size() >= 4 &&
                          a[3] == GLSLstd450UnpackHalf2x16;
      if (inst.op == spv::OpReturnValue) return cost;
      if (inst.op == spv::OpBranch || inst.op == spv::OpBranchConditional) {
        uint32_t target = 0;
        if (inst.op == spv::OpBranch) target = a.at(0);
        else {
          const auto condition = known(a.at(0));
          Require(name, "format branch setup", condition.has_value(),
                  "format path condition cannot be evaluated; not a cost RED");
          target = a.at(*condition ? 1 : 2);
        }
        previous = current;
        current = target;
        jumped = true;
        break;
      }
      if (inst.op == spv::OpPhi) {
        for (size_t i = 2; i + 1 < a.size(); i += 2)
          if (a[i + 1] == previous) {
            if (const auto value = known(a[i])) values[a[1]] = *value;
            else values.erase(a[1]);
          }
      } else if ((inst.op == spv::OpCopyObject || inst.op == spv::OpBitcast) && a.size() == 3) {
        if (const auto value = known(a[2])) values[a[1]] = *value;
      } else if (inst.op == spv::OpLogicalNot && a.size() == 3) {
        if (const auto value = known(a[2])) values[a[1]] = !*value;
      } else if ((inst.op == spv::OpLogicalAnd || inst.op == spv::OpLogicalOr ||
                  inst.op == spv::OpIEqual || inst.op == spv::OpINotEqual) && a.size() == 4) {
        const auto lhs = known(a[2]), rhs = known(a[3]);
        if (lhs && rhs) values[a[1]] = inst.op == spv::OpLogicalAnd ? (*lhs && *rhs)
          : inst.op == spv::OpLogicalOr ? (*lhs || *rhs)
          : inst.op == spv::OpIEqual ? (*lhs == *rhs) : (*lhs != *rhs);
      } else if (inst.op == spv::OpSelect && a.size() == 5) {
        if (const auto condition = known(a[2]))
          if (const auto value = known(a[*condition ? 3 : 4])) values[a[1]] = *value;
      }
    }
    Require(name, "terminated helper block setup", jumped,
            "helper block has no bounded return/branch; not a cost RED");
  }
  Require(name, "bounded path setup", false,
          "helper did not return within 128 blocks; not a cost RED");
  return cost;
}

void CheckGenericFormattedConversionPathCost(VulkanHarness&) {
  GenericCompilerProfile();
  FetchGpuGuest guest;
  GenericCompilation shader(1, 32);
  constexpr const char* name = "GenericFormattedConversionPathCost";
  struct Case {
    const char* label;
    uint32_t bits, flag, divisions, unsigned_float, signed_float, half, sign;
  };
  // Flag indices are the actual helper ABI: half, SInt, UScaled, SScaled,
  // UNorm, SNorm. All zero flags represent UInt or non-half Float raw bits.
  constexpr std::array cases{
    Case{"UInt32", 32, 0, 0, 0, 0, 0, 0},
    Case{"Float32", 32, 0, 0, 0, 0, 0, 0},
    Case{"UInt8", 8, 0, 0, 0, 0, 0, 0},
    Case{"Float16", 16, 2, 0, 0, 0, 1, 0},
    Case{"SInt8", 8, 3, 0, 0, 0, 0, 1},
    Case{"UScaled16", 16, 4, 0, 1, 0, 0, 0},
    Case{"SScaled16", 16, 5, 0, 0, 1, 0, 1},
    Case{"UNorm8", 8, 6, 1, 1, 0, 0, 0},
    Case{"SNorm8", 8, 7, 1, 0, 1, 0, 1},
  };
  for (const auto& test : cases) {
    std::array<uint32_t, 10> params{};
    params[0] = 0x80u;
    params[1] = test.bits;
    if (test.flag != 0) params[test.flag] = 1;
    // Denominators are not evaluated by this path-cost walker. Real runtime
    // numerical tests continue to supply independent actual descriptor data.
    params[8] = 0x437f0000u; // 255.0f
    params[9] = 0x42fe0000u; // 127.0f
    const auto cost = GenericConversionPathCost(shader.compiled.spirv, params);
    std::printf("[path-cost] %s FDiv=%u UToF=%u SToF=%u half=%u sign=%u\n",
                test.label, cost.divides, cost.unsigned_float, cost.signed_float,
                cost.half_unpack, cost.sign_extract);
    Require(name, test.label,
      cost.divides <= test.divisions && cost.unsigned_float <= test.unsigned_float &&
      cost.signed_float <= test.signed_float && cost.half_unpack <= test.half &&
      cost.sign_extract <= test.sign,
      "selected legal format path evaluates unnecessary conversions of other formats");
  }
}
} // namespace
} // namespace Libs::Graphics
