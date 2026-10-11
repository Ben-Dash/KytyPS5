# Integration test debt

## Periodic texture disappearance / bindless region lifetime (2026-10-11)

Native actual-table RED181957 reproduced active mapping loss at temporary30
and983168 requested arena entries onRTX5060Ti after realGPU/image setup.
Exact expected assertion, no shader dispatch/timeout/hang; standalone target
bindless_lifecycle_tests and unchanged fixture are the GREEN oracle. Shared
safe-reclaim/lifetime correction passed the same unchanged fixture at183446,
exit0 after native build183359. Stable offsets/mappings survive the same pressure.
Preparation/growth/abort neighbors passed184423/184627 (bounds on/off). Native
feedback copies/owner reuse passed184623/184624 (bounds off/on). Visual runtime
retry184655 is active; stable textures remain unverified.
Retry ended gracefully18:49:40 exit0 on user input concern; window1848 still
shows severe stretched surfaces. Stable visual rendering remains unresolved.

Independent epoch-policy case: with KYTY_BINDLESS_KEY_BOUNDS=0, an old completed
feedback snapshot must remain rejected after numeric-region reuse. A native
test now uses real feedback copies, same-owner controls and actual arena reuse.
Scoped restoration of only the legacy conditional guard produced intended
native RED184536. Byte-exact restored unconditional guard then passed the same
unchanged test184623; old/new owner controls passed, no shader issuer claim.
Review limits: first growth pins its old region even without an earlier patch;
old growth feedback is not forwarded to the current incarnation. Pending-tick
transfer completion and actual SRT-upload coverage remain unproved.

Independent next required regression: actual CommitBindings with a registered
bindless image and no ordinary image bindings must refresh its access tick/LRU,
with a direct-binding positive control. Ordinary GC is already blocked by the
successful-resolution pin; ResolveOverlap uses access age and can retire a
pinned resource. Prove intended access-age RED before any shared correction;
held-GPU descriptor-slot retirement needs separate bounded coverage.

User reports all textures disappear and redraw. Run d8addc visibly alternates
normal world view (`window-now-1758.png`) and severely distorted surfaces
(`window-now-1756.png`, `window-now-1800.png`). This establishes unstable
rendering; controlled character movement is not yet proved.

Region exhaustion in FindOrCreateHeap calls ResetHeaps, erasing all settled
heap mappings/image references and restarting region1. No individual reclaim
or scheduler retirement protects live mappings; delayed feedback must rebuild
them. Recorded reset epochs30/57/85/111 confirm repeated resets. Earlier
prepared.bindless_patches and older recorded GPU references may retain/reuse
those regions. Mapping retention now has intended native RED/GREEN proof;
pending GPU use and late feedback require separate neighboring cases.

Required regression against actual host state: stable active heap A's resolved
mapping must survive bounded unrelated temporary-heap churn, with old temporary
users completed/inactive. No rejection of required allocations or larger limits
may substitute for reclaim. Also test allocation pressure within one prepared
stage, preserving earlier patches, and rejection of epoch-old feedback after
region reuse. Prefer a behavior-preserving CPU metadata seam; if existing
Vulkan-backed table setup is used, label incidental device setup and bound it
without shader dispatch. Preserve real capacity and fence/ownership boundaries.

## Visible texture artifacts and pointer-record provenance (2026-10-11)

Focused native RED171311 -> unchanged GREEN172445/172446 proves scalar
pointer-mask preservation and distinct pointer-member source identity. Vector
mask transformations, host/GPU readback and actual845 artifact causality remain
pending. Final mask/source neighbors and original visual retry are next.

Existing full ResourceTracking default suite172901 failed the invariant
mixed-numeric-class check. Scoped baseline173620 on0c201484 with the same v2
fixture failed identically; all five saved files were restored byte-exact.
This is a pre-existing permissive finite-candidate-to-null policy, controlled by
KYTY_STRICT_INDIRECT_CANDIDATES; strict-suite validation is still pending.
Do not suppress this failure, change its oracle or claim the default suite passes.

User requests clear/correct textures. Original retry5a4933 shows calligraphy
and later hand/cloth animation, with many requested invalid T# records for a
pointer-record image shader (labels identify artifacts, not behavior guards).
Repeated81818181/7e7e7e7e values alone do not prove root/address/dirty-memory
causality. Capture original pointer record/T#, stride/offset/target/bounds,
dirty state and feedback allocation epoch before attributing the image defect.

Independent source defect lead: TryMakePointerRecordImage strips immediate
low/high AND and zero-offset BFE without retaining their masks/width, while
host resolution follows a raw48-bit pointer. Required synthetic CPU regression:
P differs from Q=P&mask, descriptors at P+976 and Q+976 have different numeric
classes; require the guest-selected Q read/class. Run intended RED before any
production metadata/resolver correction, then unchanged GREEN and low/high
masks, extraction widths, offsets/overflow and supported pointer flavors.
Signed extraction and later GPU/feedback ownership remain explicit boundaries.
This regression is a general binding contract, not proof that the current
visual artifact uses this transformation. Native slot is occupied by5a4933.

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
