#pragma once
#include "libs/libs.h"
#include "common/switches.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Libs::BodyCostDiagnostic {
enum class Function : size_t { ShPatch, Payload, ShRange, CxPatch, ShRegs, Dispatch, Count };
inline constexpr const char* Names[] = {"AgcSetShRegIndirectPatchAddRegisters", "AgcGetDataPacketPayloadAddress", "AgcCbSetShRegisterRangeDirect", "AgcSetCxRegIndirectPatchAddRegisters", "AgcCbSetShRegistersDirect", "AgcCbDispatch"};
struct Counters {
  std::atomic<uint64_t> calls{0}, samples{0}, cycles{0}, wall_ns{0}, invalid{0};
  std::atomic<uint64_t> query_failed{0}, straddled{0}, underflow{0}, last_error{0};
  std::atomic<uint64_t> empty_samples{0}, empty_cycles{0}, empty_wall_ns{0}, empty_failed{0}, empty_last_error{0};
};
inline std::array<Counters, static_cast<size_t>(Function::Count)> CountersByFunction;
inline std::atomic<uint32_t> MainOsTid{0};
inline std::atomic<uint64_t> Epoch{0};
// Only sampled completions and5s snapshots take this; no per-call body mutex.
inline std::mutex CompletedSamplesMutex;
inline bool Enabled() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  static const bool enabled = KYTY_GETENV("KYTY_DEBUG_HLE_BODY_COST") != nullptr;
  return enabled;
#else
  return false;
#endif
}
// Explicit function-body scope; PRINT_NAME retains its current macro semantics.
// Timing is inclusive of child calls and memory faults. Never sum overlapping scopes.
class Scope {
 public:
  explicit Scope(Function function) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
    if (!Enabled() || !g_main_call_diagnostic_role) return;
    auto& counter = CountersByFunction[static_cast<size_t>(function)];
    const auto call = counter.calls.fetch_add(1, std::memory_order_relaxed);
    if ((call & 63u) != 0) return;
    MainOsTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    // Matched empty bracket on this SAME Main, only when a body will be sampled.
    // Report separately; never force subtraction from the actual body sample.
    const auto empty_wall = std::chrono::steady_clock::now();
    ULONG64 empty_begin = 0, empty_end = 0;
    const bool empty_first = QueryThreadCycleTime(GetCurrentThread(), &empty_begin) != 0;
    DWORD empty_error = empty_first ? 0 : GetLastError();
    const bool empty_second = empty_first && QueryThreadCycleTime(GetCurrentThread(), &empty_end) != 0;
    if (empty_first && !empty_second) empty_error = GetLastError();
    const auto empty_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - empty_wall).count();
    {
      std::lock_guard lock(CompletedSamplesMutex);
      if (empty_second && empty_end >= empty_begin && empty_ns >= 0) {
        counter.empty_samples.fetch_add(1, std::memory_order_relaxed);
        counter.empty_cycles.fetch_add(empty_end-empty_begin, std::memory_order_relaxed);
        counter.empty_wall_ns.fetch_add(static_cast<uint64_t>(empty_ns), std::memory_order_relaxed);
      } else {
        counter.empty_failed.fetch_add(1, std::memory_order_relaxed);
        counter.empty_last_error.store(empty_error, std::memory_order_relaxed);
      }
    }
    m_epoch = Epoch.load(std::memory_order_relaxed);
    m_wall = std::chrono::steady_clock::now();
    ULONG64 cycles = 0;
    if (!QueryThreadCycleTime(GetCurrentThread(), &cycles)) {
      const DWORD error = GetLastError();
      std::lock_guard lock(CompletedSamplesMutex);
      counter.invalid.fetch_add(1, std::memory_order_relaxed);
      counter.query_failed.fetch_add(1, std::memory_order_relaxed);
      counter.last_error.store(error, std::memory_order_relaxed);
      return;
    }
    m_cycles = cycles;
    m_counter = &counter;
