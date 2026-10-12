# Performance cycle 1: checked changes and comparisons

Latest checkpoint: local `faa5d3ac`, published `a73b9592`. Native generic-conversion
build/install passed; full world runs still show roughly2.5–2.6FPS, without a
demonstrated useful improvement. User now requires **30-second title FPS samples
directly in the game**, no menus, images only after FPS growth, and three agents
proposing distinct changes while root implements/tests them sequentially.

Uninstrumented same-executable runtime experiments: direct recording mean2.532,
direct submission2.542, speculation disabled2.592FPS over first matched30s windows.
All exited0 with no guards; complete rendering and neutral input were retained.
Baseline first matched window2.435 contains a startup hitch (min1.1); later warm
title mean was2.55. These samples do not establish a useful gain or select a new
default. Exact UTC parsing and artifacts are saved in ignored performance-cycle2
`title-fps-config-comparison.json`. No images or input navigation in these trials.

Native Windows, `yotei-integration-native`, baseline `a73b9592` with c0 production
source. User now explicitly requested WSL shutdown and three-agent suggestions
with sequential root-owned implementation/testing. WSL shutdown returned0,
vmmemWSL absent, initially39.33GiB free. Older keep-WSL instructions are superseded.

## Current-world baseline

`_Build/runs/game-20261012-191212-perf-cycle1-wsl-off-baseline-1080-e5d801`
used private cloned c0 runtime, EXE SHA256
`DC6B68AD628D172B718E0359FE2A20BD0BE7BB871CD2DC2763B43D8B7BB3A2AB`.
Compile workers4, normal rendering/async-pipelines=false, neutral input,
drain30/GPUzones; unchanged8GiB physical/2GiB commit/42GiB owned guards.
Run closed19:19:53UTC/exit0 by task-owned stop-request, no guard; maxWS17.91GiB,
minphysical27.86GiB/mincommit23.70GiB. Village world visible in180/300/450s
captures. Source remained frozen throughout. Trigger captured two world frames.

Two adjacent world intervals report77presents/30s, about2.6FPS. GPU command interval
union about229ms/present, gaps160–161ms, CPU full-drain154–155ms; overlapping
categories must not be summed. GPU zones: game dispatch148–150ms, tiler26ms,
buffer copy18ms, draw18ms, image copy11–12ms. Counts are merged zone segments,
not actual dispatch counts. Main targets3328×1872/postprocessing3840×2160;
actual1080 and60FPS not achieved. Details in ignored `worldbaselinequick.md`.

## PM4 clean-byte fetching

Defect: a CPU-owned packet sharing a protected page with unrelated GPU-written
bytes forced a readback/drain through ordinary guest pointer loads. The existing
byte ownership model already supports checked clean-backing reads.

New synthetic `Pm4CleanPacketReadTests.inc` runs actual Thread_Gpu with lookahead
enabled, independent native GPU fills, output and sibling canaries. Four cases:
clean packet, GPU-written payload, cross-page clean packet, GPU-written header
changing NOP to WRITE_DATA together with new payload. All producer/setup/value
controls are checked before the expected no-unrelated-submission assertion;
every resource is finished/published/unmapped before assertions can abort.

Controlled RED192851/F1A166EF: all4setup and numerical controls pass; clean cases
advance tick1→2 and retire the unrelated sibling. Exact same fixture GREEN193537
and final restored GREEN194021/002DECF3: clean cases tick1→1/sibling remains pending;
actual GPU-owned header/payload cases tick1→2 and consume new9abcdef0 values.
Diagnostic recent-PM4 mode GREEN193610 also passes. Full hashes, commands,
source diffs and output retained in performance-cycle1 `checks/` folders.

Shared fix uses bounded per-packet clean snapshots only when protected pages are
involved. Actual producers keep ordinary fault/wait coherence; lookahead stops
at unavailable packet bytes instead of publishing them. Guest packet identity,
cursor/resume spans and COND_EXEC advancement remain original. Legacy AA centroid
bundle and unbounded PushMarker diagnostic keep original payload; predictive
lookahead conservatively stops at those bundles. No title/hash/path selection.

Neighbor194022–194023: PM4 WAIT/mask/resume, rewind/mutable resume and scheduler
timeline PASS; command-stream193613 PASS. The aggregate performance-memory suite
reaches ConservativeImagePressure and fails safe on-demand recovery. Scoped
unfixed-source control193905/81597725 repeats the identical failure, so it is a
separate existing debt; no full-suite PASS claim. Restored source timestamp was
advanced before native rebuilding to avoid Copy-Item retaining an older timestamp.

PM4 source committed locally7823f07f. Native build194211/install194318 PASS;
EXE A9907A86B7EA026256F740EC7B0CDB2B4178E591EF04A2872340690366E4A760.
Game194349-c23fbc ran360s/exit0, no memory guard, maxWS18.04GiB/minfree26.82GiB;
180/300s screenshots visibly contain character/environment/grass/fire/water.
Adjacent warm intervals73presents/30s/~2.43FPS against baseline78/~2.60.
PM4 read-fault class disappears but waits move to actual WAIT_REG_MEM consumers.
GPUbusy233–235ms versus226–228; no FPS gain demonstrated. Startup is not a fair
comparison because compatible cache/journal is warmer on the second run.
Passing regressions proves removed false dependencies, not a runtime speedup.

## Remaining proposals

Generic conversion is now a separately completed shared correction, pending its
native core/game timing. Test-only build195134 and intended RED195324/E74AEA7B:
real UInt32 helper path performs2FDiv, unsigned/signed casts, half unpack and
sign extension. After lazy structured format branches, same9-case test
GREEN195549/0ADF5A1B: UInt/Float32 paths0unrelated operations, normalized formats
one required division. Runtime format, arithmetic operands, bounds/EXEC and ABI
remain unchanged. Codegen-size neighbor195551 PASS.

Fresh native numerical baseline195325 and candidate195550 each emit all4800
lanes/100requests/25cases. Existing independent comparator reports0changed/missing/
extra lanes; both raw output SHA256
e167dc0e9bfebed2b9374a2eb1e0f14d1f714aaa7e2997f5a34dd25e6b165068.
Both measurement processes exit2 with the same144UNorm8-one mismatches. This is
exact preservation, not a strict numeric-suite PASS or a precision fix. No other
gameplay title or current-world GPU speedup has yet been tested for this correction.

- Exact compute prefetch: controlled RED192852/F1A166EF proves a valid distinct
  zero-stride permutation is suppressed after an earlier source was seen; ordinary
  nonzero strides correctly share identity. No production change yet. Keep this
  test opt-in/out of registered CTest until its correction passes. Required
  indirect/dirty metadata safeguards are separate from inline-only proposals.
- Further GPU shader candidates remain proposals: repeated raw BUFFER component
  address arithmetic and unconditional formatted DWORD windows. They require
  their own work-budget RED and unchanged independent numerical checks.
- Driver cache compatibility, broad barrier changes and subword tiler packing are
  still research proposals. Dominant current tiler formats are4/8bytes, so the
  R8/R16 packing proposal does not directly address measured world cost.

Proposals/artifacts: `_Build/analysis/yotei-performance-cycle1-20261012/`.
Sampler archive remains preserved and unapplied. No game compatibility or complete
effects claim follows from this cycle; no push or issue update during these fixes.
