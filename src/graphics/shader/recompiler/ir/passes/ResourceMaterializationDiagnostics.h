// Optional diagnostic bridge. Compiler IR has no renderer/Windows dependency.
#pragma once
#include <cstdint>
namespace Libs::Graphics::ShaderRecompiler::IR::MaterializationDiagnostics {
enum class Phase : uint32_t { Setup, Active, Flat, Memo, Decode, Specialization, Count };
enum class Counter : uint32_t {
  SampledCalls, SelectedLeafId, CompiledCalls, CompiledSuccess, CompiledFailure,
  ReferenceCalls, ReferenceFallback, MemoAbsent, MemoEligible, MemoIneligible, MemoReused,
  MemoMiss, FlatSlots, FlatRuns, PlanNodes, DescriptorSources, BuffersDeclared, ImagesDeclared,
  SamplersDeclared, StrictReadCalls, StrictReadWords, StrictBlockHits, StrictBlockMisses,
  StrictBlockFailures, StrictExactFallbacks, OrdinaryReadCalls, OrdinaryReadWords,
  OrdinaryWholeCalls, OrdinaryBlockHits, OrdinaryBlockMisses, OrdinaryBlockFailures,
  OrdinaryExactFallbacks, CaptureReads, StrictBlocksEnabled, OrdinaryBlocksEnabled, Count
};
inline constexpr uint32_t CounterCount=static_cast<uint32_t>(Counter::Count);
inline constexpr const char* CounterNames[]{
  "sampled_calls","selected_leaf_id_sum","compiled_calls","compiled_success","compiled_failure",
  "reference_calls","reference_fallback","memo_absent","memo_eligible","memo_ineligible","memo_reused",
  "memo_miss","flat_slots","flat_runs","plan_nodes","descriptor_sources","buffers_declared","images_declared",
  "samplers_declared","strict_read_calls","strict_read_words","strict_block_hits","strict_block_misses",
  "strict_block_failures","strict_exact_fallbacks","ordinary_read_calls","ordinary_read_words",
  "ordinary_whole_calls","ordinary_block_hits","ordinary_block_misses","ordinary_block_failures",
  "ordinary_exact_fallbacks","capture_reads","strict_blocks_enabled","ordinary_blocks_enabled"
};
static_assert(sizeof(CounterNames)/sizeof(*CounterNames)==CounterCount);
struct Observer {
  void* userdata=nullptr;
  void (*phase)(void*,Phase,bool)=nullptr;
  void (*count)(void*,Counter,uint64_t)=nullptr;
};
inline thread_local const Observer* observer=nullptr;
inline void Count(Counter counter,uint64_t value=1) noexcept {
  const auto* sink=observer;if(sink!=nullptr&&sink->count!=nullptr)sink->count(sink->userdata,counter,value);
}
class PhaseScope final {
 public:
  explicit PhaseScope(Phase phase) noexcept : m_observer(observer),m_phase(phase) {
    if(m_observer!=nullptr&&m_observer->phase!=nullptr)m_observer->phase(m_observer->userdata,m_phase,true);
  }
  ~PhaseScope() noexcept { End(); }
  void End() noexcept {
    if(m_observer!=nullptr&&m_observer->phase!=nullptr)m_observer->phase(m_observer->userdata,m_phase,false);
    m_observer=nullptr;
  }
  PhaseScope(const PhaseScope&)=delete;PhaseScope& operator=(const PhaseScope&)=delete;
 private:
  const Observer* m_observer;Phase m_phase;
};
} // namespace Libs::Graphics::ShaderRecompiler::IR::MaterializationDiagnostics
