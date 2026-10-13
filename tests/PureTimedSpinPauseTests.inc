// Ignored preparation only. Include inside the existing VirtualMemoryAllocationTests
// anonymous namespace, under its Windows/Linux instruction-test platform guard.
// Existing target/API only: no proposed symbols are required to build the RED.
namespace PureTimedSpinPauseFixture {

constexpr const char* Option = "KYTY_TIMED_SPIN_PAUSE";
constexpr uint64_t CodeSize = 0x4000;

struct Environment {
  std::string old;
  bool had_old = false;
  Environment(const char* test, bool enabled) {
    if (const auto* value = std::getenv(Option)) {
      old = value;
      had_old = true;
    }
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
    Check(test, _putenv_s(Option, enabled ? "1" : "0") == 0,
          "failed to configure the timed-spin policy");
#else
    Check(test, setenv(Option, enabled ? "1" : "0", 1) == 0,
          "failed to configure the timed-spin policy");
#endif
  }
  ~Environment() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
    (void)_putenv_s(Option, had_old ? old.c_str() : "");
#else
    if (had_old) (void)setenv(Option, old.c_str(), 1);
    else (void)unsetenv(Option);
#endif
  }
};

struct Owner {
  uint64_t mapping;
  bool& cleanup;
  ~Owner() {
    Loader::UnregisterGuestInstructionPatchModule(reinterpret_cast<void*>(mapping));
    cleanup = Libs::LibKernel::Memory::FreeGuestMemory(mapping, CodeSize * 2);
  }
};

struct Result {
  // Snapshot before the fixture's flag-capture scratch operation.
  uint64_t rax, rdx, rcx, rsi, rdi, r8, r9, r10, r11, flags;
};
struct Output {
  uint64_t before = 0x1122334455667788ull;
  Result result {};
  uint64_t after = 0x8877665544332211ull;
};
using Entry = KYTY_SYSV_ABI void (*)(Result*, uint64_t);

// Independent SUB/CMP arithmetic-status oracle; no host flags/emitter helpers.
uint64_t CompareFlags(uint64_t lhs, uint64_t rhs) {
  const auto difference = lhs - rhs;
  uint64_t flags = lhs < rhs ? 1 : 0;
  unsigned ones = 0;
  for (unsigned bit = 0; bit < 8; ++bit) ones += (difference >> bit) & 1;
  if ((ones & 1) == 0) flags |= 4;
  if (((lhs ^ rhs ^ difference) & 0x10) != 0) flags |= 0x10;
  if (difference == 0) flags |= 0x40;
  if ((difference >> 63) != 0) flags |= 0x80;
  if ((((lhs ^ rhs) & (lhs ^ difference)) >> 63) != 0) flags |= 0x800;
  return flags;
}

void CheckNativeState(const char* test, Entry entry, uint64_t deadline,
                      uint64_t frequency) {
  Output out;
  const auto wall_before = std::chrono::steady_clock::now();
  _mm_lfence();
  const auto clock_before = __rdtsc();
  _mm_lfence();
  entry(&out.result, deadline);
  _mm_lfence();
  const auto clock_after = __rdtsc();
  _mm_lfence();
  const auto& result = out.result;
  Check(test, out.before == 0x1122334455667788ull &&
                  out.after == 0x8877665544332211ull,
        "the native timed-spin fixture wrote beyond its output");
  Check(test, result.rax <= UINT32_MAX && uint32_t(result.rdx) == result.rax,
        "RDTSC high-RAX clearing or EDX:EAX reconstruction changed");
  Check(test, int64_t(result.rdx) >= int64_t(deadline),
        "the timed loop returned before its original signed deadline");
  Check(test, result.rdx >= clock_before && result.rdx <= clock_after,
        "the loop produced a fabricated/out-of-bracket clock value");
  Check(test, result.rcx == deadline && result.rsi == deadline &&
                  result.rdi == reinterpret_cast<uint64_t>(&out.result) &&
                  result.r8 == 0x0123456789abcdefull &&
                  result.r9 == 0xfedcba9876543210ull &&
                  result.r10 == 0xa1b2c3d4e5f60718ull &&
                  result.r11 == 0x1020304050607080ull,
        "the timed-loop hint changed its deadline or unrelated registers");
  Check(test, (result.flags & 0x8d5) == CompareFlags(result.rdx, deadline),
        "the timed-loop hint changed the original final comparison flags");
  Check(test, clock_after - clock_before < frequency * 5 &&
                  std::chrono::steady_clock::now() - wall_before < std::chrono::seconds(5),
        "the finite native timed loop exceeded the fixture budget");
}

