# Explicit temporal pass integration

The optional native Windows backend can process a declared HDR temporal pass
before the guest's downstream tone mapping and HUD. Default startup is unchanged.
`KYTY_DLSS_TEMPORAL_METADATA` selects an external version1 JSON declaration.
There are no game, title, shader-hash, address or executable selectors in core.
The declaration supplies semantic roles; resource formats/counts only validate it.

The provider copies the declared producer's real color, native device depth and
raw motion into private images. It associates those immutable inputs with the
same-frame reconstruction through the current color identity/generation and
actual bound native-buffer constants (immutable stream or synchronized GPU
snapshot). Current guest backing is not a substitute
for the buffer slice bound to the GPU. Unsupported storage paths, mismatched
resources, repeated serials and ambiguous producers are refused explicitly.

The original guest dispatch still produces required auxiliary values and alpha.
After it, NGX evaluates on the same device/ordered graphics command buffer.
A shared bounded compute merge writes NGX RGB into the two declared outputs,
preserving each original alpha. Downstream guest processing remains responsible
for presentation. Replacing history RGB is an explicit experimental declaration.
This initial integration adds work and does not itself establish an FPS gain.

Private images, the backend and per-use immutable descriptor sets remain owned
until scheduler retirement. Deferred Vulkan recording is drained before native
SDK calls; native state changes invalidate the renderer's cached state. Failures
preserve the original output and stop further provider evaluations. The owner
drains scheduler use before feature/image/device teardown.

The external declaration includes NGX project/runtime paths, render/output
extents, quality/preset, depth and motion conventions, explicit exposure values,
producer/reconstruction interface guards and role indices, and constant-buffer
offsets/units. Unknown keys are rejected. `pre_exposure_assumption` explicitly
labels the current experiment; auto exposure does not prove absolute exposure.
Game captures and deployment metadata stay outside versioned source.

## Native evidence, 2026-10-15

Records are under `_Build/analysis/dlss-integration-20261015/checks/`.

- Association: intended RED185348/b4756f, unchanged GREEN185423/8e50ce;
  matching tuple accepted,27 rejection cases plus first-frame/serial-gap neighbors.
- RGB merge: independent native controls pass before intended Unsupported
  RED185506/875a49; unchanged GREEN190059/a7fd38. Three downstream readbacks
  verify both RGB outputs, separate alpha, active rectangle/padding, untouched R8
  and final Vulkan validation0. Earlier185451/fbdb33 had stale implicit-layer
  manifest noise and is not the clean RED record.
- External retirement neighbor190541/23f9fa:24 completed GPU uses and releases,
  24 premature-destroy rejections,23 view recreations, exact RGB/alpha/padding,
  final validation0. No handle-keyed descriptor cache remains.
- Actual saved Yōtei input experiment185520/a018a4: two1920×1080→3840×2160 HDR
  evaluations, completed copies/readbacks, all RGB pixels finite, two independent
  first resets, runtime310.9.1.0 exact expected bytes, teardown validation0.
  This is offline acceptance; its game evaluation/presented counts are zero.
- Core build190430/345abe and install190611/4dd1c3 pass. Installed SDKON core
  SHA256 `B8305E8D2B77873E4BB9D6B77EC017FBE61E8847DB09938A4FAAFFFA10C128F5`,
  built on8326dad9 WIP. Initial190116/b070d7 compile errors were corrected and
  are not emulator regression REDs. CPU contracts190630/5b91bc pass3/3.

Game-integrated activation is now verified on sourceccd7b8a7/coreFC3467A8,
run `_Build/runs/game-20261015-201809-dlss-bound-native-view-live-81ad05`:
native NGX+RGB merges and GPU retirements startframe313/serial1; title reports
200 completed uses byframe537. Root viewed the nighttime village cinematic
with people/horses/buildings and counter46. Original game saves/cache remain
untouched in their source location; the run uses the copied activation runtime.
This proves processing in the game and visible downstream world, not full
temporal quality or controllable gameplay. Native D32S8 is exercised by this
game run with validationOFF; previous offline uploads used R32F depth.

The20s title-only window afterframe400 averages2.7667FPS (2.6..2.9), completed
DLSS64→115. A matched FPS improvement is unproved; retaining the original CS
and adding NGX does not reduce upstream render work. Exposure1/full motion
coverage/continuous history remain experimental. See the launch checkpoint
for exact executable, metadata, screenshot and test references.

Actual activation required three independent mechanism corrections: native host
ASLR with ordinary guest-memory-first startup, immutable snapshots of the actual
bound GPU constant buffer, and exact cached VkImageView metadata rather than an
unnormalized descriptor request (R32/COLOR may become D32S8/DEPTH). Exact-view
CPU RED201620/aa29ae→unchangedGREEN201654/8a43fb passes;5/5 neighboring
contracts201709/96f067 pass. No title/hash/path selectors were introduced.
