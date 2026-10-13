# Main-only call attribution diagnostic

Existing opt-in HLE counters combine all threads; Silent logs omit Main's
internalID/WindowsTID mapping. Diagnostic instrumentation adds an explicit TLS
Main role in the existing main-thread initializer, an optional counter filter,
and one identity line when counters are enabled. Default processes have no hook
or diagnostic output. No scheduler, clock or guest behavior changes are proposed.

Separate child-process environment:

```text
KYTY_DEBUG_CALL_COUNTS=1
KYTY_DEBUG_CALL_COUNTS_MAIN=1
KYTY_DEBUG_TRACE_FUNCTIONS=KernelReadTsc,KernelGetProcessTime,KernelGetProcessTimeCounter,KernelClockGettime,KernelUsleep,PthreadCondWait
KYTY_DEBUG_TRACE_LIMIT=1
```

Both counter switches use presence; unset to disable. Named samples use existing
one-per-function/five-second budgets. Explicit TraceCalls/TraceAll behavior stays
ahead of filtering; no automatic trace-all is enabled. The first return slot is
a calling-thread HLE return address; other scanned qwords are candidates, not an
unwound stack. Frequency and callsites are not CPU duration. Missing rows are
not proof of no work: mutex/semaphore/yield coverage is incomplete.

Native linked target015611 passes. Core/private install and a separate bounded
30-second in-world diagnostic are pending. Preserve same full-render Performance
profile and actual scale100, neutral input and one native process. Read-only OS
thread CPU deltas may accompany the diagnostic, without suspending threads.
Instrumented FPS is never credited as an uninstrumented speedup.

Ignored proposal/parser/sampler references:
`_Build/analysis/yotei-performance-cycle7-20261013/main-cpu-current-diagnostic.md`,
`extract-hle-call-counts.ps1`, and cycle3
`sample-thread-cpu-readonly-validated.ps1`. Preserve raw stdout and exact source
revision/EXE; boot and in-world reports must be separated by timestamps/frame gate.
