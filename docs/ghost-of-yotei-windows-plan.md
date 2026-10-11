# Ghost of Yōtei: active integration and session resume

## Latest checkpoint: CPU fixes and full batch, 2026-10-11 16:45 UTC

Tested source: HEAD`66e073b2` plus working diff; original retry is next.

- Full journal batch:2999 manifests,2770 distinct inputs, all2770 passed native
  decode/translation/emission and SPIR-V Vulkan1.3 validation in234.233s.
  Two isolated CPU workers,15s per record,600s coordinator; no failures/timeouts.
  Artifacts: `_Build/analysis/yotei-integration-20261011/batch-native-full-20261011-163652-9854636`.
  Auditor SHA256`106b5d16a10977b21c4f703d5440d8dd20d3bbb0a0e6371b7ce167b382a4d2fd`.
  Conservative explicit profile and reconstructed v8 defaults: no original-input
  equivalence, optimizer, driver, GPU execution or gameplay proof.
- Pixel journal: native RED162451 -> unchanged GREEN162914; final native
  neighbors164451 passed. Version9 preserves sample VGPR, target mask and
  provoking vertex mode; explicit8 refuses values it cannot represent.
  Runtime filename `<title>.v9.shaders` preserves previous journals.
- Volume admission: real descriptor CPU RED163408, exact4GiB, fatal321 at
  tile.cpp1348 -> unchanged GREEN164349, explicit unsupported rejection.
  Neighbor modes164350 passed8MiB/2GiB geometry and4/8GiB soft rejection;
  ordinary4GiB and strict4/8GiB requested errors still exit321.
  Green auditor SHA256`e341e7c7ffa3619a8f61ed6b440c5e4ace3f640a9105e038f4f61ab3c6a78d58`.
- Existing fork fallbacks remain experimental. Checked rejection does not
  implement oversized images or prove feedback/slot ownership. Original raw
  oversized T# was not collected in the diagnostic retry.
- Tools: `tools/prepare-shader-journal.ps1`, `tools/audit-shaders.ps1`, explicit
  profile JSON and `tools/README.md`. Runner fixtures passed dedup/resume,
  environment isolation and bounded cleanup. Failed-attempt archive is pending.
- Next: native core/private install with computed key (no diagnostic pin),
  bounded original retry, v9 cache/memory measurements and owned screenshots.
  Native slot is idle; no menu/gameplay/issue108 update from these CPU results.

## Latest checkpoint: diagnostic retry, 2026-10-11 16:18 UTC

- Native configure/build/install passed from primary HEAD`66e073b2` plus
  diagnostic-only `descriptors.cpp` diff. Private EXE SHA256:
  `d0185f62e3c41b14d9ea65b1cc40c78892f8f848c79fbaced8061cc8b4b7b723`.
  Exact diff and provenance: `_Build/analysis/yotei-integration-20261011/runtime-built`.
- Run: `_Build/runs/game-20261011-161201-integration-layout-diagnostic-6358a8`.
  Same original game, autoswipe4, eager bindless and explicit diagnostic flag;
  old source-key pin preserved compatible cache because generated SPIR-V and
  journal codec were unchanged. Clear the pin for later semantic/codec changes.
- Owned `window-1613.png`/`window-1615.png` show hand/cloth/calligraphy;
  `window-1617.png` is black. One foreground/hash-verified J/Cross input at
  16:16:13UTC. No character movement or game-scene entry proved.
- Finished16:18:25.115Z, graceful exit0 after382.69s because available RAM
  reached7.76GiB (8GiB reserve). No timeout, task32GiB guard, forced cleanup or
  GPU reset. Driver cache saved274,931,081payload bytes. Counters998VS/2031PS/549CS
  indicate observed permutations, not unique shader failures or memory ownership.
- The earlier oversized-volume fatal did NOT reproduce and no diagnostic T#
  was emitted. Passing its earlier elapsed-time/count point is not a fix.
  Required admission CPU regression remains pending; additionally inspect
  actual cache/resource memory owners before choosing any memory correction.
