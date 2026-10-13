// Optional ignored neighbor include, AFTER PureTimedSpinPauseTests.inc.
// These reject cases are decoded/patched only; no unsafe loop is executed.
namespace PureTimedSpinPauseFixture {

enum class Boundary {
  AlternateInteriorEntry,
  UnresolvedIndirectFlow,
  MemoryReadInsideLoop,
  ChangedDeadlineInsideLoop,
  WrongSelfMove,
  InteriorFunctionStart,
  UnreachableEmbeddedLoopBytes,
};

void Boundaries() {
  const char* test = "PureTimedSpinPauseBoundaries";
  Environment environment(test, true);
  unsigned checks = 0;
  for (const auto shape : {Boundary::AlternateInteriorEntry,
                           Boundary::UnresolvedIndirectFlow,
                           Boundary::MemoryReadInsideLoop,
                           Boundary::ChangedDeadlineInsideLoop,
                           Boundary::WrongSelfMove,
                           Boundary::InteriorFunctionStart,
                           Boundary::UnreachableEmbeddedLoopBytes}) {
    bool cleanup = false;
    {
      const auto mapping = Libs::LibKernel::Memory::AllocateProgramMemory(
          0x904000000, CodeSize * 2, Common::VirtualMemory::Mode::ExecuteReadWrite,
          "timed_spin_boundary_fixture");
      Check(test, mapping != 0, "failed to allocate finite CPU matcher fixture");
      Owner owner {mapping, cleanup};
      Xbyak::CodeGenerator code(CodeSize, reinterpret_cast<void*>(mapping));
      Xbyak::Label loop, interior, indirect;
      if (shape == Boundary::AlternateInteriorEntry) {
        code.test(code.rsi, code.rsi);
        code.jnz(interior);
      }
      if (shape == Boundary::UnresolvedIndirectFlow) {
        code.test(code.rsi, code.rsi);
        code.jnz(indirect);
      }
      if (shape == Boundary::UnreachableEmbeddedLoopBytes) code.ret();
      code.L(loop);
      code.rdtsc();
      code.L(interior);
      const auto interior_offset = code.getSize();
      if (shape == Boundary::WrongSelfMove) code.mov(code.eax, code.edx);
      else code.mov(code.eax, code.eax);
      if (shape == Boundary::MemoryReadInsideLoop)
        code.mov(code.r10, code.qword[code.rdi]);
      if (shape == Boundary::ChangedDeadlineInsideLoop) code.inc(code.rcx);
      code.shl(code.rdx, 32);
      code.or_(code.rdx, code.rax);
      code.cmp(code.rdx, code.rcx);
      code.jl(loop, Xbyak::CodeGenerator::T_SHORT);
      code.ret();
      if (shape == Boundary::UnresolvedIndirectFlow) {
        code.L(indirect);
        code.jmp(code.rax);
      }
      Check(test, Xbyak::GetError() == 0, "failed to emit CPU matcher reject fixture");
      const std::vector<uint8_t> original(code.getCode(), code.getCurr());
      std::vector<uintptr_t> starts {mapping};
      if (shape == Boundary::InteriorFunctionStart)
        starts.push_back(mapping + interior_offset);
      Loader::RegisterGuestInstructionPatchModule(
          reinterpret_cast<void*>(mapping), CodeSize,
          reinterpret_cast<void*>(mapping + CodeSize), CodeSize);
      (void)Loader::PatchGuestInstructions(mapping, original.size(), starts, false, true);
      Check(test, std::equal(original.begin(), original.end(), code.getCode()),
            "the scheduling policy changed a loop without a complete safe proof");
    }
    Check(test, cleanup, "finite CPU matcher reject fixture cleanup failed");
    ++checks;
  }
  std::printf("[host] timed-spin matcher rejected=%u cleanup=1 unsafe_execution=0\n", checks);
}

} // namespace PureTimedSpinPauseFixture

void TestPureTimedSpinPauseBoundaries() { PureTimedSpinPauseFixture::Boundaries(); }
