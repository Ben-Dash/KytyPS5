// CPU-only diagnostic admission fixtures: actual flattened SRT/SRD metadata, no Vulkan setup.
#include "graphics/host_gpu/renderer/dispatchSnapshot.h"

namespace Libs::Graphics {
namespace {
void CheckDispatchSnapshotJobSelector(bool frame_mode = false, bool default_mode = false) {
  constexpr const char* name = "DispatchSnapshotJobSelector";
  using DispatchSnapshot::JobRangeSelector;
  using DispatchSnapshot::ParseJobRangeSelector;
  using DispatchSnapshot::MatchesJobRangeSelector;
  JobRangeSelector selector{7u, 9u, 11u, 13u};
  Require(name, "strict decimal parser",
      ParseJobRangeSelector("23:196:2097:2", selector) &&
      selector.count_dword == 23u && selector.base_dword == 196u &&
      selector.first_index == 2097u && selector.required_count == 2u,
      "valid four-field job selector rejected");
  Require(name, "representable integer boundaries",
      ParseJobRangeSelector("4294967295:4294967295:4294967295:1", selector) &&
      selector.count_dword == UINT32_MAX && selector.base_dword == UINT32_MAX &&
      selector.first_index == UINT32_MAX && selector.required_count == 1u,
      "typed maximum or exclusive end exactly2^32 rejected");
  Require(name, "large interval and leading zeros",
      ParseJobRangeSelector("00:01:01:4294967295", selector) &&
      selector.count_dword == 0u && selector.base_dword == 1u &&
      selector.first_index == 1u && selector.required_count == UINT32_MAX,
      "zero word index, leading zeros or wide nonwrapping interval rejected");
  constexpr std::string_view invalid[] = {
      "", "23:196:2097", "23:196:2097:2:3", ":196:2097:2", "17::2097:2",
      "23:196::2", "23:196:2097:", "23:196:2097:0", "23:196:2097:000",
      "-1:196:2097:2", "+1:196:2097:2", "23:196:-1:2", "23:196:2097:+2",
      " 23:196:2097:2", "17: 196:2097:2", "23:196:2097:2 ", "23:196:2097:2x",
      "0x11:196:2097:2", "23:196:0x831:2", "4294967296:196:2097:2",
      "17:4294967296:2097:2", "23:196:4294967296:2", "23:196:2097:4294967296",
      "23:196:4294967295:2", "23:196:2:4294967295",
      "999999999999999999999999:1:1:1"};
  for (const auto text : invalid) {
    selector = {7u, 9u, 11u, 13u};
    Require(name, "malformed/overflow parser",
        !ParseJobRangeSelector(text, selector) && selector.count_dword == 7u &&
        selector.base_dword == 9u && selector.first_index == 11u &&
        selector.required_count == 13u,
        "malformed selector accepted or failure changed output");
  }

  ShaderRecompiler::IR::CompiledShaderInfo program{};
  program.stage = ShaderType::Compute;
  program.shader_hash = 0xf00du; // Synthetic selector label, no guest shader bytes.
  program.info.buffers.resize(3u);
  program.info.buffers[2].written = true;
  ShaderRecompiler::IR::ResourceSnapshot resources{};
  resources.buffers.resize(3u);
  resources.flattened_srt.resize(197u);
  ShaderComputeInputInfo input{};
  input.stage = {&program, &resources};
  input.threads_num[0] = 64u;
  input.threads_num[1] = input.threads_num[2] = 1u;
  const std::array<uint32_t, 3> groups{1u, 1u, 1u};
  selector = {23u, 196u, 2097u, 2u};
  const auto set_job = [&](uint32_t count, uint32_t base) {
    resources.flattened_srt[23] = count;
    resources.flattened_srt[196] = base;
  };
  set_job(9u, 2090u); // Exact exclusive end2099 contains BOTH2097 and2098.
  Require(name, "entire target interval",
      MatchesJobRangeSelector(selector, input, groups), "fully covered two-index interval rejected");
  set_job(8u, 2090u);
  Require(name, "partial overlap is insufficient",
      !MatchesJobRangeSelector(selector, input, groups), "only first required index was admitted");
  set_job(64u, 2098u);
  Require(name, "late base misses interval start",
      !MatchesJobRangeSelector(selector, input, groups), "range missing first target index admitted");
  set_job(0u, 2090u);
  Require(name, "empty runtime job count", !MatchesJobRangeSelector(selector, input, groups),
      "empty count admitted");
  set_job(10u, 2090u);
  input.threads_num[0] = 8u;
  Require(name, "logical capacity limits flat count",
      !MatchesJobRangeSelector(selector, input, groups), "flat count bypassed dispatch capacity");
  input.threads_num[0] = 4u;
  Require(name, "multiple groups widen actual capacity",
      MatchesJobRangeSelector(selector, input, {3u, 1u, 1u}), "known 1D groups ignored");
  input.threads_num[0] = 64u;
  Require(name, "unknown or multidimensional groups",
      !MatchesJobRangeSelector(selector, input, {0u, 1u, 1u}) &&
      !MatchesJobRangeSelector(selector, input, {1u, 0u, 1u}) &&
      !MatchesJobRangeSelector(selector, input, {1u, 1u, 0u}) &&
      !MatchesJobRangeSelector(selector, input, {1u, 2u, 1u}) &&
      !MatchesJobRangeSelector(selector, input, {1u, 1u, 2u}),
      "unknown or non1D dispatch admitted");
  input.threads_num[0] = 0u;
  Require(name, "zero thread shape", !MatchesJobRangeSelector(selector, input, groups),
      "unknown thread count admitted");
  input.threads_num[0] = 64u;
  input.threads_num[1] = 2u;
  Require(name, "multidimensional threads", !MatchesJobRangeSelector(selector, input, groups),
      "2D threads admitted");
  input.threads_num[1] = 1u;
  input.threads_num[2] = 0u;
  Require(name, "unknown thread shape", !MatchesJobRangeSelector(selector, input, groups),
      "unknown Z shape admitted");
  input.threads_num[2] = 1u;
  program.stage = ShaderType::Vertex;
  Require(name, "actual compute stage required", !MatchesJobRangeSelector(selector, input, groups),
      "non-compute runtime admitted");
  program.stage = ShaderType::Compute;
  const auto saved_stage = input.stage;
  input.stage = {};
  Require(name, "missing runtime", !MatchesJobRangeSelector(selector, input, groups),
      "missing runtime admitted");
  input.stage = saved_stage;
  Require(name, "flat word bounds and malformed explicit selector",
      !MatchesJobRangeSelector({UINT32_MAX, 196u, 2097u, 2u}, input, groups) &&
      !MatchesJobRangeSelector({17u, UINT32_MAX, 2097u, 2u}, input, groups) &&
      !MatchesJobRangeSelector({23u, 196u, 2097u, 0u}, input, groups) &&
      !MatchesJobRangeSelector({23u, 196u, UINT32_MAX, 2u}, input, groups),
      "missing flat values or malformed explicit selector admitted");

  input.threads_num[0] = 65536u;
  set_job(UINT32_MAX, 1u);
  Require(name, "exact2^32 capacity and widened interval arithmetic",
      MatchesJobRangeSelector({23u, 196u, UINT32_MAX, 1u}, input, {65536u, 1u, 1u}),
      "nonwrapping maximum target/capacity rejected");
  Require(name, "capacity beyond U32 space rejected",
      !MatchesJobRangeSelector({23u, 196u, UINT32_MAX, 1u}, input, {65537u, 1u, 1u}),
      "oversized capacity admitted");
  input.threads_num[0] = UINT32_MAX;
  Require(name, "large product is widened not wrapped",
      !MatchesJobRangeSelector({23u, 196u, 1u, 1u}, input, {UINT32_MAX, 1u, 1u}),
      "large U32 product wrapped into an admitted capacity");
  input.threads_num[0] = 1u;
  set_job(1u, UINT32_MAX);
  Require(name, "last valid index included",
      MatchesJobRangeSelector({23u, 196u, UINT32_MAX, 1u}, input, groups),
      "exclusive end exactly2^32 lost last index");
  input.threads_num[0] = 2u;
  set_job(2u, UINT32_MAX);
  Require(name, "runtime base plus count must not wrap",
      !MatchesJobRangeSelector({23u, 196u, UINT32_MAX, 1u}, input, groups) &&
      !MatchesJobRangeSelector({23u, 196u, 0u, 1u}, input, groups),
      "wrapped job range admitted");

  const auto set_descriptor = [&](uint32_t count) {
    ShaderBufferResource descriptor{};
    descriptor.UpdateAddress48(0x0000000200000000ull);
    descriptor.fields[1] |= 48u << 16u;
    descriptor.fields[2] = count;
    resources.buffers[2].dword_count = 4u;
    std::copy_n(descriptor.fields, 4u, resources.buffers[2].dwords.begin());
  };
  input.threads_num[0] = 64u;
  _putenv_s("KYTY_DISPATCH_SNAPSHOT", frame_mode ? "frame:10" : "f00d");
  _putenv_s("KYTY_DISPATCH_SNAPSHOT_MAX", "1");
  _putenv_s("KYTY_DISPATCH_SNAPSHOT_RING", "0");
  _putenv_s("KYTY_DISPATCH_SNAPSHOT_WRITTEN_BUFFER", "2:124224");
  _putenv_s("KYTY_DISPATCH_SNAPSHOT_JOB_RANGE", default_mode ? "" : "23:196:2097:2");
  set_descriptor(2588u);
  set_job(349u, 2239u); // Same-sized real metadata shape, independently wrong job interval.
  const bool legacy = default_mode ? false : DispatchSnapshot::Wanted(input.stage, 10u);
  const bool unknown = default_mode ? false : DispatchSnapshot::Wanted(input, {0u, 0u, 0u}, 10u);
  const bool wrong_job = DispatchSnapshot::Wanted(input, groups, 10u);
  set_job(12u, 2090u);
  set_descriptor(12063u); // Job matches, but composed written SRD span does not.
  const bool wrong_span = DispatchSnapshot::Wanted(input, groups, 10u);
  set_descriptor(2588u);
  const bool first = DispatchSnapshot::Wanted(input, groups, frame_mode ? 11u : 10u);
  const bool second = DispatchSnapshot::Wanted(input.stage, frame_mode ? 11u : 10u);
  const bool repeated = DispatchSnapshot::Wanted(input, groups, frame_mode ? 11u : 10u);
  Require(name, "actual composed quota/frame admission",
      !legacy && !unknown && !wrong_span && !second && !repeated &&
      (default_mode ? (wrong_job && !first) : (!wrong_job && first)),
      "rejected job/span consumed frame/quota or default/overload quota changed");
  std::printf("[cpu] DispatchSnapshotJobSelector parser=%zu malformed cases; frame=%u default=%u legacy=%u unknown=%u wrong_job=%u wrong_span=%u first=%u repeated=%u; GPU_setup=0\n",
      std::size(invalid), frame_mode ? 1u : 0u, default_mode ? 1u : 0u,
      legacy ? 1u : 0u, unknown ? 1u : 0u, wrong_job ? 1u : 0u,
      wrong_span ? 1u : 0u, first ? 1u : 0u, repeated ? 1u : 0u);
}
} // namespace
} // namespace Libs::Graphics
