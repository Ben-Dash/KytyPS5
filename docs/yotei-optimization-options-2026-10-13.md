# Ghost of Yōtei: new optimization options, 2026-10-13

Native Windows, `yotei-integration-native`, measured source `ff1a70e5` with the
earlier shared corrections. Three agents researched upstream GPU, CPU and memory
mechanisms; root owns every native build, test and game comparison. This record
separates measured results, source findings and proposals.

## Current measurements

| Variant | World-title sample | Result |
| --- | --- | --- |
| Earlier ordered-literal correction |30 s,2.257 FPS | No improvement established |
| Fresh default control, native thread names |30 s,2.472 FPS | New comparison baseline |
| Existing GPU core reservation, physical ordinal1 |30 s,2.600 FPS | About5.2% above fresh control; small single-run difference |
| Static buffer formats, dynamic strides | Two bounded300s warming attempts | New permutations compiled; neither reached a complete world sample |

All completed runs retained full rendering, neutral input,1920x1080 window,
async-pipelines=false and existing memory guards. No menus or images were used.
Internal rendering was previously observed at3328x1872 with3840x2160 postprocess;
the window setting does not prove actual1080 rendering. Stable60 remains pending.

The valid20-second named CPU sample in the reserved-core run saw86 threads,
47 active, total2.707 logical-core equivalents. `Thread_Main` consumed87.46% of
one core, `Thread_Gpu`34.88%, draw speculation23.41%, GPU submission19.97%.
`Thread_Main` is named in the runtime linker's guest-entry path; its CPU includes
guest execution, HLE calls and fault handling. This is not proof that guest logic
alone is the bottleneck. Guest affinity attributes are currently stored, rather
than translated into a restriction to the first seven Windows logical processors.

One earlier sample had zero readable thread rows due to an interpolation error
in the ignored helper; it is invalid for role attribution. The corrected helper
produced the86-thread sample. A validated replacement preserves query failures,
rejects empty snapshots and guards PID/TID reuse; its self-process smoke passed.

## Why the native PS5 result suggests specific work

