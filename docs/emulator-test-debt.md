# Integration test debt

## Late image-backed BDA fringe: regression resolved, 2026-10-12 07:50 UTC

The actual production graphics preparation phase reproduced the defect before
changing production: `checks/20261012-074808-0238556-late-bda-production-phase-red`,
C07E7549 executable, child -1073740791. The new owner/PTE, image pixel,
ordinary SRD, preserved GPU marker and atomic counter=1 passed; the first
FLAT fringe read returned zero instead of 0x71335599.

The identical fixture (SHA256 7B870F50792AABA38291127E3651FD890447DEABE7A50C23D18950C13156CB86)
passed after final BDA synchronization following resource acquisition:
`checks/20261012-074940-4760654-late-bda-production-phase-green`, D3CC0DE5,
child 0. Graphics and both direct/indirect compute use this ordering before
snapshot/command handles/chains. Existing image-binding, first-fault deferred,
rendering/chain protocol and ordered EOP neighbors passed at 075016/075025/075026/075027.

Still pending: original game retry; direct/indirect compute integration variants;
late vertex/index owners created after graphics preparation; forced upload-ring wrap.
This image-backed reproduction does not establish the zero-matrix producer cause:
the previously captured 549 dispatches had no image bindings.


## Late image-backed BDA owner fringe (2026-10-12 07:24 UTC)

RequiredTESTONLYrealmaterialization reproduction beforeanysharedsyncfix:
earlierlegitimateGPUwrite inregisteredpageA; freshsampledlinearimage8KiB
crossesA/B; FindBuffers registersA beforePrepareBda, RebindImages mustactually
merge32KiBownerAB AFTERprepare. KnownmappedCPUfringeB+0x2000 isoutsideall
requestedimage/SRDspans. PublicCompiledguestFLAT/image/ordinaryloads andatomic
oncecounter: image/ordinaryvalues+GPUmarker/PTE/sentinels positive, Babsent
afterprepare thenPTEactualnewowner nonzero beforedispatch. Knownnonzerofringe
mustreadcorrectly onfirstdispatch, counter1 forbidsblindproducerreplay.
Noactual lateowner =>invalidsetup, notRED. FrozenB445DB3A .inc, defaultcore
unchanged. Thisgeneralimagecase doesnotprove549cause (itscapturesimages0).

## First64 matrix rows: wave grouping baseline and unresolved inputs (2026-10-12 06:16 UTC)

Newreusable fa7ee9fb TESTONLYgroupingfixture64866D5B: nativebuild060754PASS,
run061225 C41A4AE1 allsixWG64/128/512 full/sparseEXECcasesPASS, independent
logicalIDs/READLANE31/63/first/fullsentinels. No groupingRED, no productionfix.
Existingtwo-wave47/LDS tests retained. Count349 is a workquota, notWG349;
native64/generalallowedVulkan subgroup permutations remaincoverage limits.

Capturedfirst64-zero vsremaining285finite shape remains a lead. Elevenfixed
assetinputs match/nonzero first64XYZ, but42SRTwords/dynamicpalette/written
scratch differ; fullinputequivalence notproved. MissingBDAcapturepage is not
actual host-unmapped/fault proof. Requirednextcapture filtersjobcoverage2097/
2098 beforequota, keepsactual page/fault/EXEC/inputmetadata. First-useBDA
zero-until-next-frame behavior is a sourcelead, notdemonstrated549causality.
Anysharedresidency/ordering fix needs knownfinite boundedfirst-useGPU RED,
sameGREEN andneighbor atomic/side-effect safety; never blindlyreplay work.

## NGG partial native vertex-wave entry count (2026-10-12 05:30 UTC)

Update05:50: original054138 (B048335F) endedexit0/300s, severegeometry persists
in210sownedimage,FPS~3.3diagnostic/internalabove1080. CorrectNGGentry proof
stands; doesnotresolvepalettezeros orprove~60FPS. Newactualreaderrows match
before/aftercorrespondingCS, zeros predatethosewriters; missingproducerjob
coverage requiresnewdiagnosticselection, notspeculativestore/mathfix.

Update05:42: production1c9e3dfa committed; focusedCLI b026e202 separately.
Unchangedfixture053446GREEN,422records/12cases,71C43725; embedded053448PASS.
Existingmetadata/export/cacheguards053823PASS viafocusedmode(FD31DBBE).
FullCFG053704 abortmatchesolder215846/6ba0computeempty-declarationsbaseline;
fullsuite remainsred, oracleunchanged. Core053904/install054009PASS, original
054138 nowACTIVE; actualgamecausality/host64/sparseinitialmasks notproved.

