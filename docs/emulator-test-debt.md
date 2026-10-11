# Integration test debt

## Discarded embedded-fetch offset (2026-10-11 21:56 UTC)

Independent review found a possible stale offset candidate: add s18 to v0
before the canonical CNDMASK overwrites v0, then fetch with the raw vertex
index. The discarded addition must not publish vertex_offset_sgpr=18.
Required synthetic public-translation RED checks the actual final fetch and
exact offset metadata (-1), with ordinary once-only offset controls retained.
Intended RED215803 now confirms actual raw index with native4 attributes but
incorrect offset18; exact metadata assertion fails, no timeout. Test EXE
A2EDDF4B20DE6F9FBED2BE1BF7BED92F10067D94AE2067A6CDC6C6FF8A2BE336.
Selected-offset/reselection and multiple-fetch variations are being added
before correcting the shared lifetime. No production offset-lifetime fix yet.

The neighbor branch encodings were corrected from SOPC bf05 to SOPP bf85;
the earlier join result did not exercise a CFG join. A public decoder gate
now checks the actual conditional branch/target/end instruction before the
eligibility oracle. The former boundary error was a fixture error, not a
production native-substitution failure. Corrected actual join and finite
backedge both pass215806, retaining guest fetches.

Affected shader_cfg215732 fails the empty-shader unused-types budget. A
scoped byte-preserving frontend reversal to6ba and native rebuild215829/run
215846 reproduce the SAME failure; restored WIP SHA d1a604 byte-exact.
This is an existing independent baseline failure, not a full-suite pass.
Current corrected target must be rebuilt before any future cfg execution.

Additional intended RED220004: canonical selection, valid once-only add,
then reselection discards it but publishes offset18. RED220005: two live
fetches need raw+7 and raw respectively, yet both become native attributes
under offset18 (attributes8/no shader loads). Exact metadata/second-consumer
assertions fail with EXEAB2BFC4AD7AD9F91F6D9626D47E747FF3D8A2D6BBA8FA3488015D3A2EFEE025C,
no timeout. Shared per-register offset-lifetime patch is frozen4f0c1f9d,
and unchanged all-neighbor GREEN220533 now passes: both discarded offsets
publish -1; the incompatible two-fetch case retains both actual buffer loads
and their raw+7/raw expressions. All nine original and twelve neighboring
cases pass on EXE97f31b90. No original-game causality established.

Required GPU follow-up: actual graphics pipeline with distinct synthetic
vertex records, transformed guest fetch and independent raw/high-ID exports.
Include host ResolveDrawOffsets/native baseVertex plus guest prolog offset
and mixed native/manual reads; CPU expression evaluation supplies raw input
independently and cannot prove Vulkan builtin/native-offset equivalence.
Derive guest register initialization and host addressing separately before
any new ABI correction. Masked-fetch numerical proof is now established by
unchanged real GPU oracle221933 RED (frontend6ba only) ->222119 GREEN4f0c1f9d:
wrong55667788 native raw-index sentinel becomes expected11223344 shader
index1, with full001d0001/high29 preserved. Canonical control passes both.
Host offset/builtin equivalence and a working original world remain pending.

Position tracing must include vertex-image sample results, coordinates and
sampler/T# when the selected branch depends on them. Actual2a SSA union
contains3 ImageSample operations; vertex-image CONTENTS are absent from draw
snapshots. Matching finite packed buffers alone do not prove all shader inputs
finite. No arithmetic/format/sampling correction is justified yet.

## Exported-position / transformed-index investigation (2026-10-11 21:05 UTC)

The original retry after6ba0a504 still has dispersed objects/covering surfaces.
VTX diagnostic reports actual exported NaN/Inf in68 VS at finalrun; screenshots
1006/1109 confirm visualfailure. A few finite huge/cull/zeroW events can be
legitimate clipping, and attribution to the last draw is approximate.

Before any geometry fix, capture actual buffers/index type/native fetch layout
and live shader for db7a7e115b3004a2 and2a6a418c0787e9b9. Draw snapshot MAX1
matches first occurrence and forces readback synchronization; retain this
coverage limit. Suspect32-bit index words001d0000/1/2 alone do not prove wrong
width. Valid32-bit examples also exist. Packed game metadata is not ISA proof.

Conditional required synthetic regression if transformed-fetch eligibility
is wrong: real decoded guest fetch explicitly uses index=(raw_index&ffff),
with raw001d0001. Independent memory oracle selects vertex1 while raw index
and its upper29 remain available for other guest expressions. Native fixed
attribute fetch must represent that computation or preserve a shader fetch;
never globally strip indices/change widths. Cover ordinary16/32-bit indices,
mask/BFE and supported offsets/instance cases. Prove intended RED first.
Source gap has not been established yet; no production fix is authorized by
these logs alone. Other transform/buffer/ISA causes remain open.

Read-only frontend review now establishes an independent eligibility gap:
ordinary VGPR writes do not clear vgpr_is_index, and canonical CNDMASK trusts
ABI register numbers after prior transformations. Native GetAttribute then
omits the guest index expression. Per-PC replacement exists; do not describe
it as destination-only matching. A standalone public TranslateProgram CPU
test is being constructed with canonical/offset/Local controls and explicit
masked/overwritten/ABI-transformed index variants. No intended RED or production
fix yet. Captured current-game cause remains unproved; first masked db7a loads
precede tagging and may correctly remain buffer pulls.