Sucker Punch describes GPU compute sequences for culling, allocation and draw
records, followed by CPU readback to assemble command lists. Cloth, particles
and fog also use compute; fog uses16-bit GPU arithmetic. That makes transfer
latency and translated compute cost relevant, rather than assuming everything
can stay on the GPU. These are the developer's descriptions, not profiling of
our emulator. [Sucker Punch technical discussion](https://blog.playstation.com/?p=411201).

## Ranked next options

| Priority | Mechanism and expected opportunity | Required evidence and current status |
| --- | --- | --- |
|1|Make the existing transfer-queue readback path usable without diagnostic flags, then reduce unnecessary waits for old/submitted producers.|Real two-queue RED223050 now passes all numerical/setup controls, then rejects already completed A while unrelated B stays open. Correct write provenance is required for binding-prep flush, owner migration and image copies before enabling it in game.|
|2|Submit aged recorded commands early so GPU execution overlaps CPU work.|[PR1430 readback/submission commit](https://github.com/Almo7aya/KytyPS5/commit/cdc50a836bdced1c4d0a7ed567ebfc1e6e91bc2d). Local general recording-age check is absent. Public-dispatch +8ms bounded stimulus/+2 output regression drafted; pending native RED/GREEN.|
|3|Replace proven ordinary buffer fill/copy compute kernels with shared DMA operations.|[PR1430](https://github.com/KytyPS5/KytyPS5/pull/1430), [specific DMA commit](https://github.com/Almo7aya/KytyPS5/commit/bc71a3b3cb6fbd6907f770d34778daab745bcb85). Our branch has image/meta clears but lacks this ordinary-buffer route. Public-dispatch cost fixture is drafted; actual eligible world-kernel coverage and gain are unproved. Start with fills, then copies separately.|
|4|Attribute `Thread_Main` cost to guest work, HLE, memory faults or waits; optimize the largest measured host component.|Native OS names now work with Tracy disabled. Do not infer a hot function from a thread name. Use bounded counters or sampling before changing guest priority/waits. [PR1050](https://github.com/KytyPS5/KytyPS5/pull/1050) is conditional: its starvation trigger has not been proved here.|
|5|Keep more LDS in native workgroup memory when a conservative address proof shows the actual footprint fits the device.|Native LDS already exists; only oversized declared allocation falls back to SSBO, which also forces CPU reads of indirect group counts. Separate physical footprint from guest allocation/wrap only with an all-access proof; retain fallback for unknown/high addresses. No fixture or FPS proof yet.|
|6|Run the existing EXEC-select optimization after resource planning removes consumers.|[PR1295](https://github.com/KytyPS5/KytyPS5/pull/1295). Our early pass already handles some cases. Need a real public compiler cost regression exposing the remaining late case, unchanged sparse-mask numerical controls and measured GPU benefit.|

The existing static-format profile2 passed its numerical matrix but generated
additional20–32-second driver compilations even in the second private launch.
Both300-second warm runs ended before the benchmark's frame650 gate. This has no
comparable world FPS result and is currently lower priority than the new transfer/
submission mechanisms. Preserve the new cache and default profile. A narrower
format-specialization policy could reduce variant growth, but requires its own
resource-origin proof and key/correctness tests; it is only a research lead.

The first static-format matrix compiled a fresh concrete module for each case
and wave size, before pipeline creation. It did not reuse one UInt module for
other formats. The144 differing lanes all resolve the baseline UNorm8-one errors;
the other4656 lanes agree. This proves the captured synthetic matrix, not dirty
GPU-written metadata, every addressing mode or cross-game compatibility.

## Transfers and batching need precise boundaries

The transfer queue must preserve unknown writers, open command buffers, image
publication, buffer lifetime, latest generations and CPU overwrites. Enabling
the flag alone is not a proof: existing test harnesses normally create one queue,
so a new separate-queue fixture is required. [Related upstream readback work](https://github.com/Almo7aya/KytyPS5/commit/cdc50a83).

Compute chaining was disabled earlier as a synchronization comparison, not by a
proved performance result. Current chaining skips some inter-dispatch barriers;
there is no general resource RAW tracker between chained dispatches. Before a
trial, prove disjoint outputs and producer/guest-ACQUIRE/consumer ordering with
real shaders, including BDA/image aliases and indirect arguments. A later safe
batching mechanism could omit barriers only for proven disjoint work.

Native packed FP16 is already capability-gated and enabled where supported;
"enable FP16" is not a missing feature to port. Investigate only remaining scalar
lowerings in actual hot inputs, retaining round/denorm/NaN/Inf behavior. Likewise,
much indirect rendering and resource caching is already present. The full upstream
PR bundle and SharpEmu's C# backend are comparison sources, not blind merge targets.

## Execution order and acceptance

Take the reproduced readback candidate through its ownership safety neighbors,
then implement the shared correction; compare recording-age submission as the
next independent option. Take DMA fills when accepted world patterns show greater
potential. Only an actual increase permits image inspection under the user's
rule. Keep30-second samples, serial native runs, original failed inputs and
unchanged goldens. Repeat a successful candidate once to distinguish it from the
observed run-to-run variation. Do not promise60FPS or count static work reduction
as a runtime improvement. Save each completed correction separately.

Evidence: ignored `_Build/analysis/yotei-performance-cycle3-20261013/`, native
checks `214049/214122` for names and `215050/215051` for formatted profiles;
run folders `game-20261012-214304-*-cd4b8a`, `215150-*-1d8f79` and pending
`220310-*-20df2d`. Active shader/game fallback limitations remain in test debt.

Final native focused CTest: `formatted_buffer_static_numeric` and
`native_thread_names_without_profiler`,2/2 PASS,2.11 seconds. The AGE-off queue
reproducer remains an opt-in failing mode, not a registered default CTest and not
a production fix. Initial accessor-name collision/build failure and old-binary
CLI rejection are not reproductions; corrected native RED223050 is the evidence.
All owned game/test/build processes are stopped at this checkpoint.
