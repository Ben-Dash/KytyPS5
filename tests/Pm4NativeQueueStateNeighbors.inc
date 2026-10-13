// Ignored/uncompiled proposal. Include globally in BindlessLifecycleTests.cpp AFTER
// WaitRegMemNativeQueuedNeighborTests.inc (existing real idle accessor) and
// EmbeddedFetchGpuTests.inc (DstSel). Root owns integration/build/native execution.
// Uses the same synthetic LOAD_DWORD/ADD_U32/STORE_DWORD as recording-age fixture.
#include "common/hostException.h"
#include "graphics/guest_gpu/command_processor/pm4Dispatch.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/shader/shaderCompiler.h"
#include <chrono>
#include <condition_variable>
#include <semaphore>
#include <thread>

namespace Libs::Graphics {
namespace {
void ConfigurePm4NativeQueueStateNeighbors(bool enabled) {
  Require("Pm4NativeQueueState", "configuration before GPU/static getters",
      _putenv_s("KYTY_PM4_CPU_QUANTUM_PACKETS", enabled ? "64" : "0") == 0 &&
      _putenv_s("KYTY_RECORD_THREAD", "0") == 0 &&
      _putenv_s("KYTY_RECORDING_AGE_US", "0") == 0 &&
      _putenv_s("KYTY_DISPATCH_LOOKAHEAD", "0") == 0 &&
      _putenv_s("KYTY_COMPUTE_CHAIN", "1") == 0,
      "could not pin actual shared queue configuration; not a result");
}

std::vector<uint32_t> Pm4QueueStateShader(uint32_t increment) {
  // BUFFER_LOAD_DWORD v1, v0, s[0:3], inline-zero scalar offset.
  const auto mubuf0 = [](uint32_t op) {
    return (0x38u << 26u) | (op << 18u) | (1u << 13u);
  };
  constexpr uint32_t mubuf1 = (128u << 24u) | (1u << 8u);
  // V_ADD_U32 v1, inline increment, v1; all lanes write a different V# record.
  return {mubuf0(0x0cu), mubuf1,
      (0x25u << 25u) | (1u << 17u) | (1u << 9u) | (128u + increment),
      mubuf0(0x1cu), mubuf1, 0xbf810000u};
}

void CheckPm4NativeQueueStateNeighbors(VulkanHarness& vulkan,
                                      bool expect_quantum, bool ib_ce = false) {
  constexpr const char* name = "Pm4NativeQueueState";
  constexpr uint64_t base = 0x00000002c8000000ull, bytes = 0x80000u;
  constexpr uint64_t code_a = base + 0x10000u, code_b = base + 0x11000u;
  constexpr uint64_t out_a = base + 0x20100u, out_b = base + 0x30100u;
  constexpr uint64_t markers = base + 0x40000u;
  constexpr uint64_t control_a = base + 0x50100u, control_b = base + 0x60100u;
  constexpr uint32_t lanes = 64u, count = 192u, guard = 0xdecafbadu;
  constexpr uint32_t seed_a = 0xfffffff0u, seed_b = 0x12345000u;
  constexpr uint32_t fringe = 0xbca1f007u, const_value = 0xbadc0123u;
  constexpr uint32_t dead_value = 0xf00dbeefu;
  auto& renderer = vulkan.RuntimeRenderer();
  renderer.InitializeGpu(nullptr);
  auto& gpu = renderer.GetGpu();
  auto& scheduler = renderer.GetCommandScheduler();
  auto& cache = renderer.GetBufferCache();
  Require(name, "real protected-memory exception route",
      Common::HostException::InstallHandler([](const Common::HostException::ExceptionInfo& info) {
        if (info.type != Common::HostException::ExceptionType::AccessViolation ||
            (info.access_violation_type != Common::HostException::AccessViolationType::Read &&
             info.access_violation_type != Common::HostException::AccessViolationType::Write)) return false;
        return LibKernel::Memory::HandleGpuFault(
            info.access_violation_type == Common::HostException::AccessViolationType::Write ?
            PageFaultAccess::Write : PageFaultAccess::Read, info.access_violation_vaddr);
      }), "real fault handler unavailable; not native result proof");
  int64_t direct = -1;
  Require(name, "owned direct allocation", LibKernel::Memory::KernelAllocateDirectMemory(
      0, LibKernel::Memory::KernelGetDirectMemorySize(), bytes, 0x10000u, 0, &direct) == 0,
      "finite synthetic allocation failed; not intended regression");
  void* mapped = reinterpret_cast<void*>(base);
  Require(name, "owned direct mapping", LibKernel::Memory::KernelMapDirectMemory(
      &mapped, bytes, 0x3, 0x10, direct, 0x10000u) == 0 && mapped == reinterpret_cast<void*>(base),
      "finite synthetic mapping failed; not intended regression");
  std::fill_n(static_cast<uint32_t*>(mapped), bytes / 4u, guard);
  for (const auto address : {out_a, control_a, out_b, control_b}) {
    const uint32_t seed = (address == out_a || address == control_a) ? seed_a : seed_b;
    for (uint32_t lane = 0; lane < lanes; ++lane)
      reinterpret_cast<uint32_t*>(address)[lane] = seed + lane;
  }
  std::fill_n(reinterpret_cast<uint32_t*>(markers), 16u, 0u);
  const auto shader_a = Pm4QueueStateShader(1u), shader_b = Pm4QueueStateShader(7u);
  for (const auto& pair : {std::pair{code_a, &shader_a}, std::pair{code_b, &shader_b}}) {
    std::memcpy(reinterpret_cast<void*>(pair.first), pair.second->data(), pair.second->size() * 4u);
    ShaderMapUserData(pair.first, {.type = Prospero::ShaderBinaryType::kCs,
        .code_size_bytes = static_cast<uint32_t>(pair.second->size() * 4u)});
  }
  const auto descriptor = [&](uint64_t address) {
    ShaderBufferResource d{}; d.UpdateAddress48(address); d.fields[1] |= 4u << 16u;
    d.fields[2] = lanes;
    d.fields[3] = DstSel(4u, 5u, 6u, 7u) |
        (static_cast<uint32_t>(Prospero::BufferFormat::k32UInt) << 12u);
    return d;
  };
  // A real finite timeline wait bounds native completion; no polling GPU shader.
  const auto flush_bounded = [&] {
    scheduler.Flush();
    const uint64_t tick = scheduler.CurrentTick() - 1u;
    const auto semaphore = scheduler.GetMasterSemaphore().Handle();
    vk::SemaphoreWaitInfo wait{}; wait.semaphoreCount = 1u;
    wait.pSemaphores = &semaphore; wait.pValues = &tick;
    const auto result = vulkan.Device().waitSemaphores(&wait, 5'000'000'000ull);
    Require(name, "bounded real native completion", result == vk::Result::eSuccess,
        "finite native command failed/timed out; no GPU reset/repeat permitted");
    scheduler.PopPendingOperations(false);
    scheduler.WaitPriorityOperations(tick);
  };
  bool setup = false, controls = true;
  gpu.SendCommandSync([&] {
    setup = GuestGpu::IsGpuThread() && !CommandRecordingDeferred();
    LibKernel::Memory::InstallGpuResources(&renderer); renderer.MapMemory(base, bytes);
    HW::Context regs{}; HW::UserConfig user{}; HW::Shader shaders{};
    scheduler.Begin(regs, user, shaders);
    for (const auto& pair : {std::pair{code_a, control_a}, std::pair{code_b, control_b}}) {
      shaders.SetCsShader({.data_addr = pair.first, .num_thread_x = lanes,
          .num_thread_y = 1u, .num_thread_z = 1u, .wave_size = 64u,
          .user_sgpr = 4u, .tgid_x_en = false});
      const auto d = descriptor(pair.second);
      for (uint32_t i = 0; i < 4u; ++i)
        shaders.SetCsUserSgpr(i, d.fields[i], HW::UserSgprType::Unknown);
      ShaderComputeInputInfo info{};
      const auto program = renderer.GetPipelineCache().GetComputeProgram(
          shaders.GetCs(), regs.GetShaderRegisters(), info);
      Require(name, "real ordinary load/add/store program", bool(program) &&
          info.stage.program != nullptr && info.stage.resources != nullptr &&
          info.stage.resources->uniform_fill.kind == ShaderRecompiler::IR::UniformFillKind::None &&
          renderer.GetPipelineCache().GetComputePipeline(info, program).pipeline != nullptr,
          "synthetic shader did not create the actual production compute pipeline");
      Require(name, "independent native dispatch", renderer.GetRenderExecutor().DispatchDirect(
          0, scheduler.Current(), 1u, 1u, 1u, 0x41u), "warm pipeline was deferred");
      uint32_t old = 0;
      Require(name, "native ownership before actual readback",
          cache.IsRegionGpuModified(pair.second, lanes * 4u) &&
          LibKernel::Memory::TryReadBacking(pair.second, &old, 4u) &&
          old == (pair.second == control_a ? seed_a : seed_b),
          "positive control did not leave old CPU bytes behind a real GPU writer");
      flush_bounded();
      cache.ReadMemory(pair.second, lanes * 4u); flush_bounded();
      std::array<uint32_t, lanes + 8u> actual{};
      Require(name, "native control readback", LibKernel::Memory::TryReadBacking(
          pair.second - 16u, actual.data(), sizeof(actual)), "native control not published");
      for (uint32_t i = 0; i < actual.size(); ++i) {
        const uint32_t expected = i >= 4u && i < 4u + lanes ?
            ((pair.second == control_a ? seed_a + 1u : seed_b + 7u) + i - 4u) : guard;
        controls &= actual[i] == expected;
      }
    }
  });
  Require(name, "independent A/B native pipeline/numeric/guard controls", setup && controls,
      "positive native controls failed before scheduling test; not fairness RED");

  const auto write = [](std::vector<uint32_t>& to, uint64_t dst, uint32_t value) {
    to.insert(to.end(), {KYTY_PM4(5, Pm4::IT_WRITE_DATA, 0), 0u,
        static_cast<uint32_t>(dst), static_cast<uint32_t>(dst >> 32u), value});
  };
  const auto set_sh = [](std::vector<uint32_t>& to, uint32_t offset,
                         std::initializer_list<uint32_t> values) {
    to.push_back(KYTY_PM4(static_cast<uint32_t>(values.size()) + 2u, Pm4::IT_SET_SH_REG, 0));
    to.push_back(offset); to.insert(to.end(), values);
  };
  const auto setup_cs = [&](std::vector<uint32_t>& to, uint64_t code, uint64_t output) {
    set_sh(to, Pm4::COMPUTE_PGM_LO,
        {static_cast<uint32_t>(code >> 8u), static_cast<uint32_t>(code >> 40u)});
    set_sh(to, Pm4::COMPUTE_PGM_RSRC1, {0u, 4u << Pm4::COMPUTE_PGM_RSRC2_USER_SGPR_SHIFT});
    set_sh(to, Pm4::COMPUTE_NUM_THREAD_X, {lanes, 1u, 1u});
    const auto d = descriptor(output);
    set_sh(to, Pm4::COMPUTE_USER_DATA_0, {d.fields[0], d.fields[1], d.fields[2], d.fields[3]});
  };
  const auto dispatch = [](std::vector<uint32_t>& to) {
    to.insert(to.end(), {KYTY_PM4(5, Pm4::IT_DISPATCH_DIRECT, 0), 1u, 1u, 1u, 0x41u});
  };
  const auto call_ib = [](std::vector<uint32_t>& to, const std::vector<uint32_t>& target, bool chain) {
    const auto address = reinterpret_cast<uint64_t>(target.data());
    to.insert(to.end(), {KYTY_PM4(4, Pm4::IT_INDIRECT_BUFFER, 0),
        static_cast<uint32_t>(address), static_cast<uint32_t>(address >> 32u),
        0x0f200000u | (chain ? 1u << 20u : 0u) | static_cast<uint32_t>(target.size())});
  };
  std::vector<uint32_t> first, second, tail, nested, middle, ce;
  if (ib_ce) first.insert(first.end(), {0xc0008600u, 1u, 0xc0008500u, 0u});
  setup_cs(first, code_a, out_a); dispatch(first);
  auto& body = ib_ce ? tail : first;
  for (uint32_t i = 1u; i <= count; ++i) write(body, markers, i);
  // Guest dependency for a valid read-modify-write pair even with quantum OFF:
  // CPU marker writes do not break ComputeChain. This actual ACQUIRE_MEM packet
  // retains the CS and SGPRs while ordering A's first write before its second read.
  body.insert(body.end(), {0xc0055800u, 0x00400000u, 1u, 0u, 0u, 0u, 10u});
  // Crucial resumed packet: no re-write of A's shader/user registers after B.
  dispatch(body);
  // Actual protected CPU fringe write after two native A updates; preserve written siblings.
  write(body, out_a - 4u, fringe);
  if (ib_ce) {
    call_ib(nested, tail, true); write(nested, markers + 28u, dead_value);
    write(middle, markers + 20u, 0xa001u); call_ib(middle, nested, false);
    write(middle, markers + 24u, 0xa002u);
    call_ib(first, middle, false);
    ce = {KYTY_PM4(3, Pm4::IT_WRITE_CONST_RAM, 0), 0x40u, const_value,
        0xc0008400u, 1u, 0xc0008800u, 1u,
        0xc0038300u, 0x40u, 1u, static_cast<uint32_t>(markers + 16u),
        static_cast<uint32_t>((markers + 16u) >> 32u)};
  }
  write(first, markers + 8u, 0xa1f1u);
  setup_cs(second, code_b, out_b); dispatch(second);
  second.insert(second.end(), {KYTY_PM4(6, Pm4::IT_COPY_DATA, 0),
      1u | (1u << 8u) | (1u << 20u), static_cast<uint32_t>(markers),
      static_cast<uint32_t>(markers >> 32u), static_cast<uint32_t>(markers + 4u),
      static_cast<uint32_t>((markers + 4u) >> 32u)});
  write(second, markers + 12u, 0xb1f1u);
  std::binary_semaphore entered{0}, release{0};
  bool gate_timeout = false;
  gpu.SendCommand([&] {
    entered.release(); gate_timeout = !release.try_acquire_for(std::chrono::seconds(2));
  });
  const bool gate_entered = entered.try_acquire_for(std::chrono::seconds(1));
  if (ib_ce) gpu.Submit(first, ce); else gpu.SubmitCompute(0x20u, first);
  gpu.SubmitCompute(ib_ce ? 0x20u : 0x21u, second);
  release.release();
  bool idle = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  do {
    idle = WaitRegMemNativeQueueTestAccess::Read(gpu).idle;
    if (!idle) std::this_thread::yield();
  } while (!idle && std::chrono::steady_clock::now() < deadline);
  Require(name, "bounded real queue retirement", gate_entered && !gate_timeout && idle,
      "finite queue parser did not retire; do not continue to an unbounded idle wait");
  bool values = true, guards = true, marker_values = false, clean = false;
  uint32_t observed = UINT32_MAX;
  gpu.SendCommandSync([&] {
    flush_bounded();
    for (const auto address : {out_a, out_b}) {
      cache.ReadMemory(address, lanes * 4u); flush_bounded();
      std::array<uint32_t, lanes + 8u> actual{};
      Require(name, "actual queued native bytes", LibKernel::Memory::TryReadBacking(
          address - 16u, actual.data(), sizeof(actual)), "queued native range not published");
      for (uint32_t i = 0; i < actual.size(); ++i) {
        if (i >= 4u && i < 4u + lanes)
          values &= actual[i] == (address == out_a ? seed_a + 2u : seed_b + 7u) + i - 4u;
        else guards &= actual[i] == (address == out_a && i == 3u ? fringe : guard);
      }
    }
    std::array<uint32_t, 18u> actual{};
    Require(name, "CPU-clean marker readback", LibKernel::Memory::TryReadBacking(
        markers - 4u, actual.data(), sizeof(actual)), "marker range not readable at actual idle");
    observed = actual[2];
    marker_values = actual[1] == count && actual[3] == 0xa1f1u && actual[4] == 0xb1f1u &&
        (!ib_ce || (actual[5] == const_value && actual[6] == 0xa001u &&
                     actual[7] == 0xa002u && actual[8] == 0u));
    guards &= actual[0] == guard && actual[17] == guard;
    clean = !cache.IsRegionGpuModified(out_a, lanes * 4u) &&
        !cache.IsRegionGpuModified(out_b, lanes * 4u);
    renderer.UnmapMemory(base, bytes); LibKernel::Memory::InstallGpuResources(nullptr);
  });
  renderer.ShutdownGpu();
  Require(name, "owned mapping cleanup", LibKernel::Memory::KernelMunmap(base, bytes) == 0 &&
      LibKernel::Memory::KernelReleaseDirectMemory(direct, bytes) == 0,
      "owned guest mappings/allocation did not retire");
  std::printf("[pm4-native-state] quantum64=%u ibCE=%u Aplus2=%u Bplus7=%u "
      "guards=%u markers=%u clean=%u snapshot=%u count=%u cleanup=PASS\n",
      expect_quantum, ib_ce, values, values, guards, marker_values, clean, observed, count);
  Require(name, "two actual CS outputs/CPU fringe/guards/CE/IB/cleanup",
      values && guards && marker_values && clean,
      "actual native pipeline/bindings were not restored or writes escaped supported range");
  Require(name, "configured actual queue scheduling signature",
      expect_quantum ? observed > 0u && observed < count : observed == count,
      "other queue did not execute at the requested finite packet boundary; native outputs passed");
}
} // namespace
} // namespace Libs::Graphics
