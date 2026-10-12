# Yotei effects: verified mechanisms and runtime limits, 2026-10-12

Native Windows, G:\repos\KytyPS5, branch yotei-integration-native. User visually
confirms old character/object artifacts gone on5b231fdf/B284. Active work is
shadows, vegetation, water, particles, wind, volumetric fog, fire and reflections.
Stable actual1920x1080/~60FPS and complete effect rendering remain unproved.

## Completed local corrections

| Mechanism | Commit | Intended failing-before | Same passing-after and neighbors |
|---|---|---|---|
| Partial GPU buffer writes into cached BC arrays |0f40da8a12e15720034f10ca04db599b1c54af6c|135900/B4C2:4 stale BC7 consumer DWORDs; nativeB/CPUold/guards pass. Flag1 comparison135902 loses priorGPU C.|141815/EB1D unchanged BC7 passes;141816 all8 BC4/5/6HU/7 priorC variants pass;141818/141819 old alias/tail pass.|
| Publication statistics lock ownership |part of0f40da8a|144435/CE17:stats enabled, deterministic TrackingSpinLock recursive-owner rejection on priorGPU C.|144626/912D same8cases pass with statistics;144900/6F85 actual DCC-clear+metadata oracle passes with accounting.|
| HTile clear state over registered121-layer extent |63c97d047123b4aa2ce96ee92165a4f64c752510|141111/160C state32/121;141113 samebinary native31 clear works,32/120 do not, untouched controls pass.|142528/4F1A state121/121;142529 native31/32/120 clear, laterGPU0.75 preserved.143534 registration/growth/memo/bounds pass.|
| Basic alpha coverage and required pixel-stage retention |14f2a88648205f4929e9294427dc2e024af210dd|152819/5349 hardened32cases:enabled alpha0 writes depth0.25 instead of preserving0.75 at1x/4x; all28other controls and canaries pass.|153233/9FB1 same unchanged hardened32cases pass.153721/0BB5 additional64 Z_ORDER0/1 variants pass;153722 pure60 request-state cases pass.|

All evidence directories have their full executable hash, exact command, source
revision, timeout/cleanup and output in `_Build/analysis/yotei-integration-20261011/checks/`.
Before-fix/passing-after tests include real producer and untouched-memory controls;
setup/compile failures were recorded separately and never counted as RED.

BC preparation publishes the complete GPU image backing before a partial overwrite,
then refreshes all actually overlapping BC consumers. Explicit image/metadata
publication avoids recursive preparation and keeps exact DCC accounting under the
already-owned texture lock. It does not enable writable FLAT/BDA ownership.

HTile stores words according to the actual registered layer extent, not another
fixedcapacity. Clear/expand/touch/memo preserve high slices, consumed clears and
strictbounds. A smaller view retains known allocation size. The old synthetic
retirement case expected slice1 of a one-layer fixture; it now registers real2layers
instead of depending on invalid bits.144215 corrected fixture and143536..143540
DCC/layered/storage/depth/358GPUtiler neighbors pass.

Alpha coverage uses decoded enable/override and valid noninteger COL0 alpha. It
retains the PS for depth-only rendering, prevents normal early Z from committing
before coverage, includes rawstate in speculation identity and adds coverage to
monolithic/static and both fragment/output library keys. Nativeprovider requires
actual floating logicalEXP0 alpha atLocation0/Index0 with alpha channel preserved;
sparse/remappedsource and forcedDEPTH_BEFORE_SHADER remain explicit unsupported
errors. Exact AMD fractional/dither/EQAA thresholds are **unsupported**: dynamic
fractional pixels use a logged diagnostic host approximation, not correctness proof.
CPU predicate tests for formats3/6 do not prove those unsupported export formats'
shader emission or GPU behavior. Only float endpoints0/1 at1x/4x are numerically
proved here. Source-routing/forced-depth rejection fixtures remain test debt.

