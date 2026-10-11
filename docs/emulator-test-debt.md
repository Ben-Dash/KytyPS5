# Integration test debt

## Pixel permutation journal drops live compiler inputs (2026-10-11)

Native RED162451 -> unchanged GREEN162914 proved the correction; final
neighbor suite164451 passed. Codec9 preserves all three fields, explicit8
reconstruction rejects known nondefault values, and versioned filenames retain
older journals. Optimizer/driver/runtime/memory effects remain pending.

Read-only v8 journal review found omitted `ps_perspective_sample_vgpr`,
`target_shader_mask` and `parameter_mode` in Pixel serialization, despite
current frontend/IR/backend consuming them. Required CPU regression: a
synthetic nondefault pixel permutation must preserve each field through the
unchanged Encode/Decode contract. Record intended mismatch RED before any
codec/version correction; validate defaults, supported modes, other stages,
malformed/truncated input and journal compatibility. Existing v8 inputs remain
reconstructed-default scope. Different replay/live keys and memory retention
are investigation leads; no memory-causality or performance claim is proved.

## Oversized volume encountered during eager heap sweep (2026-10-11)

CPU admission RED163408 reproduced the exact4GiB/U32 fatal without GPU work;
unchanged GREEN164349 after checked unsupported-footprint rejection passed.
Neighbors164350 passed8MiB/2GiB geometry plus4/8GiB soft rejection. Ordinary
requested4GiB and strict requested4/8GiB still exit321 with the explicit reason.
This proves planner rejection only: actual original T#, later-demand/slot
settlement, guest backing/alias ownership and GPU behavior remain unproved.
The inherited fork fallback remains diagnostic and does not implement these
oversized resources. Native artifacts are under the current analysis checks.

Pinned engine6d041035/private EXEb3ed warm run154145 with existing autoswipe4
reached interactive writing-names image, then naturally exited321 at15:44:16UTC.
No timeout/memory guard/reset; minfree16.28GiB. Fatal tile.cpp1348:
layout.total_size > UINT32_MAX in TileGetTextureTotalSize for a tiled3D volume.
Symbolized chain: ResolveTextureInto -> ResolveBindlessKey ->
ResolveBindlessHeapEagerly -> PrepareBindlessHeaps -> DispatchDirect. This is
not proof that a GPU-selected resource or shader emission failed.

First collect actual raw8-DWORD T#, extent/depth/mips/format/tile/address,
heap/key/shader and eager-versus-feedback selection context before correcting
behavior. Temporary bounded diagnostics may preserve the same failure. Required
CPU regression must exercise descriptor admission before allocation: oversized
volume during speculative/eager inspection, actual requested unsupported volume,
supported neighboring volumes and arrays, arithmetic bounds, no truncated U32
sizes or invented resources. Preserve strict selected-resource rejection and
avoid counting unrequested heap words as actual required shader work.

Artifacts: _Build/runs/game-20261011-154145-integration-warm-intro-9a03b0;
last owned image window-1543.png confirms input progress. Character movement
and world entry remain pending.

Diagnostic-only native retry161201 (EXEd0185f62, exact diff archived under
_Build/analysis/yotei-integration-20261011/runtime-built) passed the earlier
fatal point without producing an oversized T# record. Finished16:18:25UTC,
graceful exit0 at the8GiB freeRAM guard (7.76GiB observed), no forced cleanup,
timeout or GPU reset. Latest image1617 is black after calligraphy images1613/1615.
This is not RED/GREEN evidence or a semantic fix. CPU descriptor admission
regression is pending; native execution slot is idle after stream drain.

## Resource/cache memory pressure after introduction (2026-10-11)

Run161201 reached998VS/2031PS/549CS before the freeRAM guard. These counters
alone do not locate retained memory or prove a leak. First identify owners and
entry/byte accounting (program/IR/source memo, image/buffer residency, driver
pipeline cache) with bounded diagnostics. Any later eviction/variant identity
change requires independent lifecycle/aliasing/ownership regression; do not
raise limits, discard required resources or classify all variants by hash only.

Read the current checkpoint in ghost-of-yotei-windows-plan.md and the historical
G:\repos\KytyPS5\_Build\worktrees\yotei-windows-bringup-5664f3ff\docs\emulator-test-debt.md. Verify older results against current
source and executable hashes.

Pending: warm introduction input and actual character movement/world entry;
fallback collection preserving original inputs and taint/coverage; safe bounded
GPU variants; source/capture admission follow-up on the previous branch; rerun
old corpus groups against this fork before selecting ports.

The fork currently uses sampler/resource substitutions in experimental paths.
Their visual success is not independent proof of ISA/API correctness. New semantic
corrections require shared synthetic intended RED/GREEN and affected neighbors.
