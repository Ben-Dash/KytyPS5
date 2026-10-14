#ifndef KYTY_LABEL_CAUSAL_TRACE_DIAGNOSTIC_PROPOSAL_H_
#define KYTY_LABEL_CAUSAL_TRACE_DIAGNOSTIC_PROPOSAL_H_

// Temporary diagnostic only, default OFF; no resource/scheduler state changes.
#include "common/switches.h"
#include "kernel/memory.h"
#include "graphics/host_gpu/renderer/drainStats.h"
#include "graphics/host_gpu/renderer/tiledDownloadDiagnostic.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Libs::Graphics::LabelCausalTrace {
enum class Stage : uint32_t {
  GuestEnqueue, CpStart, LabelRecord, OrderedBegin, ReadbackArm,
  NativeSubmit, NativeObserved, PublishBegin, BackingWritten, Finalized,
  FaultBegin, FaultEnd, PointerResolved, PointerUnavailable, SliceEnter, SliceExit,
  WaitSite, WaitMask, WaitPollOutcome, WaitReadResult, WaitEpisodeDone, WaitEpisodeTiming,
  WaitCvBegin, WaitCvEnd, WaitSourceNotify
};
struct Row {
  uint64_t ordinal, ns, owner, tick, token, address, size, value, pc, submit;
  uint32_t stage, tid;
};
struct Config {
  bool on = false;
  uint64_t pointer = 0, span = 0x4000, start_frame = 400;
  uint64_t capture_not_before_ns = 0;
  std::array<uint64_t, 8> read_pcs {};
  size_t pc_count = 0;
};
inline const Config& GetConfig() {
  static const Config config = [] {
    Config c;
    const char* on = KYTY_GETENV("KYTY_LABEL_CAUSAL_TRACE");
    c.on = on != nullptr && on[0] == '1' && on[1] == '\0';
    if (!c.on) return c;
    if (const char* p = KYTY_GETENV("KYTY_LABEL_CAUSAL_MIN_ELAPSED_S"); p != nullptr) {
      char* end = nullptr;
      const auto seconds = std::strtoull(p, &end, 10);
      if (end == p || *end != '\0' || seconds > 300) {
        c.on = false;
        std::fprintf(stderr, "label-causal invalid elapsed gate; diagnostic disabled\n");
        return c;
      }
      const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
      c.capture_not_before_ns = now + seconds * 1'000'000'000ull;
    }
    if (const char* p = KYTY_GETENV("KYTY_LABEL_CAUSAL_POINTER"); p != nullptr) {
      char* end = nullptr;
      c.pointer = std::strtoull(p, &end, 16);
      if (end != p && *end == '+') c.span = std::strtoull(end + 1, &end, 16);
      if (end == p || *end != '\0' || c.pointer == 0 || c.span == 0 ||
          c.span > 0x100000) {
        c.on = false;
        std::fprintf(stderr, "label-causal invalid pointer/span; diagnostic disabled\n");
      }
    }
    const char* p = KYTY_GETENV("KYTY_LABEL_CAUSAL_READ_PC");
    while (p != nullptr && *p != '\0' && c.pc_count < c.read_pcs.size()) {
      char* end = nullptr;
      const auto pc = std::strtoull(p, &end, 16);
      if (end == p || pc == 0) break;
      c.read_pcs[c.pc_count++] = pc;
      p = *end == ',' ? end + 1 : nullptr;
    }
    return c;
  }();
  return config;
}
inline bool Enabled() noexcept { return GetConfig().on; }
inline constexpr size_t Capacity = 262144;
inline std::array<Row, Capacity> rows {};
inline std::mutex mutex;
inline std::atomic<uint64_t> start_ns {0}, target {0};
inline std::atomic_bool active {false};
inline uint64_t requested = 0, dropped = 0, pointer_errors = 0;
inline size_t retained = 0;
inline bool dumped = false;
// Temporary fixed-size counters. Share the already phase-gated 10s trace epoch;
// no full FunctionProfile, nested function timers, GPU timestamps, row allocations or extra waits.
inline std::array<std::atomic<uint64_t>,64> partial_event_counts{};
inline std::array<std::atomic<uint64_t>,3> partial_chain_counts{},partial_render_counts{},partial_shader_write_counts{};
inline std::atomic<uint64_t> partial_outside{0},partial_meta_consumed{0},partial_image_consumed{0};
inline std::array<std::atomic<uint64_t>,4> native_clear_counts{}; // allTransfer,allColor,CStransfer,CScolor
inline std::array<std::atomic<uint64_t>,DrainStats::Pm4OpCount+1u> chain_access_counts{},chain_full_counts{};
inline std::atomic<uint64_t> cs_color_required{0},cs_stage_history_unknown{0};
inline thread_local bool partial_inside_compute_clear=false;
inline thread_local bool partial_color_pending=false,partial_stage_history_known=false;
inline thread_local uint64_t partial_stage_owner=0;
inline void ObservePartialOwner(uint64_t owner) noexcept {
  if(partial_stage_owner!=owner) {
    // A different scheduler may still have unsubmitted native producers. Discard
    // diagnostic eligibility knowledge, never declare that other owner covered.
    partial_stage_owner=owner;partial_stage_history_known=false;partial_color_pending=false;
  }
}
inline bool PartialCaptureActive() noexcept { return active.load(std::memory_order_acquire); }
class PartialComputeClearScope {
public:
  PartialComputeClearScope() noexcept : on(PartialCaptureActive()),previous(partial_inside_compute_clear) {
    if(on)partial_inside_compute_clear=true;
  }
  ~PartialComputeClearScope() { if(on)partial_inside_compute_clear=previous; }
private:
  bool on,previous;
};
inline void NotePartialEvent(uint32_t type,bool chain,bool rendering,bool writes,uint64_t owner=0) noexcept {
  if(!PartialCaptureActive())return;
  if(type<64u)partial_event_counts[type].fetch_add(1,std::memory_order_relaxed);
  else partial_outside.fetch_add(1,std::memory_order_relaxed);
  const uint32_t index=type==7u?0u:type==15u?1u:type==16u?2u:3u;
  if(index<3u) {
    if(chain)partial_chain_counts[index].fetch_add(1,std::memory_order_relaxed);
    if(rendering)partial_render_counts[index].fetch_add(1,std::memory_order_relaxed);
    if(writes)partial_shader_write_counts[index].fetch_add(1,std::memory_order_relaxed);
  }
  if(type==7u) {
    ObservePartialOwner(owner);
    if(!partial_stage_history_known)cs_stage_history_unknown.fetch_add(1,std::memory_order_relaxed);
    else if(partial_color_pending)cs_color_required.fetch_add(1,std::memory_order_relaxed);
    // Diagnostic history only: no barrier, resource, scheduler, guest or Vulkan state mutation.
    partial_color_pending=false;partial_stage_history_known=true;
  }
}
inline void NotePartialConsumed(bool meta) noexcept {
  if(!PartialCaptureActive())return;
  (meta?partial_meta_consumed:partial_image_consumed).fetch_add(1,std::memory_order_relaxed);
}
inline void NotePartialNativeClear(bool color,uint64_t owner) noexcept {
  if(!PartialCaptureActive())return;
  ObservePartialOwner(owner);
  native_clear_counts[color?1u:0u].fetch_add(1,std::memory_order_relaxed);
  if(partial_inside_compute_clear)native_clear_counts[color?3u:2u].fetch_add(1,std::memory_order_relaxed);
  if(color)partial_color_pending=true;
}
inline void NotePartialChainBoundary(uint32_t op,bool full) noexcept {
  if(!PartialCaptureActive())return;
  (full?chain_full_counts:chain_access_counts)[std::min(op,DrainStats::Pm4OpCount)].fetch_add(1,std::memory_order_relaxed);
}
inline void ReportPartialCounts() noexcept {
  std::printf("partial-flush-counts population=all_actual_boundaries_in_label_epoch snapshot=atomic_fields_not_transactional outside=%llu meta_consumed=%llu image_consumed=%llu native_transfer_clear=%llu native_color_clear=%llu cs_transfer_clear=%llu cs_color_clear=%llu cs_color_required=%llu cs_history_unknown=%llu",
    static_cast<unsigned long long>(partial_outside.load()),static_cast<unsigned long long>(partial_meta_consumed.load()),
    static_cast<unsigned long long>(partial_image_consumed.load()),static_cast<unsigned long long>(native_clear_counts[0].load()),
    static_cast<unsigned long long>(native_clear_counts[1].load()),static_cast<unsigned long long>(native_clear_counts[2].load()),
    static_cast<unsigned long long>(native_clear_counts[3].load()),static_cast<unsigned long long>(cs_color_required.load()),
    static_cast<unsigned long long>(cs_stage_history_unknown.load()));
  for(uint32_t type=0;type<64u;++type)if(const auto count=partial_event_counts[type].load();count!=0)
    std::printf(" event_%02x=%llu",type,static_cast<unsigned long long>(count));
  std::printf("\n");
  for(uint32_t i=0;i<3u;++i)std::printf("partial-flush-state event=%02x pending_chain=%llu rendering=%llu shader_writes=%llu\n",
    i==0u?7u:i==1u?15u:16u,static_cast<unsigned long long>(partial_chain_counts[i].load()),
    static_cast<unsigned long long>(partial_render_counts[i].load()),static_cast<unsigned long long>(partial_shader_write_counts[i].load()));
  for(uint32_t op=0;op<chain_access_counts.size();++op) {
    const auto access=chain_access_counts[op].load(),full=chain_full_counts[op].load();
    if(access+full!=0)std::printf("partial-chain-boundary pm4=%u access=%llu full=%llu\n",op,
      static_cast<unsigned long long>(access),static_cast<unsigned long long>(full));
  }
}
inline uint64_t Now() noexcept {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
inline bool Overlaps(uint64_t a, uint64_t as, uint64_t b, uint64_t bs) noexcept {
  return a != 0 && b != 0 && as != 0 && bs != 0 && as <= UINT64_MAX - a &&
         bs <= UINT64_MAX - b && a < b + bs && b < a + as;
}
inline bool ReadPointer(uint64_t address, uint64_t& resolved) noexcept {
  if (Libs::LibKernel::Memory::TryReadBacking(address, &resolved, sizeof(resolved)))
    return true;
#if defined(_WIN32)
  // Loader image data is outside the kernel backing allocator. Observe only
  // already-readable committed bytes: never service a guest fault or unblock it.
  MEMORY_BASIC_INFORMATION info {};
  const auto* pointer = reinterpret_cast<const void*>(address);
  if (VirtualQuery(pointer, &info, sizeof(info)) != sizeof(info) ||
      info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
    return false;
  const auto protection = info.Protect & 0xff;
  if (protection != PAGE_READONLY && protection != PAGE_READWRITE &&
      protection != PAGE_WRITECOPY && protection != PAGE_EXECUTE_READ &&
      protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY)
    return false;
  SIZE_T copied = 0;
  return ReadProcessMemory(GetCurrentProcess(), pointer, &resolved, sizeof(resolved),
                           &copied) != 0 && copied == sizeof(resolved);
#else
  return false;
#endif
}
inline bool Matches(Stage stage, uint64_t address, uint64_t size, uint64_t pc) noexcept {
  const auto& c = GetConfig();
  if (stage == Stage::FaultBegin || stage == Stage::FaultEnd) {
    if (c.pc_count != 0) {
      for (size_t i = 0; i < c.pc_count; ++i) if (c.read_pcs[i] == pc) return true;
      return false;
    }
    // Never retain all guest faults while discovering a yet-unresolved table.
    return c.pointer != 0 && Overlaps(address, size, target.load(), c.span);
  }
  if (stage == Stage::LabelRecord || stage == Stage::OrderedBegin ||
      stage == Stage::ReadbackArm || stage == Stage::PublishBegin ||
      stage == Stage::BackingWritten || stage == Stage::Finalized) {
    return c.pointer == 0 || Overlaps(address, size, target.load(), c.span);
  }
  return true;
}
// owner is the scheduler's numeric identity, never a retained/dereferenced object.
// token is a readback token, or the existing guest Submission.sequence for
// GuestEnqueue/CpStart. submit is the existing per-CP submit ID where available.
inline void Record(Stage stage, uint64_t owner = 0, uint64_t tick = 0,
                   uint64_t token = 0, uint64_t address = 0, uint64_t size = 0,
                   uint64_t value = 0, uint64_t pc = 0, uint64_t submit = 0) noexcept {
  if (!Enabled() || !active.load(std::memory_order_acquire) ||
      !Matches(stage, address, size, pc)) return;
  const auto now = Now();
  if (now - start_ns.load(std::memory_order_acquire) >= 10'000'000'000ull) {
    active.store(false, std::memory_order_release);
    return;
  }
  uint32_t tid = 0;
#if defined(_WIN32)
  tid = GetCurrentThreadId();
#endif
  std::lock_guard lock(mutex);
  // A producer can reach this mutex after TickFrame closed/dumped the window.
  // Recheck admission here so copied output is never followed by hidden rows.
  if (!active.load(std::memory_order_acquire) || dumped) return;
  ++requested;
  if (retained == rows.size()) { ++dropped; return; }
  rows[retained++] = {requested, now, owner, tick, token, address, size, value, pc,
                      submit, static_cast<uint32_t>(stage), tid};
}
inline void TickFrame(uint64_t frame) {
  if (!Enabled()) return;
  const auto now = Now();
  if (start_ns.load() == 0) {
    if (frame < GetConfig().start_frame || now < GetConfig().capture_not_before_ns) return;
    start_ns.store(now, std::memory_order_release);
    active.store(true, std::memory_order_release);
    std::printf("label-causal begin frame=%" PRIu64 " seconds=10 capacity=%zu "
                "domain=steady_ns scope=dedicated_diagnostic\n", frame, Capacity);
  }
  if (now - start_ns.load() < 10'000'000'000ull) {
    const auto& c = GetConfig();
    if (c.pointer != 0) {
      uint64_t resolved = 0;
      // Read checked backing outside the trace mutex; no protected-VA access.
      const bool valid = ReadPointer(c.pointer, resolved) && resolved != 0 &&
          c.span <= UINT64_MAX - resolved;
      target.store(valid ? resolved : 0, std::memory_order_release);
      if (!valid) { std::lock_guard lock(mutex); ++pointer_errors; }
      Record(valid ? Stage::PointerResolved : Stage::PointerUnavailable, 0, 0,
             0, c.pointer, sizeof(resolved), valid ? resolved : 0);
    }
    return;
  }
  active.store(false, std::memory_order_release);
  std::vector<Row> copy;
  uint64_t n = 0, loss = 0, errors = 0;
  {
    std::lock_guard lock(mutex);
    if (dumped) return;
    dumped = true;
    copy.assign(rows.begin(), rows.begin() + retained);
    n = requested; loss = dropped; errors = pointer_errors;
  }
  // No cache/queue wait or object query; print after releasing trace mutex.
  ReportPartialCounts();
  TiledDownloadDiagnostic::Report();
  std::printf("label-causal end frame=%" PRIu64 " requested=%" PRIu64
              " retained=%zu dropped=%" PRIu64 " pointer_errors=%" PRIu64
              " completion_domain=cpu_observed_timeline_upper_bound\n",
              frame, n, copy.size(), loss, errors);
  for (const auto& r : copy)
    std::printf("label-causal row=%" PRIu64 " ns=%" PRIu64 " stage=%u tid=%u "
                "owner=0x%" PRIx64 " tick=%" PRIu64 " token=%" PRIu64
                " address=0x%" PRIx64 " size=%" PRIu64 " value=0x%" PRIx64
                " pc=0x%" PRIx64 " submit=%" PRIu64 "\n",
                r.ordinal, r.ns, r.stage, r.tid, r.owner, r.tick, r.token,
                r.address, r.size, r.value, r.pc, r.submit);
  std::fflush(stdout);
}
} // namespace Libs::Graphics::LabelCausalTrace
#endif