Native public TranslateProgram build212514 and canonical/supported-offset/
Local controls212814 passed. Intended RED212841 mask-dst and ABI-instance-shift
now prove incorrect GetAttribute substitution for transformed indices while
raw001d0001/high29 remain intact. Original immutable fixture127fa0cc is the
unchanged GREEN oracle. Shared provenance correction is in progress; actual
real-game cause and GPU-readback equivalence remain unproved. Neighbor widths,
CFG joins/branches, identity and repeated-offset boundaries remain needed.

Same original fixture213941 is GREEN for all nine cases after shared index
provenance/native-eligibility correction. CPU public translation and index
expression evaluation only; actual GPU vertex/readback remains unproved.
Extra widths/repeated-offset/control-flow neighbors are in construction.
Fresh v9 verification resolves code identity:2a primary8016/db7a2240 match
registered code. Legacy rows451/1416 are fe3, not2a; discard that earlier
misattribution. The snapshot's refound-stage/native-VB bytes can differ before
final storage synchronization; do not treat this as a proved binding bug.

Neighbor build214551 passed. Single and Local offsets214614/214615, relative
write/identity/join214617–214618 pass. Two additional intended CPU REDs:
repeated same offset214615-8640752 emits native4 for two guest additions that
one native base offset cannot represent; wide ABI214616-6579370 emits native4
after V_ADD_F64 overwrites both v4/v5 but provenance clears only v4. Eligibility
failure occurs before F64 numerical evaluation; no new FP64 semantics claim.
Shared once-only offset and actual destination-width corrections are now
authorized by these REDs. Preserve unchanged eligibility oracles.
Backedge214619 emits no native attributes and no retained fetch at expectedPC;
that failure is not proof of incorrect native substitution. Diagnose fixture/
CFG reachability independently before making any production claim or change.

## Active descriptor-retirement pixel regression (2026-10-11 20:32 UTC)

User explicitly reauthorizes game execution and tests. The fresh d7895a56
retry202640 has severe covering stretched surfaces at frame414 and missing
world/body with floating items at frame1180. This is not fixed by the earlier
table-retention and access-age corrections.

Required minimal RED: wholly synthetic loop-free native compute shader reads
a real registered red texture through both the actual translation-selected
slot and the original fixed sampled-array slot. Positive control must read
red. Record the old consumer without submitting, tag its actual ownership,
invoke actual cache invalidation/FreeImage, then submit and read back both
pixels. Expected earlier guest use retains red; host Vulkan UAB behavior
alone does not preserve the old descriptor. No GPU semaphore gate or unsafe
hang is needed. Bound the harness and retain native image/view lifetime.
Test-only construction is in progress; no production fix or intended RED
has been obtained yet. Deferred slot clearing alone is insufficient if the
translation word or physical image ownership changes first.

Construction and native build203417 completed. Intended GPU RED203642 now
passes actual opaque-red positive control, then reads RGBA0 from both old
translation-selected and direct slots after actual cache invalidation. Exact
recorded tick2 ownership assertion, no timeout/GPU gate/reset. The immutable
test is the GREEN oracle. Shared recorded-consumer completion ordering is in
progress; preparation-before-recording, batching and replacement neighbors
remain pending. Real game geometry causality is still unproved.

Unchanged native sampled oracle GREEN204246 now returns original opaque red
from both paths. Eleven-line shared whole-set use-tick/scheduler.Wait ordering
correction protects recorded consumers before descriptor AND translation
mutation. General callbacks are not drained by Wait. Batching/replacement,
no-region-use and completed/no-use boundaries are next; preparation-only and
actual original-world rendering remain unproved.

Native neighboring sampled cases205604 passed: red/green batch ownership with
only first incomplete-use wait, no-ref/complete fast paths, callback sentinel,
fresh green replacement after old physical deletes, and fixed-only blue shader
without translation/region leases. Existing affected cases205215–205220 pass.
Preparation-only and original geometry causality remain unproved. Whole-set
wait is conservative; original retry must measure behavior/performance before
claiming that current flicker is resolved.

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
Manual comparison185229 with autoswipe0/chain0 still shows covering stretched
surfaces at frames512/704; ended18:54:28 gracefully exit0. Controller log is
neutral only. An access-age oracle must inspect raw image metadata; GetImage
refreshes access and cannot be used as the observation that proves this RED.
First native attempt185759 failed fixture preparation, not a semantic RED.
Tracked direct-memory backing and cold-insertion generation setup were fixed
in the test alone. Native build190102 passed; intended RED190144 now observes
actual bindless use initial1/current34/observed1 and GC33/LRU0, while direct
RefindImage control observes34. Actual CommitBindings/cache/views and raw
PeekImage observations; no shader dispatch/timeout. Same unchanged oracle is
required for GREEN before further neighboring or visual claims.
The same unchanged age fixture passed190356 and191046 after one shared
commit-path MarkImagesAccessed call: observed34/LRU33. Actual compute/graphics
duplicate/repeated/already-read-only/target/null/unregistered/stale-ID cases
passed191010/191011 under binding-plan1/0, including zero-write early returns.
User permits tests only; no further game launches. This does not prove sampled
pixels, preparation overlap or pending old-consumer descriptor lifetime.

Fork-derived required regression: a recorded or pending native shader sampling
consumer must retain its original image/descriptor slot through actual image
invalidation until its owning GPU tick completes. Sharp926 queues null-write
and slot reuse at completion; our OnImageUnregistered rewrites shared slots
immediately despite deferred native-image deletion. Use real sampled readback,
positive controls and bounded ownership; do not claim a metadata tick test
proves old pixels or defer arbitrary callbacks without generation checks.

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