Update05:32: nativebuild053046PASS,fresh96DFB5CC. SAMEfixture053122 intended
REDafterall12nativecases andcleanup: wave32S3low8=32vsinitialactive1/17/31;
wave64low8=64vs1/17/31/32. AllvertexIDs/records/sentinels/upperS3controls valid.
Productioncommoninitialpopcount correction nowauthorized for BOTH ordinary
andWaveHalves paths; preserveSGPR2capacity andupperS3group/waveID packing.
SameunchangedGREEN plusCFGmetadata/exportguards andembeddedfetchneighbors
pending. No actualgamecausality proven from this synthetic count regression.

Upstream cf77fb43 active initial ESVertCount already exists behind an opt-in.
Required loop-free native RED before any default change: public guest vertex
compiler, point-list/discard pipeline; capture initial EXEC as FIRST guest
instruction, before S3-derived mask changes, then rawS3 and VertexIndex into
bounded independently ticketed records. CPU bitloop popcount of initial mask
checks S3 low8, all requested indices/header/padding/sentinels/upperS3 fields.
Draw1/17/31/33/64/65, guestwave32/64 on actualnative32; do not equate draw count
with number of Vulkan invocations or use compute virtual-half semantics here.
Fixture05B8A4A3 no loops and8192record guard; GPU cleanup before aggregate RED.
Unrelated setup/compiler errors are not RED. Preserve merged-wave packing and
neighbor metadata/cache guards. ActualVS2a staticS3 use is observed, but neither
wrong live count nor zero-palette causality has been proved in the game.

## BC R64 texture block layout (2026-10-12 05:08 UTC)

Update05:17: committedf55ffc00. SAMElayout/GPU051227 bothGREEN (F6088272),
BC1/BC7eightlayers/twodirections/padding verified. Broadtiler358cases/264pairs
051559 PASS (0202862E) afterfixtureidentityupdate to current scheduler-lived
scratchcontract (68a5d42f/59cb8b16). Original14numericDWORDoracle unchanged,
no productiontilerchange; previous051233 obsoletegrown==reused fail explicit.
Originalgamewithnewcore stillpending.

Update05:11: build050920 PASS; fresh9FB012D0. Unchanged publiclayout
RED051015 rejects compressed R64 admission beforeVulkan. Previous050742
compile failure was only Windows small macro name collision, not RED; fixed
fixture name without oracle changes. Shared correction now authorized; same
layout/GPU roundtrip and existing tiler neighbors pending.

Upstream b7b335ac already partly present behind an OFF opt-in gate. Before
changing default public admission, run TEST-ONLY --bc-r64-texture-layout-only
against current core: BC1/BC7 block elements must be admitted for render64KB,
depth must reject compressed/multitexel and unsupported wide elements.
Independent BC1 2048x2048x8 footprint: total0x1000000/slice0x200000, block128x64.
Then bounded --bc-r64-texture-gpu-only must tile/detile BC1/BC7 67x51x8 through
actual public layouts/copies, checking every layer and padding byte. Fixtures
53CB9C63/giant858B8D61 frozen before RED; no production BC change yet. Preserve
existing format bounds; do not transplant upstream's broader layout rewrite.

## Packed buffer prefix loads (2026-10-12 05:04 UTC)

Update05:10: committed3ebf42f8. SAMEcostGREEN050649 0/1/1/2 push+storage;
GPU050651 all20fullbyte/sentinelchecksPASS after fixtureBegin setup, original
independent oracle unchanged. Strictgenericneighbor050700 knownoneULP fails
identicalold000321; fullmatrix050825/4800lanes/100requests bit-identical to
baseline001759 (SHAe167dc0e). Precision debt NOT resolved; cost/GPU port verified,
originalgame/FPS gain pending. Originalfrozen fixtures preserved under analysis.

Update 05:05: build050330 PASS, executable77E02AF3. Intended RED050428
validated all eight public modules and live stores: actual0/1/4/5 reads for
push and storage, vs budget0/1/1/2. Aborted at explicit cost oracle. GPU
baseline050433 stopped at scheduler inactive-current guard before dispatch;
this is harness plumbing, not a reproduced emulator numeric defect. Preserve
independent bytes/sentinels while establishing actual recording scope.

