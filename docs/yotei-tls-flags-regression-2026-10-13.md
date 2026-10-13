# Shared TLS MOV replacement: arithmetic FLAGS contract

Intel's instruction reference specifies unchanged FLAGS for MOV and LEA. ADD and SUB update CF, PF, AF, ZF, SF and OF. Therefore replacing a guest MOV with a callback bridge must retain these flags, including the subsequent Jcc outcome. The original SafeCall entry SUB ran before PUSHFQ and its exit ADD ran after POPFQ; TlsRegStub independently contained the same destructive adjustments. LEA computes the required stack address without changing FLAGS.

Primary sources checked on 2026-10-13:

- [Intel SDM Volume 2B, MOV, printed 4-37 / PDF page 39](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2b-manual.pdf#page=39): MOV flags unchanged.
- [Intel SDM Volume 2B, SUB, printed 4-655 / PDF page 657](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2b-manual.pdf#page=657): six arithmetic flags follow the result.
- [Intel SDM Volume 2 A-Z, LEA, printed 3-596](https://cdrdv2-public.intel.com/789581/325383-sdm-vol-2abcd.pdf): LEA flags unchanged; verified in Intel's indexed text. Full-document browser retrieval exceeds its size limit.
- [Microsoft x64 calling convention](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170#callercallee-saved-registers): callback ABI permits volatile GPR/vector changes and requires caller-provided shadow space. This does not grant a replacement MOV permission to change guest registers.

Root's actual unchanged regression: RED 032105 / F0868860: 896 observations, zero semantic/control failures, 882 arithmetic-FLAGS/branch failures. After flag-neutral stack adjustments in BOTH shared stubs, GREEN 032207 / 69026323: all 896 controls/flags/branches pass; CTest 1/1 passes. The root added a RET immediately after each fixture Call9 before baseline RED, so the separate MOV control cannot overwrite the observed bridge output. The ignored original draft lacks this fixture correction; the tracked test is authoritative.

Binary layout remains part of the contract: SafeCall callback immediate at 0x22; TlsRegStub rel32 at 9 with next-RIP 13 and output ModRM at 15; GetOffset = 0x100 + reg*32; existing 32-byte/0x90-byte capacities. Entry LEA plus NOP padding retains the original seven-byte prefix; the exit RET moves within existing capacity. Call9 relocations must be computed on final executable addresses.

Scope/debt: this proves arithmetic flags, conditional branches, GPRs, final RSP, real callback invocation and supported non-RAX destinations. It does not prove SIMD/x87 preservation, unwind behavior, red-zone canaries, R8-R15 TLS destinations or FS-patching execution. Call9 itself pushes a return address before the stub's 128-byte reservation; any guest red-zone contract needs a separate direct canary fixture. Packed unaligned relocation writes and rel32 reach limits remain existing layout/portability concerns. This correction does not establish an FPS gain; the next owned full-render title-FPS run measures that independently.