// Generate synthetic semantics; no proprietary game bytes/addresses are used.
// Every accepted routine has a single entry, a 16-byte pure clock loop, and a
// post-loop snapshot. The output memory operations are outside the timed loop.
size_t Emit(Xbyak::CodeGenerator& code) {
  code.mov(code.rcx, code.rsi);
  code.mov(code.r8, 0x0123456789abcdefull);
  code.mov(code.r9, 0xfedcba9876543210ull);
  code.mov(code.r10, 0xa1b2c3d4e5f60718ull);
  code.mov(code.r11, 0x1020304050607080ull);
  Xbyak::Label loop;
  code.L(loop);
  code.rdtsc();
  const size_t hint_offset = code.getSize();
  code.mov(code.eax, code.eax);
  code.shl(code.rdx, 32);
  code.or_(code.rdx, code.rax);
  code.cmp(code.rdx, code.rcx);
  code.jl(loop, Xbyak::CodeGenerator::T_SHORT);
  const std::array registers {code.rax, code.rdx, code.rcx, code.rsi,
                             code.rdi, code.r8, code.r9, code.r10, code.r11};
  for (size_t index = 0; index < registers.size(); ++index)
    code.mov(code.qword[code.rdi + index * sizeof(uint64_t)], registers[index]);
  code.pushfq();
  code.pop(code.r11);
  code.mov(code.qword[code.rdi + offsetof(Result, flags)], code.r11);
  code.ret();
  return hint_offset;
}

void Run(bool enabled) {
  const char* test = enabled ? "PureTimedSpinPause" : "PureTimedSpinPauseOff";
  Environment environment(test, enabled);
  bool cleanup = false;
  bool hint_present = false;
  {
    const auto mapping = Libs::LibKernel::Memory::AllocateProgramMemory(
        0x904000000, CodeSize * 2, Common::VirtualMemory::Mode::ExecuteReadWrite,
        "timed_spin_pause_fixture");
    Check(test, mapping != 0, "failed to allocate finite CPU instruction fixture");
    Owner owner {mapping, cleanup};
    Xbyak::CodeGenerator code(CodeSize, reinterpret_cast<void*>(mapping));
    const auto offset = Emit(code);
    Check(test, Xbyak::GetError() == 0, "failed to emit finite CPU instruction fixture");
    const std::vector<uint8_t> original(code.getCode(), code.getCurr());
    Check(test, original[offset] == 0x89 && original[offset + 1] == 0xc0,
          "synthetic fixture did not emit the redundant 32-bit self move");
    const auto frequency = Libs::LibKernel::KernelGetTscFrequency();
    Check(test, frequency > 20000 && frequency < UINT64_MAX / 5,
          "native TSC frequency does not permit a bounded short deadline");
    const auto entry = reinterpret_cast<Entry>(mapping);
    // Original behavior must be semantically correct before asserting its cost.
    CheckNativeState(test, entry, 0, frequency);
    CheckNativeState(test, entry, Libs::LibKernel::KernelReadTsc() + frequency / 20000,
                     frequency);
    Loader::RegisterGuestInstructionPatchModule(
        reinterpret_cast<void*>(mapping), CodeSize,
        reinterpret_cast<void*>(mapping + CodeSize), CodeSize);
    const std::array<uintptr_t, 1> starts {mapping};
    (void)Loader::PatchGuestInstructions(mapping, original.size(), starts, false, true);
    const auto* changed = reinterpret_cast<const uint8_t*>(mapping);
    hint_present = changed[offset] == 0xf3 && changed[offset + 1] == 0x90;
    for (size_t byte = 0; byte < original.size(); ++byte)
      if (byte != offset && byte != offset + 1)
        Check(test, changed[byte] == original[byte],
              "the scheduling hint changed another instruction or control-flow size");
    CheckNativeState(test, entry, 0, frequency);
    CheckNativeState(test, entry, Libs::LibKernel::KernelReadTsc() + frequency / 20000,
                     frequency);
    std::printf("[host] timed-spin original_state=1 patched_state=1 flags=1 "
                "real_deadline=1 real_clock=1 canaries=1 hint=%d enabled=%d\n",
                hint_present ? 1 : 0, enabled ? 1 : 0);
  }
  Check(test, cleanup, "finite CPU fixture cleanup failed");
  std::printf("[host] timed-spin cleanup=1\n");
  // Genuine expected RED: all native semantic controls and cleanup above pass,
  // then this fails because the existing patcher leaves the busy loop unhinted.
  Check(test, hint_present == enabled,
        enabled ? "pure timed-spin scheduling policy omitted its safe native PAUSE hint"
                : "disabled timed-spin scheduling policy modified the guest loop");
}

} // namespace PureTimedSpinPauseFixture

void TestPureTimedSpinPause() { PureTimedSpinPauseFixture::Run(true); }
void TestPureTimedSpinPauseOff() { PureTimedSpinPauseFixture::Run(false); }