Upstream 13e3ca31 reuses each packed DWORD for four host-buffer prefixes.
Before adapting shared EmitMemoryOffsets, test public compilation with 0, 1,
4 and 5 real descriptors in both push-data and storage fallback placement.
Validate SPIR-V and count actual shader-data loads in the reflected offset
interval: budget ceil(memory_offset_count/4), with positive load controls and
all observable guest stores retained. Run intended cost RED before production
changes. Independent bounded GPU readback must retain prefix/suffix sentinels
and store values for 1/4/5 descriptors in both placements. This proves module
cost and correctness; actual FPS benefit remains unmeasured.

## Upstream R8 SNORM sampled texture admission (2026-10-12 04:53 UTC)

Update 05:03: committed c38f1981. Native build 045841 PASS; fresh executable
CFDC2E53, same numeric-class oracle plus seven GPU cases 050238 all PASS:
signed sample/gather, swizzle, linear, 128-thread wave32/wave64, R8/RG8 scaled
neighbors. Swizzle neighbor initially required sampled-view plumbing; original
source B37B7404 preserved, basic admission/numeric oracle unchanged, final test
5E20456B. CTest texture_r8_snorm registered; original game retry pending.

Fresh peepostarexx main730e1a3c contains7e4e142aaebd2a5169ac73cb1a91b6c8d6fe2a7f.
Local code has native R8 SNORM path behind opt-in gate but excludes sampled
format by default. Ported synthetic sampler/gather/swizzle/linear/wave32+64
fixtures plus public numeric-class oracle are frozen B37B7404. Native build
045038 PASS, unchanged gate-unset RED045239 on774906dd: classification
Unsupported0 vs expectedFloat1, before texture GPU allocation. Production
sampling admission change is now authorized; SAME GREEN and actual GPU
signed-normalized results/neighbors pending. Do not infer zero-bone causality
or working textures/FPS from this format-level proof. Other formats/attachment
eligibility and host capability errors remain intact.

## First fault binding inside deferred recording reservation (2026-10-12 03:25 UTC)

Update03:43: actual publicPrepareBindings→plannedCommit deferred DF052D1B
oracle RED033923 exactguard321 withrouted/probe/planned controls1 andno GPU
dispatch ->sameGREEN034220 exit0. Five-line earlyPrepareFaultBuffer acquisition
is WIP. Graphics active-rendering/compute pending-chain neighbors after
preparation, originalinitchecks and originalCSsnapshotretry pending.

Init683172a8 original032417 with2CSsnapshot request ended after6.38s/exit321,
before captures: commandHooks.cpp647 rejects a nested command between
ReserveRecordedCall andCommitRecordedCall. New lazy first GetFaultBuffer
records Buffer::Fill; deferred binding materializer calls this getter after
reservation (descriptors.cpp3542..3595). Do not suppress transaction guard or
omit initialization. Required TEST-ONLY regression: real first fault-resource
CommitBindings with deferred command stream active, same native scheduling/
resource preparation path; reservation mustcommit and actual producer results
remain correct. First-use clear must precede reservation, observation stayspure,
and repeat bindings preservependingfaults. Preserve existing init a27c and
transport/queue goldens. CPU command-hook invariant fixture is acceptable if
it exercises real binding path with bounded native cleanup; unrelated setup
crashes or unenabled extensions are not intendedRED.

## Initial BDA fault bitmap and readback visibility (2026-10-12 02:24 UTC)

Update03:12: unchangeda27c oracle GREEN030802 onfreshd10b98cf afterfirstGPU
binding init, poisonRegistered0/currentRegistered1. Same exactreuse/seed/pin
setup asRED025856; testcleanupcomplete. Observation/repeatbinding/laterbatch
neighbors and original retry stillpending; initproductionWIPuncommitted.

Update03:00: native controlled-reuse oracle025856 intendedRED, actual same
memory/offset/12582912bytes checked, predecessor zero+known positive poison,
new actual atomicOr positive page, first binding plus actualparser. Aftercleanup
poisonRegistered1/currentRegistered1; expectedpoisonabsent fails. Bootstrap
KHRpush_descriptor explicitlyenabled/max32; oldbuild025236 notexecuted without
it. Frozena27c1e28 oracle; production init correction now authorized, sameGREEN
pending. Never first-parser clear that would erase genuine guest work.