Native focused CTest150447:8/8PASS,5.53s. Final affected CTest154203:11/11PASS,7.59s,
including32endpoint/64early-neighbor/60CPU requests. Executables9A1146AB(bindless)
and0BB5B8B6(shader), fullSHA256 in run.json. Additional153722 sampled depth/comparison/
attachment-feedback/A16address and153723 color/1D target neighbors pass. No other
original game is available for live cross-game validation; synthetic cases are
not cross-game gameplay proof. Known baseline full-suite failures remain.

## Leads closed or still pending

- Counted multi-draw: firstfixture used wrong BDA opcode and a readonly stream
  snapshot as count witness. Corrected before/after PeekPage readback proves all10
  ordinarySRD indexed/nonindexed N0/1/2/3/5 cases already correct on baseline143828.
  Dedicated mode144216 andCTest pass; no productionCOUNTfix. BDAstore rejects at
  explicit writableFLAT/GLOBAL tracking guard before execution; not COUNT RED.
- Existing NativeIndirectDispatch descriptor-sync assertion fails both new143536
  and scoped production-absence143827. Numerical semantics/source attribution
  remain pending; no fullsuiteGREEN claim or speculative correction.
- Dynamic sampler planning152233/C7BD: intended CPU RED after valid image/control
  planning: genuine dynamicS# replaced by defaultsampler. Exactroots/16byte stride/
  selectors/materialization/reference/bounds and real wrap/clamp GPUfixtures are
  preserved. Opt-in --dynamic-sampler-only reproduction does not introduce a new
  default test failure. No sampler production correction yet. Same-frame bound
  shrink/CPUchange/GPUwritten or unreadable heaps require additional coherence
  regressions before enabling dormant hostmirror. Effect attribution is pending.

## Original-game comparison and current memory constraint

B284/5b231fdf baseline131752 completed900s/exit0, rendered village/character and
in-gameSETTINGS(window780). Manual133526 exited13:38:50UTC/0 after user inspection.
Captured guesttargets4K despite1080window; title readings~2.5..5.6FPS are not a
60FPS benchmark. Some reeds, fire basket and embers visible; vegetation density/
type, water(flatteal ROI), wind/volume/particle/reflection quality not established.

First new BC/HTile runtimee776 EXE9A17, native build/install/help pass. It shares
source-generated shaderkey387dd6fe with prior runtime; compatible private cache
and savedata copied, originals retained. Three neutral native retries:

| Run suffix | Outcome |
|---|---|
|150515-bc-htile-effects-234af3|Stopped gracefully15:06:15UTC/0 at physicalfree7.42GiB, loadingonly frame186/title8.0. BC6H102layerreflection array has actual nonbase buffer invalidation +0x6000/0x10000; not a visual claim.|
|151125-cache-reclaimed-b46c66|Stopped gracefully15:12:06UTC/0 at7.88GiB, loadingonly frame408/title14.5. Linux clean cache released after sync; no process stopped, insufficient host headroom.|
|151503-lazy-pipelines-16e377|KYTY_DEBUG_REPLAY_PIPELINES0 retains journal modules/recording but creates actual used GPU pipelines on demand. Stopped gracefully15:15:30UTC/0 at6.48GiB; emulatorWS10.66GiB. No visible effects comparison.|

WSL independently restarted and occupies~20GiB Windows working set. User now
explicitly says **leave WSL running**. Do not shut it down, restart it, change its
memory limit or stop other tasks. Unchanged8GiB physicalfree/2GiB commitheadroom
and42GiB ownedprocess guards remain. Next lower-memory native profile should use
an empty private scene journal while preserving the archive, native driver cache,
shader recording and required on-demand work; do not disable correctness checks
or drop guest draws. New alpha source key differs; old journals/nativecache gates
must remain strict. Newalpha native package/game verification pending.

## Authorized milestone publication

Latest five issue108comments read; existingOct5menu milestone retained. Native Git
credential identity verified asfxpw and publishedPR497head5664f3ff before posting
one authorized game-entry milestone:
https://github.com/KytyPS5/KytyPS5/issues/108#issuecomment-6126916747

It reports5b231fdf/B284 realworld+character+SETTINGS, bounded/manualcleanexit,
local-only versus publishedPR/CI scope, lowFPS/4K and remainingeffects. No newfix/
intermediate post is authorized. No push performed.