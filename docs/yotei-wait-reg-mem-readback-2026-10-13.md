# GPU-backed PM4 waits resume after actual readback publication

The old WAIT_REG_MEM volatile load faults on a protected GPU-owned page and
synchronously drains the GPU dispatcher. The shared buffer cache now starts its
existing real readback, submits an open publication tick and returns pending
until authoritative dirty/armed state says CPU data is published. PM4 suspends
that guest queue before dereferencing; other queues keep progressing. Actual
values, masks, comparisons, page protection, ownership tokens and staging
retirement stay in their existing mechanisms. No cached/fake ready value.

Scope is mapped buffer-backed reads. Unknown/invalid/plain host addresses retain
old dereference/validation; image-only protection may take the old fault route.
Staging capacity pressure may still retire synchronously. A wake-token framework
was deferred: existing retry100 is microseconds (native Windows alternative may
round to1ms), not100ms. No promised universal zero-wait or FPS improvement.

## Genuine native RED and unchanged GREEN

Finite native compute writes a protected32-bit guest word; legal timeline gate
has a1-second host failsafe. Two existing PM4 executions and all actual value,
mask, suffix markers11/22, native guards, publication and cleanup controls pass
before final dispatcher wait cost. Fixture obtains the real written owner once;
an earlier clean-read handle could be an upload stream and caused setup failure.

Baseline034342/203C9D2E: all controls pass, intended costRED FullDrain/TickWait/
PriorityWait1/0/1. SAME fixture034629/BD73E808 after three-file core change:
all controls pass, both initially pending, wait budgets0/0/0.

Actual GuestGpu queue neighbor035123/153EF80B: graphics WAIT32 and compute WAIT64
both block; independent compute queue writes33 before publication. Afterwards
native source and11/22/33 markers, upper/prefix guards, GPU-clean state and cleanup
pass, no dispatcher wait. This covers real queue progression and all-blocked
completion, not merely manual calls to two parsers.

## Neighbor validation and limits

- Poll040047/80BCB8D0: old arm/new native writer retains newer ownership; CPU
  overwrite13579bdf is immediately ready without new submit; direct8-byte range
  crosses a page with mixed armed/unarmed producers and exact12345678/abcdef09;
  guards and host false-then-true comparator pass. The initial test omitted its
  scalar suffix guard seed; explicitly seeded it, leaving expected values intact.
- Existing armed typed four cases and literal span three cases035232 pass.
- Existing eight EOP literal cases035606 pass, including false mask, CPU overwrite,
  repeated store, IRQ and old/new ownership. The driver now resumes the SAME PM4
  execution until real publication then tests the actual comparator; one-call
  completion was an obsolete harness assumption. Values, fence observer and
  cost budgets remain unchanged.
- BC4/5/7 aliases and three mip-copy cases040212/213 pass with zero mismatches.

Image-only fallback and sustained ring-pressure latency remain explicit pending
cases; no other game is available to claim cross-game runtime compatibility.
Native cost tests and independent resource cases do not establish visible pixels
or game FPS. The uninstrumented TLS correction world sample mean3.033333,
min3/max3.1/24values/30s/exit0, had no meaningful gain.

All build/test artifacts and source/EXE snapshots are under ignored integration
and performance-cycle1 checks. Proposals/research:
`_Build/analysis/yotei-performance-cycle12-20261013/wait-reg-mem/`.
Next: native core/install into a new private actualPerformance runtime, one
650-gated30-second title measurement, then image inspection only after useful
gain. Preserve neutral input, full rendering, scale100 and source freeze.
