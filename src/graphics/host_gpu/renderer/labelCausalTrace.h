#ifndef KYTY_LABEL_CAUSAL_TRACE_DIAGNOSTIC_PROPOSAL_H_
#define KYTY_LABEL_CAUSAL_TRACE_DIAGNOSTIC_PROPOSAL_H_

// Temporary diagnostic only, default OFF; no resource/scheduler state changes.
#include "common/switches.h"
#include "kernel/memory.h"
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
  FaultBegin, FaultEnd, PointerResolved, PointerUnavailable
};
struct Row {
  uint64_t ordinal, ns, owner, tick, token, address, size, value, pc, submit;
  uint32_t stage, tid;
};
struct Config {
  bool on = false;
  uint64_t pointer = 0, span = 0x4000, start_frame = 400;
  std::array<uint64_t, 8> read_pcs {};
  size_t pc_count = 0;
};
inline const Config& GetConfig() {
  static const Config config = [] {
    Config c;
    const char* on = KYTY_GETENV("KYTY_LABEL_CAUSAL_TRACE");
    c.on = on != nullptr && on[0] == '1' && on[1] == '\0';
    if (!c.on) return c;
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
inline constexpr size_t Capacity = 32768;
inline std::array<Row, Capacity> rows {};
inline std::mutex mutex;
inline std::atomic<uint64_t> start_ns {0}, target {0};
inline std::atomic_bool active {false};
inline uint64_t requested = 0, dropped = 0, pointer_errors = 0;
inline size_t retained = 0;
inline bool dumped = false;
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
    if (frame < GetConfig().start_frame) return;
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