- Native execution slot is idle after stream drain. Next: build one CPU batch
  target for checked journal reconstruction and the minimal volume regression;
  original-input attempt capture remains pending. No issue108 update published.

## Latest checkpoint: warm introduction, 2026-10-11 15:44 UTC

Primary branch: `yotei-integration-native`, documentation HEAD
`66e073b22f06b7080962a726a866e9038c3577ae`, engine baseline
`6d04103525b24559a496612ef8959a7666ce9a20`.

- Warm original-game run: `_Build/runs/game-20261011-154145-integration-warm-intro-9a03b0`.
  Used the verified baseline EXE `b3ed953555e91759b798aced8a04af324508ceaafd25a70a34d6d34c879c96ef`,
  private saves/cache and existing `KYTY_DEBUG_PAD_AUTOSWIPE=4` input simulation.
- Owned screenshot `window-1543.png` visibly shows a hand/cloth and Japanese
  writing in the introduction. Character movement, title menu and game scene
  entry remain unproved. The earlier SETTINGS/CONTROLS screen is recorded below.
- Natural exit321 at15:44:16.550Z after150.85s; no timeout, memory guard, GPU
  reset or forced cleanup. Minimum observed available RAM16.28GiB.
- Current fatal: `layout.total_size > UINT32_MAX`, `tile.cpp:1348`.
  Symbolized path: `TileGetTextureTotalSize -> ResolveTextureInto ->
  ResolveBindlessKey -> ResolveBindlessHeapEagerly -> PrepareBindlessHeaps ->
  PrepareBindings -> DispatchDirect`. This is an eager heap walk; an actual
  GPU request for this descriptor is not established.
- Next: build opt-in `KYTY_TEXTURE_LAYOUT_DIAGNOSTICS=1` instrumentation from
  the primary workspace, retaining the fatal. Collect original/effective T#,
  layout size/mips and eager/requested context. Establish a CPU regression
  before changing descriptor/layout admission. See `docs/emulator-test-debt.md`.
- Main private build directory: `_Build/windows-integration`; private install:
  `_Build/analysis/yotei-integration-20261011/runtime-built`. The ignored
  `_Build/windows-local.cmd` initializes vcvars64 and preserves prior installs.

## Shader collection and precompilation

The existing v8 journal has2999 successful permutation envelopes and2485
distinct code/stage identities (897VS/1560PS/542CS;20,679,749bytes). Read-only
framing, all2999 XXH3 checksums and an independent mirror of the current
Decoder/Valid contract passed; native ISA/IR/SPIR-V compilation is still pending.
Input SHA256: `c741440836772903e4ac6b29f9539afc35611aeeca659eaaf4c03b95e3169aff`.
There are2770 distinct record bodies,229 duplicate bodies and453 hashes with
multiple records. Journal writes happen after success and do not preserve
failed-generation inputs or every live userdata/memory callback needed for
exact replay. v8 also omits pixel-stage `sample_vgpr`, `target_shader_mask`
and `parameter_mode`, although current compiler paths consume them. Journal
replay must explicitly report reconstructed-default scope and no original
input equivalence; new captures must preserve these fields.

1. Validate journal framing/checksums and replay an exact record index in a
   bounded native CPU worker. Keep decoder, IR, SPIR-V and driver compilation
   phases separate; do not classify an unreadable input as a compiler defect.
2. Port compatible capture/runner diagnostics from the preserved prior branch.
   Archive original attempts before Translate/Emit and fallback, including
   main/back stage metadata, effective compiler policy, userdata and ordered
   memory callbacks. Mark incomplete speculative reads explicitly. Protect
   failure capacity from successful cached permutations.
3. Batch complete inputs, group overlapping failures by shared cause, then
   prove synthetic RED/GREEN and replay the affected batch after each shared
   correction. Preserve original errors and mark fallback-dependent rendering.
4. Precompile observed variants and reuse compatible SPIR-V/driver caches.
   New scenes can reveal new shaders, metadata variants and resources; an
   upfront list cannot guarantee that all future permutations are covered.

Current `--shader-bench` always creates a Vulkan device and lacks exact
record-index selection. CPU batch isolation and pre-failure capture are still
pending. Diagnostics do not authorize fabricated resources or success stubs.