#endif
  }
  ~Scope() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
    if (m_counter == nullptr) return;
    ULONG64 cycles = 0;
    const bool valid = QueryThreadCycleTime(GetCurrentThread(), &cycles) != 0;
    const DWORD error = valid ? 0 : GetLastError();
    const auto wall = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - m_wall).count();
    std::lock_guard lock(CompletedSamplesMutex);
    if (!valid) {
      m_counter->invalid.fetch_add(1, std::memory_order_relaxed);
      m_counter->query_failed.fetch_add(1, std::memory_order_relaxed);
      m_counter->last_error.store(error, std::memory_order_relaxed);
      return;
    }
    if (cycles < m_cycles || wall < 0) {
      m_counter->invalid.fetch_add(1, std::memory_order_relaxed);
      m_counter->underflow.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (Epoch.load(std::memory_order_relaxed) != m_epoch) {
      m_counter->invalid.fetch_add(1, std::memory_order_relaxed);
      m_counter->straddled.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    m_counter->cycles.fetch_add(cycles - m_cycles, std::memory_order_relaxed);
    m_counter->wall_ns.fetch_add(static_cast<uint64_t>(wall), std::memory_order_relaxed);
    m_counter->samples.fetch_add(1, std::memory_order_relaxed);
#endif
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
 private:
  Counters* m_counter = nullptr;
  uint64_t m_cycles = 0, m_epoch = 0;
  std::chrono::steady_clock::time_point m_wall;
};
// Called only by the existing5s counter printer. Exact raw cycle domain; never
// convert cycles to elapsed time/MHz. First denominator window is unavailable.
inline void Report(uint64_t counter_second) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  if (!Enabled()) return;
  static uint64_t previous_cycles = 0;
  static bool previous_valid = false;
  ULONG64 main_cycles = 0;
  bool current_valid = false;
  DWORD denominator_error = 0;
  const auto tid = MainOsTid.load(std::memory_order_relaxed);
  HANDLE thread = tid != 0 ? OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid) : nullptr;
  if (thread != nullptr) {
    current_valid = QueryThreadCycleTime(thread, &main_cycles) != 0;
    if (!current_valid) denominator_error = GetLastError();
    CloseHandle(thread);
  } else if (tid != 0) {
    denominator_error = GetLastError();
  }
  const bool denominator_valid = current_valid && previous_valid && main_cycles >= previous_cycles;
  const auto denominator = denominator_valid ? main_cycles - previous_cycles : 0;
  struct Snapshot { uint64_t calls, samples, cycles, wall, invalid, query_failed, straddled, underflow, last_error,
                            empty_samples, empty_cycles, empty_wall, empty_failed, empty_error; };
  std::array<Snapshot, static_cast<size_t>(Function::Count)> values{};
  {
    std::lock_guard lock(CompletedSamplesMutex);
    Epoch.fetch_add(1, std::memory_order_relaxed);
    for (size_t i = 0; i < values.size(); ++i) {
      auto& c = CountersByFunction[i];
      values[i] = {c.calls.exchange(0), c.samples.exchange(0), c.cycles.exchange(0),
                   c.wall_ns.exchange(0), c.invalid.exchange(0), c.query_failed.exchange(0),
                   c.straddled.exchange(0), c.underflow.exchange(0), c.last_error.exchange(0),
                   c.empty_samples.exchange(0), c.empty_cycles.exchange(0), c.empty_wall_ns.exchange(0),
                   c.empty_failed.exchange(0), c.empty_last_error.exchange(0)};
    }
  }
  std::printf("hle-body-window: t=%llus scope=main os_thread_id=%u main_cycles=%llu denominator_valid=%u denominator_error=%lu sample_stride=64 inclusive=1 boundaries=approximate\n",
              static_cast<unsigned long long>(counter_second), tid,
              static_cast<unsigned long long>(denominator), unsigned(denominator_valid), static_cast<unsigned long>(denominator_error));
  previous_cycles = main_cycles;
  previous_valid = current_valid;
  for (size_t i = 0; i < CountersByFunction.size(); ++i) {
    const auto& c = values[i];
    std::printf("hle-body-cost: t=%llus scope=main function=%s calls=%llu samples=%llu cpu_cycles=%llu wall_ns=%llu invalid=%llu\n",
                static_cast<unsigned long long>(counter_second), Names[i],
                static_cast<unsigned long long>(c.calls), static_cast<unsigned long long>(c.samples),
                static_cast<unsigned long long>(c.cycles), static_cast<unsigned long long>(c.wall),
                static_cast<unsigned long long>(c.invalid));
    std::printf("hle-body-invalid: t=%llus function=%s query_failed=%llu straddled=%llu underflow=%llu last_win32_error=%llu\n",
                static_cast<unsigned long long>(counter_second), Names[i],
                static_cast<unsigned long long>(c.query_failed), static_cast<unsigned long long>(c.straddled),
                static_cast<unsigned long long>(c.underflow), static_cast<unsigned long long>(c.last_error));
    std::printf("hle-body-empty: t=%llus function=%s samples=%llu cpu_cycles=%llu wall_ns=%llu failed=%llu last_win32_error=%llu subtraction=none\n",
                static_cast<unsigned long long>(counter_second), Names[i],
                static_cast<unsigned long long>(c.empty_samples), static_cast<unsigned long long>(c.empty_cycles),
                static_cast<unsigned long long>(c.empty_wall), static_cast<unsigned long long>(c.empty_failed),
                static_cast<unsigned long long>(c.empty_error));
  }
  std::fflush(stdout);
#endif
}
} // namespace Libs::BodyCostDiagnostic