Independent source audit: DeviceLocal Buffer allocation (streamBuffer.cpp65–147)
does not initialize contents; FaultManager constructor has no bitmap clear.
Diagnostic explode clearing covers only the tail; first parser pass clears
words after earlier guest GPU writes may already have occurred. Initial bitmap
state is separate from proved quota RED and from unproved original page0 origin.
Require a bounded actual initialization/lifecycle regression with a controlled
nonzero allocation predecessor before changing this behavior. Clearing at first
ProcessFaultBuffer would discard genuine guest faults and is not a correction.

Parser host-visible output also needs explicit COMPUTE_SHADER/SHADER_WRITE to
HOST/HOST_READ dependency before fence signal/wait/invalidation; existing
bitmap-only barriers do not express this. Include output visibility and bitmap
read/write atomic accesses in the pending proved quota/readback protocol fix,
retain normal nonblocking scheduling and existing host-memory cache operations.

## BDA fault batch capacity and pending-bit preservation (2026-10-12 02:16 UTC)

Update02:37: unchanged726232c5 oracle GREEN022836 processes2049 bits in1023/
1023/3 lossless bounded batches. Nine quota/bit-boundary neighbors023505 and
existing EOP neighbors023506 PASS. Shared fix committed69b620a9; current native
original retry and affected queue transport refresh pending. Original page0
causality remains unproved; bitmap initialization and source4 are separate debts.

Update02:22: actual unchanged parser GPU RED022201 gives count1087/exported1023/
pending0 from2049 bits, all guards preserved.1026 requests lost; assertions
follow native cleanup. This proves quota/count/conservation defect independently
of original page0, whose actual overflowing pass was not captured. Native
shared correction now authorized; same frozen726232c5 oracle GREEN pending.

Original candidate020757 ended after29.66s/exit321 near frame292 before palette
trace/screenshots. Exact core link map and matching PE/PDB identity show
FaultManager::ProcessFaultBuffer deferred callback -> FindBuffer -> CreateBuffer;
fault page0 becomes invalid cached range0+8000. No direct EOP write in this
chain. Actual fault count/input bitmap is not captured; root cause of page0
remains unproved. Do not bypass range validation or substitute zero resources.

Separate concrete protocol lead: fault_buffer_process.comp increments a32-bit
counter before quota checking, stores only slots1..1023, clears each inputword
before returning on exhaustion. CPU callback trusts count and reads all slots
through count. Require TEST-ONLY bounded actual parser SPV regression first:
input >1023 known bits, output1024x8 bytes plus preserved sentinel guards;
dispatch only allocated inputword count; independently inspect bounded count,
exact exported addresses and remaining bits. Exported and pending sets must
equal the original unique set; repeated bounded batches must eventually drain
all bits without duplicates/loss/guard damage. No synthetic GPU stalls/loops.

Only after intended RED correct reservation/publication protocol, retain all
unexported bits for later processing and validate CPU count before reading.
Saturating monotonic reservation is a candidate; clamping away work, enlarging
capacity blindly or accepting page0 is not a correction. Runtime causality
needs actual count/bitmap evidence and a fresh original retry separately.

## End-of-pipe publication and timestamp ordering (2026-10-12 01:00 UTC)

Update 02:04 UTC: core015853/private install020135 and writer-history CPU015700
PASS. Existing giant queue target cannot yet build: stale18 image copy capacity
arguments/missing sampled expectation fixed, then missing vector fixture defs/
old DescriptorSource benchmark member found. Restore real tests, never fake
them; affected queue checks remain pending. Standalone calibrated Vulkan probe
supports required native domains/period/int64, but guest epoch and source4
bridge remain unimplemented. No original retry or picture/FPS proof yet.

Update 01:52 UTC: literal focused014742 and three registered/native-consumer/
IRQ/repeated-write/real-compute32 neighbors015005 PASS with exact data goldens.
Fixture teardown now performs normal renderer mapping/unmapping; original
two-case oracle preserved. Existing affected queue tests and accurate source4
calibrated native timestamp are still pending. Actual graphics causality unknown.

Update 01:45 UTC: actual public64 BOTTOM literal/source4 early-publication
RED is recorded in checks/20261012-012308-3986084-eop64-publication-nofault-native-red.
No gate was needed: an unsubmitted real native fill supplies bounded pending
work; TryReadBacking avoids a guest fault/readback during observation. Candidate
014416 produces correct literal observations but fails fixture teardown with
PageManager live state; GREEN and neighbors remain pending. Source4 native
clock-stage/calibration remains uncorrected. These tests do not establish EOP
causality for the original zero palettes or 4–5 FPS.