Checkpoint: 2026-10-11. Primary execution path is now the main workspace branch
yotei-integration-native based on peepostarexx/YOTEI-BRANCH6d041035.
The previous branch remains intact as a source of verified tests and mechanisms.

## Open these paths first

- Primary worktree: G:\repos\KytyPS5
- Primary branch: yotei-integration-native
- Tested engine revision: 6d04103525b24559a496612ef8959a7666ce9a20
- Previous checkout: G:\repos\KytyPS5\_Build\worktrees\yotei-windows-bringup-5664f3ff, branch yotei-windows-bringup
- Previous completed workflow commit: 85494f562070a2d245e79dea4b43931d6d5d6baa
- Previous progress/history: G:\repos\KytyPS5\_Build\worktrees\yotei-windows-bringup-5664f3ff\docs\ghost-of-yotei-windows-plan.md
- Base research/decision: G:\repos\KytyPS5\_Build\worktrees\yotei-windows-bringup-5664f3ff\docs\yotei-base-comparison-plan-2026-10-11.md
- Shared context: C:\Users\fxpw\.codex\shared-context\README.md and newest KytyPS5 handoff
- WSL access: /mnt/g/repos/KytyPS5 and /mnt/c/Users/fxpw/.codex/shared-context

A session starting in the preserved previous checkout must use the primary
worktree for new integration work. Do not replace the previous branch or merge
the divergent histories wholesale. Documentation commits on the primary branch
do not change the engine code that was tested at6d041035.

## Main workspace relocation checkpoint

The user explicitly requested G:\repos\KytyPS5 itself on yotei-integration-native.
The former baseline worktree remains at _Build/worktrees/yotei-fork-baseline-6d041035,
detached at8ae1e676, with its independent dependencies/build files preserved.
Previous source branch5664f3ff is checked out separately at
_Build/worktrees/yotei-windows-bringup-5664f3ff. Its submodules are not initialized
there yet; initialize exact gitlinks only before a later old-branch build.

All original _Build artifacts and installed binaries remain in G:\repos\KytyPS5.
Main _Build/windows and its installed EXE belong to prior builds; they are NOT
validation of this newly checked-out fork. Do not copy the baseline CMakeCache
(it embeds the detached worktree path). Configure the primary native target
explicitly using vcvars64/local wrapper and a separate build directory before
new builds; retain old outputs/caches. No native build/game was run for relocation.

## Verified state

Fork native build and run:
- Private executable: G:\repos\KytyPS5\_Build\analysis\yotei-fork-verification-20261011\runtime\kyty_emulator.exe
- SHA256: b3ed953555e91759b798aced8a04af324508ceaafd25a70a34d6d34c879c96ef
- Run: _Build/runs/game-20261011-133628-fork-6d041035-baseline-fb3974 in the previous checkout
- Visually confirmed interactive introduction with writing-names prompt and
  SETTINGS/CONTROLS menu after Enter/Options. Character movement, open world and
  title screen remain pending. Observed6.2/9.1FPS is not a measured benchmark.
- Original game01.512, RTX5060Ti/Ryzen5950X, Performance1280x720,
  bindless + linear-image readback + raw image/buffer synchronization.
- Finished/drained14:06:33UTC:1800s timeout, graceful exit0, no memory guard,
  no forced cleanup/reset, minimum free RAM9.86GiB.
- Driver cache payload265360493bytes; disk265360640bytes.
- Full evidence: _Build/analysis/yotei-fork-verification-20261011/final-baseline-20261011T141306Z.md

Previous branch diagnostic workflow:
- Native capture6/6; unchanged fused front/back audit RED -> GREEN2/2;
  missing/corrupt inputs6/6 correctly classified as input_error.
- PowerShell5.1 orphan cleanup intended RED -> unchanged GREEN.
- Corpus825 distinct partial profiles:777 passes,47 failed+1timeout in39.048s.
  These are CFG/resource planning checks, not825 successful shader generations.
