# Yōtei temporal inputs: native capture investigation

2026-10-15, native Windows, branch `yotei-integration-native`. Diagnostic source
`b4aa51cb839cd7f66aed7ab0e6c072007b68dcd2`; installed SDK-free diagnostic core
SHA256 `BB06268F83F9F0F45351BAF07268C74F3D81826689FD974BCCD99D7C6F4D4AF4`.
It was built on `a78a6dbd` with the diagnostic changes subsequently committed
as `b4aa51cb`; its embedded title therefore says `a78a6db-dirty`.

DLSS remains **inactive in the game**. This investigation does not establish
an FPS improvement. The previously validated optional NGX backend/presenter
and this SDK-free capture executable are separate artifacts. See
[the integration contracts](dlss-vulkan-integration.md).

## Reusable diagnostic change

`b4aa51cb` adds `KYTY_DEBUG_RENDERDOC_CAPTURE_REQUEST_FILE`, an absolute, initially
absent request path. A fresh empty regular file requests one existing guest-flip
capture. Stale/nonempty/directory/symlink inputs are rejected, unavailable/busy
states remain pending, and consumption precedes the SDK callback to prevent
unbounded retries. No files are removed by the gate. The existing SDK Start/End
state machine still spans guest flips; a native-present capture alone is not
evidence of a complete guest frame.

The native renderer bootstrap fixture optionally checks a real injected Vulkan
capture. Injection alone binds RenderDoc's application API but does not establish
Vulkan interception. Portable RenderDoc1.46 requires its explicit Vulkan layer
as well as injection before the renderer is initialized. All layer configuration
is child-process scoped; no registry or driver changes were made.

Checks under `_Build/analysis/dlss-integration-20261015/checks/`:

| Check | Result |
| --- | --- |
|165443/22dcef configure|PASS|
|165455/bd5f91 diagnostic request target build|PASS|
|165521/498fac request filesystem/state tests|PASS, no GPU|
|165546/60e9fa SDK-free core build|PASS|
|165651/aa07ba independent runtime install|PASS|
|170832/2f7704 bootstrap fixture build|PASS|
|173608/d19512 current native shader audit target build|PASS|

Portable bootstrap record
`_Build/analysis/yotei-dlss-capture-20261015/layer-bootstrap-20261015T1710000393895Z`
passed: actual renderer Start0→1→End0, completed267686-byte RDC, final Vulkan
validation errors0, exact child/host exit0. This tiny bootstrap capture contains
no game frame or NGX evaluation. Earlier API-only injection did not capture Vulkan.

Initial wrapper failures (CMD relative path/console encoding and later Windows
PowerShell5 versus required PowerShell7) occurred before meaningful game work;
they are setup failures, not emulator regression REDs. Helpers now require PS7.
A task-created temporary RenderDoc UI config left by the PS5 setup failure was
preserved in that failed run's host folder; the originally absent user config
state was restored before retry.

## Actual game runs

Runs are under `_Build/runs/`; full identities/settings and exact stdout/stderr
are in each `run.json`. Full1080 output, neutral input, autoswipe disabled,
independent copied save/cache. WSL was stopped. Diagnostic captures and shader
dumps perturb timing, so these are not clean matched optimization comparisons.

| Run | Observed scope |
| --- | --- |
|game-20261015-170118-temporal-capture-438416|World/cinematic pixels confirmed; API-only injection, Vulkan capture start failed;20s diagnostic title mean3.6444FPS. Normal closure observed, exact child exit not retained.|
|game-20261015-171050-temporal-capture-59797c|Player/world/HUD independently confirmed at frame552. Explicit Vulkan capture; pre-request20s title mean3.4647FPS.30s capture deadline expired during initial-state preparation; forced task cleanup, exit-1. No completed RDC.|
|game-20261015-172259-temporal-pass-inventory-a39773|No RenderDoc; metadata trace begins at flip990 but lacks END.4208 setup rows+3071 repeats=7279 observed calls. Wide village shot confirmed, controls/character pose not matched.20s diagnostic mean3.43125FPS, exit0.|
|game-20261015-172948-temporal-targeted-inputs-61b90f|No RenderDoc; four complete targeted snapshots at frames550/551. Screenshot551 confirms player/world/HUD. Exit0, no resource guard. No complete pass-trace interval or clean matchedFPS measurement.|
|game-20261015-173950-temporal-capture-0d35c1|Player/world/HUD independently confirmed at552. Two guest-flip capture completed,5,016,579,112-byte RDC;20s pre-request diagnostic title mean3.4333FPS/18 updates. Game exit0, no timeout/resource guard. Guest-work completeness still needs replay inventory/producer analysis.|

Full-capture initial states spilled34 files/approximately15.72GiB before the first
deadline. They were preserved, individually boundary-checked and moved to
`F:\KytyPS5-capture-artifacts\20261015T1717016119020Z-59797c-partials`;
the relocation manifest remains under the analysis directory. RenderDoc's512MiB
setting is a soft spill threshold, not a capture size/RAM cap. No partial file is
an RDC or proof of a completed guest-frame capture. After the user freed space,
G: had approximately92GiB available; the bounded capture retry completed in
146.55s after request. Maximum game working set23.68GiB, minimum available
physical RAM14.99GiB, minimum free disk70.99GiB. Final capture:

`_Build/analysis/yotei-dlss-capture-20261015/runtime/_RenderDoc/kyty_1792086155471264_capture.rdc`

SHA256 `98F8570B80A5E2E577EE6D099F96EFE40D65551F94EC2E5B4079653964DBCE45`.
The first metadata replay is separately bounded and must not be run alongside
the game. A completed SDK capture file does not alone certify its guest-work
coverage, resource roles, complete history or game DLSS.

## Verified targeted data and CPU audit

Artifacts:
`_Build/analysis/yotei-dlss-capture-20261015/targeted-postprocess-20261015T1729481748933Z/`.
There are two snapshots each of `5e221bbbc179b208` and `d42889155b0a6b9f`,
with original live SPIR-V, metadata, constants and native image input/output bytes.
Hashes/addresses identify diagnostic captures only; no core behavior selects them.

The captured preparation dispatch is240×135 groups of8×8 threads, actually
1920×1080. Some outputs have3840×2160 allocations, but the shader stores at the
original render pixel coordinates; allocation extent alone overstates its work.
The later accumulation dispatch is480×270 groups of8×8, actually3840×2160.
Thus a1080 window does not remove the game's actual4K temporal pass.

The saved144-byte constant block changes subpixel offsets between550 and551;
the latter retains the earlier offset as previous. The accumulation shader adds
current offset/renderSize to normalized output pixel centers. Previous-frame
RGBA16F feedback is independently byte-confirmed: frame551 input matches
frame550 output at the corresponding ping-pong resource. Projection injection,
history-reset semantics and complete motion coverage still require proof.

Preparation selects the maximum device depth in a3×3 neighborhood, fetches the
two-channel input there, multiplies it by captured(-1,-1), and adds the result
to normalized pixel coordinates for previous-frame reprojection. This proves
the consumer's normalized backward-displacement interpretation and reverse-depth
ordering. It does not alone prove every camera/object producer or jitter policy.

Frozen v9 journal `66670c05c1f82601e2f6b10ee2489aec4e361382fd16b8ba97d8ca385202ca65`
contains1983 complete checksum-verified records. Selected six PS/VS hashes and
all12 permutations passed native CPU decode/IR/emission and Vulkan1.3 SPIR-V
validation. The two captured compute permutations also passed. Saved outputs:

- `motion-cpu-records-20261015T173645390198Z/`:12 PS/VS variants.
- `motion-cpu-records-20261015T174059440995Z/`:2 compute variants.

These directories are under the same ignored analysis root. Profiles explicitly
reconstruct captured host globals (bindless/hardware bounds/nativeFP16 true,
table false,26 environment switches null); original PS/VS userdata and live
modules are not established merely by these CPU passes. No guest bytes are
included in versioned tests or documentation.

## Verified math and remaining integration gates