Read-only source lead: graphicsRun.cpp WriteAtEndOfPipeCommon breaks compute
chaining but writes32/64-bit values directly with memcpy at CPU packet parsing.
Source4 samples the100MHz reference clock there. Sync::RecordEndOfPipeWrite
records debug metadata/interrupt, rather than publishing the value at actual
pipe completion. A registered/protected label page might incidentally force
readback synchronization; verify that path before claiming actual early reuse.
Current game palette capture observes valid active indices2098/2097 returning
zero rows in one draw and finite rows in the next. EOP involvement is NOT proved.

Before any production correction, require a bounded public guest EOP regression:
native queue waits on a task-owned host-signalled timeline gate, then EOP32/64
and source4 timestamp. Snapshot an ordinary unregistered guest label before
releasing the gate: it must retain sentinel bytes. Signal the gate, drain and
dispose ALL task resources before asserting an intended failure; never leave
a GPU waiting after abort. After completion verify exact width/value and
timestamp units/ordering independently. Compare interrupt delivery ordering,
registered native buffer visibility and subsequent GPU reads; preserve cache
actions and queue distinctions. Missing dependencies/setup crashes are not RED.

Vulkan profiler nanosecond conversion and existing100MHz clock-scaling tests
already agree with their contracts; do not alter frequency or guest resolution
to hide the measured4–5FPS. Source4 capture must represent the required pipe
stage, not parser or delayed callback time merely because both are monotonic.

Primary stage clarification: PAL BOTTOM_OF_PIPE_TS=0x28; CS_DONE=0x2f.
Use a legal64 bottom event for a transfer producer; internal normalized
index0 differs from raw RELEASE_MEM EOP index5. The32 parser currently
rewrites toCS_DONE/6, so a transfer-only32 test is not a completion oracle.
An unsubmitted real native producer is already deterministic pending work
and avoids a semaphore gate; assert observations only after Finish/cleanup.
Existing GpuCommandLane immediate-label expectations encode the current
approximation and need contract-based reconciliation after validRED, while
their independent nonblocking-parser assertions must be preserved.

## Repeated generic buffer conversion code cost (2026-10-11 23:54 UTC)

Optimization candidate only: generic formatted loads inline the same
normalization ladder for every returned component. Require a synthetic
public-compiler fixture with1/8/32 distinct live XYZW reads from one runtime
generic slot, all outputs retained. Before executing the baseline, freeze
two independent cost budgets: additional reads grow the module by at most
1536words/read, and repeated normalization for that shared format requires
at most16 static FDiv instructions (no unrelated guest division). Any
equivalent scalar/vector/shared conversion or reciprocal lowering is allowed;
the fixture must not require a helper name or one exact opcode sequence.
If a budget already passes, it is not RED. Distinguish code-cost RED from
an emulated ISA correctness defect or a measured game speed improvement.

Before a production optimization, preserve unchanged actual GPU numerical
goldens for runtime format/stride/offset, signed/normalized/floating values,
dst_sel constants, bounds/overflow/alignment and both wave64 halves with
sparse EXEC. Never remove loads or required conversions to meet a budget.
Inspect the actual host optimization path for helper re-inlining. Record
native build and actual post-optimization size, then measure the original
warm-cache game separately; smaller SPIR-V alone is not60FPS evidence.

Cost RED000048 -> unchanged GREEN001747 is proved. Shared bodyde9a89cf
reduces FDiv8/64/256 ->2/2/2 and32-load words48632 ->37015. Actual100-request/
4800-lane measurements001124 and001759 are fully byte-identical, including
the existing strict UNorm8 endpoint discrepancy (3f7fffff versus3f800000 on
144 active lanes). All24 other cases/inactive lanes satisfy their independent
goldens. This is numerical equivalence plus cost proof, NOT a full strict
numerical pass. Guest normalized-conversion precision remains unproved;
do not silently add tolerance or infer its connection to the game defect.

NFv1 actual2a CPU002120 validates but grows to1942548 words; original retry
is withheld for that diagnostic version. Compact shared writer NFv2 must
retain the fixed6272-byte bounds and pass actual GPU/map/output controls.
The inactive-load fixture correction derives from exact emitted IR sinking
to load0/MUL0/SQRT0, not an emulator correction or proof of skipped arithmetic.

