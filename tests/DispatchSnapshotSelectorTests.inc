// CPU-only diagnostic selection tests: actual runtime/SRD metadata, no guest reads or GPU setup.
#include "graphics/host_gpu/renderer/dispatchSnapshot.h"

namespace Libs::Graphics {
namespace {
void CheckDispatchSnapshotSelector(bool frame_mode = false, bool default_mode = false) {
  constexpr const char* name = "DispatchSnapshotSelector";
  using DispatchSnapshot::WrittenBufferSelector;
  using DispatchSnapshot::ParseWrittenBufferSelector;
  using DispatchSnapshot::MatchesWrittenBufferSelector;
  WrittenBufferSelector selector{7u, 9u};
  Require(name, "strict decimal parser", ParseWrittenBufferSelector("2:124224", selector) &&
      selector.slot == 2u && selector.declared_bytes == 124224u, "valid selector rejected");
  Require(name, "integer parser boundaries",
      ParseWrittenBufferSelector("4294967295:18446744073709551615", selector) &&
      selector.slot == UINT32_MAX && selector.declared_bytes == UINT64_MAX,
      "representable typed boundaries rejected");
  Require(name, "zero slot/leading zeros", ParseWrittenBufferSelector("00:0001", selector) &&
      selector.slot == 0u && selector.declared_bytes == 1u, "decimal zero slot rejected");
  constexpr std::string_view invalid[] = {
      "", "2", ":4", "2:", "2:0", "2:000", "-1:4", "+1:4", "2:-4", "2:+4",
      " 2:4", "2: 4", "2:4 ", "2:4x", "2:4:5", "0x2:4", "2:0x4",
      "4294967296:4", "2:18446744073709551616", "999999999999999999999999:1"};
  for (const auto text : invalid) {
    selector = {7u, 9u};
    Require(name, "malformed/overflow parser", !ParseWrittenBufferSelector(text, selector) &&
        selector.slot == 7u && selector.declared_bytes == 9u,
        "malformed selector accepted or failed parse changed its output");
  }

  ShaderRecompiler::IR::CompiledShaderInfo program{};
  program.stage = ShaderType::Compute;
  program.shader_hash = 0xf00du; // Synthetic diagnostic selector label, no guest shader bytes.
  program.info.buffers.resize(3u);
  program.info.buffers[2].written = true;
  ShaderRecompiler::IR::ResourceSnapshot resources{};
  resources.buffers.resize(3u);
  ShaderStageRuntime runtime{&program, &resources};
  const auto set_descriptor = [&](uint32_t stride, uint32_t count) {
    ShaderBufferResource descriptor{};
    descriptor.UpdateAddress48(0x0000000200000000ull);
    descriptor.fields[1] |= stride << 16u;
    descriptor.fields[2] = count;
    auto& value = resources.buffers[2];
    value.dword_count = 4u;
    std::copy_n(descriptor.fields, 4u, value.dwords.begin());
  };
  selector = {2u, 124224u};
  set_descriptor(48u, 2588u);
  Require(name, "actual structured SRD size", MatchesWrittenBufferSelector(selector, runtime),
      "48-byte records x2588 did not match the declared-byte selector");
  Require(name, "slot/written/runtime bounds",
      !MatchesWrittenBufferSelector({1u, 124224u}, runtime) &&
      !MatchesWrittenBufferSelector({UINT32_MAX, 124224u}, runtime) &&
      !MatchesWrittenBufferSelector({2u, 0u}, runtime) &&
      !MatchesWrittenBufferSelector(selector, ShaderStageRuntime{}), "invalid slot/runtime admitted");
  program.info.buffers[2].written = false;
  Require(name, "read-only buffer", !MatchesWrittenBufferSelector(selector, runtime),
      "read-only declaration admitted as a writer");
  program.info.buffers[2].written = true;
  resources.buffers[2].dword_count = 3u;
  Require(name, "truncated descriptor", !MatchesWrittenBufferSelector(selector, runtime),
      "truncated SRD admitted");
  resources.buffers.resize(2u);
  Require(name, "missing snapshot slot", !MatchesWrittenBufferSelector(selector, runtime),
      "absent snapshot entry admitted");
  resources.buffers.resize(3u);
  set_descriptor(0u, 124224u);
  Require(name, "actual byte-addressed SRD", MatchesWrittenBufferSelector(selector, runtime),
      "zero-stride byte count did not match");
  set_descriptor(48u, 0u);
  Require(name, "empty SRD", !MatchesWrittenBufferSelector(selector, runtime), "empty buffer admitted");
  set_descriptor(16383u, UINT32_MAX);
  Require(name, "actual wide SRD product",
      MatchesWrittenBufferSelector({2u, 70364449193985ull}, runtime) &&
      !MatchesWrittenBufferSelector({2u, 4294950913ull}, runtime),
      "declared SRD size was truncated to32 bits");

  // Actual admission function, including its immutable process settings/quota.
  // A rejected wrong span must neither choose a frame nor consume MAX=1.
  _putenv_s("KYTY_DISPATCH_SNAPSHOT", frame_mode ? "frame:10" : "f00d");
  _putenv_s("KYTY_DISPATCH_SNAPSHOT_MAX", "1");
  _putenv_s("KYTY_DISPATCH_SNAPSHOT_RING", "0");
  if (default_mode) {
    _putenv_s("KYTY_DISPATCH_SNAPSHOT_WRITTEN_BUFFER", "");
  } else {
    _putenv_s("KYTY_DISPATCH_SNAPSHOT_WRITTEN_BUFFER", "2:124224");
  }
  set_descriptor(48u, 12063u); // 579024, an independently different written span.
  Require(name, "wrong declared size", !MatchesWrittenBufferSelector(selector, runtime),
      "wrong actual SRD size admitted by the predicate");
  const bool wrong = DispatchSnapshot::Wanted(runtime, 10u);
  set_descriptor(48u, 2588u);
  const bool first = DispatchSnapshot::Wanted(runtime, frame_mode ? 11u : 10u);
  const bool second = DispatchSnapshot::Wanted(runtime, frame_mode ? 11u : 10u);
  Require(name, "actual quota/frame admission",
      default_mode ? (wrong && !first && !second) : (!wrong && first && !second),
      "wrong-size dispatch consumed admission state, or opt-in/default capture limit changed");
  std::printf("[cpu] DispatchSnapshotSelector parser=%zu malformed cases; frame=%u default=%u wrong=%u first=%u second=%u; GPU_setup=0\n",
      std::size(invalid), frame_mode ? 1u : 0u, default_mode ? 1u : 0u,
      wrong ? 1u : 0u, first ? 1u : 0u, second ? 1u : 0u);
}
} // namespace
} // namespace Libs::Graphics