Prove actual camera/object producer matrices, UV/sampler conventions, projection
jitter and reset/exposure semantics. The observed camera-fill PS preserves finite
motion and fills invalid lanes from a12-coefficient mapping of(u,v,depth,1);
the actual coefficient values were not part of the two compute snapshots.
High HDR values in the color image do not establish its precise scene-linear or
pre-exposure convention. Do not guess1.0 exposure or manufacture missing data.
The selected accumulation pass's1×1RG32F input is actually zero and its own
guard consequently selects working exposure1.0; this observed shader fallback
does not establish the absolute upstream pre-exposure. A nonunit history RGB
correction is also present. The shader temporarily compresses HDR triplets and
inverts that compression before output, without final gamma/PQ conversion or
clamp to1. These facts narrow the working-color analysis but do not establish
the entire upstream color contract. NVIDIA distinguishes auto exposure from
the per-frame pre-exposure input in its
[DLSS programming guide](https://github.com/NVIDIA/DLSS/blob/main/doc/DLSS_Programming_Guide_Release.pdf).

Offline numeric checks independently evaluate2349 deterministic spatial samples
in each saved frame: every prepared motion vector exactly matches the negated
raw vector at the selected nearest-depth neighbor, and every selected MAX-depth
value matches exactly. Linearized center depth and luminance relative errors
are below0.001, consistent with their captured FP16 format. This verifies those
sampled roles; it is not exhaustive or cross-game coverage. Detailed ignored
evidence: `temporal-math-report-61b90f.md` and
`5e-saved-math-check-v3-61b90f.json` under the capture analysis root.

Both compute CPU-regenerated modules also **exactly match the original live
SPIR-V bytes** in both snapshots and runtime dumps:

- Prep:71636B, SHA256 `5325420594cb0a70b3ca6715672a4835ae9b40114e602338ffc5690e48fa2862`.
- Accumulation:576280B, SHA256 `85db60b62a5035508a89e7f08313ca6978683052f4c38637fcf0775981775947`.

This removes the profile-equivalence uncertainty for those two compute modules,
while original PS/VS binding and constant limitations remain.

The actual captured camera-fill module has no PushConstant variable. Its12
coefficients come from StorageBuffer descriptor set1/binding109, flattened SRT
indices16..27: descriptor-relative bytes[64,112). Both retained128-byte pushes
at its two draw records are zero, and they are not this matrix. Next resolve
the actual descriptor buffer/offset/range and its correct upload/submission
generation; do not interpret zero pushes as zero camera coefficients. Image
and sampler/vertex payloads also remain explicit requirements. Original guest
SRT byte offsets256..300 and compact host buffer offsets64..112 are distinct.

Then prove a generic semantic resource/lifetime provider with independent
camera-only/object-only/depth/jitter/reset regressions. Insert HDR NGX before
the real downstream tone mapping and HUD; the existing SDR presentation bridge
correctly rejects scene HDR. Replacing an existing upscale after expensive work
does not itself establish a reduction of earlier render workload. Require real
evaluated/completed/presented output and matched20–30s world titleFPS before
claiming game DLSS, image quality or speed improvement.

## Capture analysis workflow

The first GPU `OpenCapture` returned Success, but after approximately157s the
script's120s cooperative deadline expired before exporting inventory. Both
controller/capture Shutdown returned, host exit0 and output streams drained;
the result is explicitly failed, not a completed inventory or GPU hang. Logs:
`replay-host-20261015T1745388557919Z` and `inventory-0d35c1.json`.

Public1.46 `CaptureFile.GetStructuredData()` supports a separate CPU decode
without `OpenCapture`/`SetFrameEvent`. Initial actual decode took11.656s and
returned157538 command chunks/25489 opaque buffers; subsequent export exposed
an unavailable Python binding (`IsNULL`), so that attempt failed and was preserved.
V2 exports shader/pipeline creation metadata but hits an overbroad push-history
guard. V3 resolves linked graphics pipeline libraries and restricts push history
to the selected exact layouts, keeping the128-key bound. It returns API export
PASS:12.61s decode/6.687s scan,1115shaders2356pipelines,28direct+49linked selected
pipelines and174 raw work/push samples,1.44MiB artifacts. Original capture stat
unchanged, host exit0/streams drained/no guards. Record:
`structured-0d35c1-v3.json`, frozen helper SHA73792410D77336BF476DD5EC9160D78C9735B800DE16277E2185EABC1FE800B7,
host `replay-host-20261015T1756239368898Z`.

Review then finds a diagnostic association defect: the BeginCommandBuffer
serialization field is capital `CommandBuffer`, unlike bind/push fields.
V3 reads the latter casing for begin too, so its generation reset is initially
unproved. V4 corrects this and validates66 observed old samples, but retaining
unrelated regular descriptor sets hits its4096-cell bound. V5 first resolves
the four regular sets actually bound at the eight selected calls, then retains
only those cells. It keeps both128/4096 bounds. Final V5 export passes:
12.437s decode/10.86s scan,174/174 previous samples have the same pipeline and
pushes inside the correct recording generation, all8buffer bindings resolved,
no unobserved query, clean shutdown/host exit0/no guards. This repairs the
diagnostic lookup; it is not a discovered guest defect or verified execution
ordering. Evidence: `structured-0d35c1-v5.json`, helper SHA256
`D9C301AE9F16A4D9B37F615A254CBF54379988E10065AE468FB4E710A8562638`,
host `replay-host-20261015T1809554987906Z`.

Camera matrices resolve to VkBuffer194 at bytes[57066048,57066096) and
[61334080,61334128). Same-capture accumulation CBs resolve to VkBuffer896713
offsets11776/12544,144B each. Representative geometry A/B/current/other matrix
buffers are also located; no matrix values have been read yet. These are buffer
offsets, not device-memory offsets.

Pinned RenderDoc1.46 Python bindings copy an entire selected opaque blob before
a Python slice. Therefore do not claim a1KiB read bound by slicing a potentially
large InitialContents blob. Host writes before draw recording also do not prove
values visible at submission. Use explicit small `GetBufferData` after selecting
the correct replay event and verifying its actual descriptor instead.

Two selected-read attempts fail closed before any buffer read: CPU structured
chunk indices differ from replay indices, and raw timestamp/duration metadata
uses different time bases. Both replay loads succeed in15.8/16.1s, with clean
handle shutdown/host0/no guards; this warm load timing is not a gameFPS result.
The required correlation is captured baked recording plus original work-call
identity, with ambiguity rejected. Preserve the failed records
`selected-constant-bytes-0d35c1.json` and `...-v2.json`; do not treat their guessed
numeric event associations as confirmed data.

All workers have outer180s/process-memory/disk guards, fresh outputs and
task-owned cleanup. Command recording order must not be presented as proven
GPU execution order or alias generations; mapped writes can occur after draw
recording but before submission. No matrix or semantics is fabricated.

No new game issue milestone was reached and no issue comment/push was made.
Preserve saves/cache/game data, all captures and the user's untracked
`tests/ComputePrefetchPermutationTests.inc`.