## Independent near/far clipping and bad-vertex attribution (2026-10-11 23:45 UTC)

Diagnostic lead only: ClipControl retains distinct min_z_clip_disable and
max_z_clip_disable, whereas IsZClipEnabled returns the AND of both enabled
planes. Native Vulkan chains depthClipEnable separately from depthClampEnable.
Disabling one guest Z plane therefore currently disables both host planes.
This has not been linked to the actual stretched scene; capture copied flags
on exact admitted draws first. Preserve raw-word unavailable labels rather
than reconstructing discarded PA_CL_CLIP_CNTL bits.

Before any correction, require independent synthetic plane truth-table and
bounded rasterized pixel cases: both planes, neither plane, near-only,
far-only, finite points on/inside/outside each plane, DX/non-DX clip range,
negative/zero W and existing guest clip/cull distance capacity. Guest API/AMD
and Vulkan contracts must define the oracle; a screenshot is not that oracle.
Do not apply a blanket invalid-position guard or alter arithmetic semantics.

Actual trace14 and0 exports are finite. Nonfinite reporter's heuristic lastdraw
is not exact attribution; the next metadata-only change must expose the
already-written immutable token and verify selected shader/stage/CPU token
before joining. If a selected vertex changes between runs, capture the actual
failing invocation with a bounded diagnostic protocol instead of assuming its
previous vertex ID remains stable. No new semantic RED or fix is claimed here.

## DX10 instruction CLAMP portability (2026-10-11 22:49 UTC)

Primary AMD ISA defines NaN-to-zero for the decoded instruction CLAMP under
the accepted DX10_CLAMP graphics mode; ordinary IEEE RCP/RSQ/MUL must not be
globally sanitized. Existing FPSaturate32 emits GLSL FClamp, whose NaN choice
does not guarantee that guest result on all conforming hosts. RTX may already
choose zero, so a passing hardware case is not an intended failure.

Required independent regression: wholly synthetic decoded clamped MUL/ADD
with NaN and finite/unclamped controls, actual public CompileProgram output,
SPIR-V validation and observable export evaluation under an explicitly
Khronos-permitted NaN-propagating host choice. Check numerical exported value,
not a preferred opcode pattern; accept any equivalent correct backend. A
fixture/evaluator unsupported operation is not semantic RED. Only after the
intended failure may the shared saturate mechanism change. Actual2a cause
remains unproved; its three CLAMP PCs are not all known executed for vertex14.

Intended portable numerical RED231105 is now proved after ten passing finite/
unclamped controls: clamped qNaN exported7fc12345 under the allowed host model
instead of+0. SAME frozen oracle617b09fa passes all14 cases231751 after the
shared accepted-Vertex/Pixel saturate correctionCEFDE8E5. No GPU execution
or original2a causality is claimed by this proof. Other stage DX10 metadata
and GPU runtime-value saturation remain separate unproved cases.

## Stable picture and frame-time investigation (2026-10-11 22:43 UTC)

User target is stable image and approximately60FPS after warmup. Current
clean warm-cache observation is~5FPS, not achieved. Keep resolution/internal
render size and instrumentation explicit. Profiled frame histogram saturates
at100ms; use true intervals for60FPS acceptance, not its clipped quantiles.

Next diagnostic only: exact drawtoken, actual selected vertex/instance/EXEC,
sample coordinates and returned bits, live original buffer indices/results,
preclip Position. Bound records and memory, preserve outputs, do not use the
existing PS/CS taps that replace MRT/IMAGE_STORE payloads. Probe missing PCs,
dead loads or unobserved branches must be explicit rather than invented values.

Before a codegen optimization, use independent varied format/stride/alignment/
bounds/half-EXEC numeric cases for the actual generic buffer mechanism; prove
the intended failure or cost invariant on the existing implementation. TopCS
215/214/213 emitted large older reconstructed modules but current live-module
equivalence is unproved. Do not specialize a per-lane varying format by hash.

Before reducing coherence waits/readbacks, exercise actual tracked GPU writes
and host publication: repeated read with no new write needs no redundant copy;
newer overlapping write must invalidate it; unrelated pending ticks must not
drain all work. Preserve ordered bytes/lifetimes, never return stale data to
meet a frame-rate target. Existing required synthetic ownership tests apply.

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