- Fifteen reason groups overlap68times. Reconciled48 inputs:
  RT19, HS-front input-boundary artifact6, resources9, dynamic PC7,
  FP64 literal4, FLAT D16 1, SharedBase1, timeout1.
- Reports and test-only FP64 draft: _Build/analysis/shader-batch-workflow-20261011.
  The FP64 literal draft is unapplied/unvalidated. The new fork already has a
  literal implementation; reassess groups on the new base before choosing fixes.

Previous native capture run:
- _Build/runs/game-20261011-142826-shader-batch-capture-warm
- Engine85494f56, private EXE975bdff7786276b8c96e182c858504e27c30d1f52bd3a0f404660f425327cdf5
- Finished/drained14:59:27UTC, graceful0 after1800s timeout; minfree8.88GiB,
  no memory guard/forced cleanup/reset. Owned screenshot remains black.
- Large modules346832/521303words; driver cache payload1236842005bytes.
  These measurements suggest expensive generation/compilation; causality and
  equal-interval performance comparison still need proof.
-5215 captures:4096 compiler requests +1119 runtime variants. Cached user-data
  changes exhausted the compiler quota before late real generation143514.
  Admission follow-up is now committed as ea9225c43dce9651a293bbb338b6c5b87235877c:
  archive actual Translate/Emit attempts, defer successful cached transcripts,
  protect failure capacity and preserve exact policy/push/back metadata.
  Native capture152027 passed6/6 with cache-flood/late-generation/reserve cases.
  Static review passed; the new original game retry remains pending. The old
  private EXE975b belongs to85494f56 and does not contain this follow-up.

## Next actions

1. Review the completed capture-admission commits and their exact validation state.
   Native CPU coordinator/reserve tests have passed; original retry is pending before
   claiming recovered late runtime captures. Transfer compatible diagnostics as small
   reviewed ports to the primary branch; keep original engine behavior unchanged.
2. Retry the primary native artifact with warm cache and bounded lifetime; verify
   mouse/touchpad input in the interactive introduction, then actual character
   movement and entry into a scene. Record screenshots, input, exact revision and
   executable hash. Bindless placeholders/default samplers remain experimental.
3. After the saved plan, implement the user-requested batch collection with
   diagnostic fallback. Capture the original failure and full input BEFORE using
   a fallback; retain stage, main/back bytes, user data, host/static policy,
   push offset and bounded runtime read transcript. Batch independent inputs,
   group shared reasons and preserve overlapping counts and replay commands.
4. Prefer existing valid alternatives such as structured-control dispatcher
   fallback or the original validated SPIR-V after optimizer failure. Keep
   experimental resource substitutions explicit. Never submit invalid SPIR-V or
   unbounded GPU loops. If no safe alternate path exists, record the case and
   continue independent offline workers; do not manufacture a correctness result.
   Runs/resources influenced by fallback are collection evidence, not proof that
   the original shader or game was fixed. Keep ordinary validation/error paths.
5. Fix one shared cause per batch with a synthetic intended RED -> unchanged
   GREEN, affected neighbors and GPU checks when needed. Rerun the affected group,
   then one original game retry. Reassess the old48 groups against fork output;
   do not reimplement fixes already present in this base.
6. Update this plan, test debt and shared handoff after each meaningful checkpoint.
   Commit completed changes separately. No push without current authorization.

## Constraints and recovery

Run one native build/test/game at a time. Check active owned processes before
resuming; prior sessions can leave bounded supervisors alive. Use the local
windows-local.cmd/vcvars64 wrapper, independent dependency worktrees, bounded
hidden workers, drained streams and owned-process cleanup. Preserve old worktrees,
original installed EXE454656, original eboot5178 and user SaveData.

Artifacts under the previous _Build are local evidence and are not in Git.
Do not remove that folder as routine cleanup. Preserve the seed manifests,
captures, private runtimes and compatible caches. Git stores the plan and source;
shared handoffs store the current checkpoint and artifact locations for Windows/WSL.

Issue108 publication is restricted to the user's menu/entry milestones with
accurate tested revision/scope and PR497 linkage. The upstream fork baseline does
not prove the published PR497 head. No new issue comment or push has occurred.
