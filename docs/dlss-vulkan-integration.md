# Optional Vulkan DLSS Super Resolution

Status (2026-10-15): real native SR backend and actual presenter path validated
on RTX5060Ti; game activation remains pending. This is
host rendering functionality; it is not a diagnosed PS5 emulation correction.

The native Windows backend uses the public NVIDIA NGX Vulkan API directly.
Super Resolution needs color before temporal upscaling/HUD, depth, complete
camera-and-object motion, known render-pixel jitter, exposure convention and
history reset. NGX SR does not itself require camera matrices when the motion
texture already contains camera motion. These inputs must describe the same
real guest frame and remain alive until the producer's GPU work completes.

`DlssVulkan` owns NGX feature/parameters; its owner drains GPU work before
destruction or reconfiguration. All calls on one instance are serialized by
that owner. `Presenter::PrepareTemporalFrame` accepts a created shared backend,
same-device semantic resources, monotonic guest frame ID and owning resource
lease. It drains deferred CPU command recording before NGX, retains the lease
through the producer tick even on SDK failure, invalidates cached graphics
state, copies to a presenter-owned frame and restores the borrowed output layout.
Repeated presentation reuses that prepared frame and does not reevaluate DLSS.

Current presenter bridge is limited to **display-ready, perceptually encoded
LDR** (`hdr=false`). The SDR swapchain has no scene tone mapping; HDR requests
are explicitly Unsupported. The standalone backend supports HDR for an earlier
insertion point followed by actual game postprocessing. This distinction follows
the pinned SDK guide3.1.2 and has a failing-before/passing-after guard test.

## Build and dependencies

Default builds use `KYTY_DLSS=OFF` and need no NVIDIA SDK. Opt-in builds require
an external NVIDIA/DLSS checkout through `KYTY_DLSS_SDK`. SDK headers, libraries
and runtime DLLs are not vendored or redistributed by this change. The current
native target uses the dynamic release CRT, matching `nvsdk_ngx_d.lib`; builds
that explicitly select the static CRT need the corresponding `_s` loader.

Local commits: backend360fc774, device negotiation/presenter proof34e725f4,
shared allocator teardown84a9c1ec. Default SDK-free core build and three CPU
tests pass on84a9c1ec (085856/bbf444 and085958/516e65). Native build directory
is left with `KYTY_DLSS=OFF`.

Optional ON core + verified GPU executables are preserved separately in
`_Build/analysis/dlss-integration-20261015/runtime-emulator-optional`.
Installed core SHA25645F8FDE3C20086900BDAAA615CDF86930D53D4C72FFC3002537E1DDCFB6C6460;
presenter fixture AF8ED3171609574C66D7308723427233EBAE1E49D31033E2A270B050588019FC.
They were built while HEAD was360fc774 with the later committed production
changes present; final OFF core embeds84a9c1ec. No game launch used these artifacts.

Local validation SDK: NVIDIA/DLSS revision
`374959484e79a640feaba44c93ac8cfb0a03f5b5`. Release runtime file version
`310.9.1.0`, SHA256
`3975567B8943C53ACCE397F2B72380092F84F162D00B0D2C7D08A1025C563983`, NVIDIA
Authenticode signature verified. Local files are under
`_Build/analysis/dlss-integration-20261015/`; they are ignored artifacts.

SDK capabilities and required Vulkan extensions are queried before creation;
advertised extensions are not a substitute for enabled device extensions.
Failures remain explicit. The backend records work on the supplied command
buffer; its owner controls barriers, submission, completion and resource life.
Use `WindowContext::CreateVulkan(&project)` to request extension negotiation,
then `InitializeDlssBackend`. An unavailable optional requirement leaves the
ordinary renderer usable with an explicit failure status. Current game startup
still calls `CreateVulkan()` without a project and does not load/activate NGX.
Only one initialized backend per VkDevice is supported; the caller must not
initialize two independent NGX owners for the same device.

## Scope and game activation

The current guest presentation API supplies the final color image. It does not
identify the matching motion/depth/jitter resources or a location before guest
HUD and temporal reconstruction. Formats and dimensions alone do not identify
those semantic roles. Current Yotei has no verified provider for these inputs.
DLSS must stay inactive there until that provider is established. Do not
substitute zero vectors, guessed depth, a screenshot, shader-hash selection or
an output-image downscale for those missing inputs.

For performance, SR needs genuinely lower render workload before the expensive
passes. Scaling an already rendered image does not recover their cost. Previous
clean world throughput was3.80FPS; there is no DLSS FPS gain measurement yet.
See [the frame-cost measurements](yotei-render-research-and-profile-2026-10-15.md).

Frame Generation is separate work involving presentation, UI and latency
integration. Ordinary Vulkan FG exists; Dynamic MFG in Streamline2.14.1 is
D3D12-only. Neither is implemented by this SR backend.

Primary contracts: [NGX Vulkan helpers](https://github.com/NVIDIA/DLSS/blob/374959484e79a640feaba44c93ac8cfb0a03f5b5/include/nvsdk_ngx_helpers_vk.h),
[SDK headers and license](https://github.com/NVIDIA/DLSS/tree/374959484e79a640feaba44c93ac8cfb0a03f5b5),
[Vulkan SR/FG reference](https://github.com/nvpro-samples/vk_streamline),
[Dynamic MFG limitation](https://github.com/NVIDIA-RTX/Streamline/blob/2122257e0fce486f91b385aa63b9a09b0a34b363/docs/ProgrammingGuideDLSS_G.md#63-enabling-dynamic-multi-frame-generation).

## Validation record

All checks are native Windows through `_Build/windows-local.cmd`; logs are in
`_Build/analysis/dlss-integration-20261015/checks/`. Source started at68b92f62;
the initial missing-feature adapter and original fixtures are saved in `baseline/`.
That adapter truthfully reports Unsupported; this is a new-feature reproduction,
not a preexisting guest bug or a deliberate GPU failure.

| Check (directory timestamp/suffix) | Observed result |
| --- | --- |
|082323/c047d0 baseline GPU|Actual RGBA/depth/MV upload/readback controls pass; Unsupported at feature request, exit1|
|082520/d44151 unchanged GPU|Real NGX create/evaluate, eight frames960x540->1920x1080, requested TransformerM, finite output/object/background/guards pass, exit0|
|082545/c24232 unchanged CPU|Original valid/invalid temporal contract passes|
|082954/67f245 GENERAL validation|Eight real SR frames, Khronos validation errors0 through teardown|
|083303/afac65 read-only neighbor|SHADER_READ_ONLY input layouts, real SR and validation errors0; loaded runtime path/version310.9.1.0 and exact DLL bytes verified|
|083023/3b5c64 ->083040/f16d84|SDR presenter HDR guard intended CPU failure -> unchanged pass|
|083744/0b3ca6|SDK OFF: temporal/presentation/explicit Unsupported tests3/3 pass|
|085350/7fca00 ->085516/561efe|Ordinary bootstrap without NGX or runtime: actual24-image teardown failure -> unchanged validation-clean pass|
|085517/1c5915|Actual hidden-SDL renderer device negotiation, NGX, Presenter copy, first reset/duplicate rejection, lease retirement and independent owned-frame readback; final device teardown errors0|
|085608/73aa85|Existing host allocation and present downscale GPU neighbors2/2 pass|
|085958/516e65|Final84a9c1ec SDK-free contract/state/Unsupported tests3/3 pass|

Original GPU fixture SHA2560BEC2EBE8DE9A73071F783DB013E5E47868086EED2F1CB1332E9D65205DD5598;
original CPU fixture F70B27AA730E216B5A1C2683E3FA40B83CB64D9CAE3C0932FE1B40E46877E80B.
Validation/provenance and state neighbors were added after the unchanged proof.
Requested preset is recorded; no assertion about driver profile overrides.

Validation setup failures are separate: initially no registered validation layer;
the portable layer then reported a stale system Epic overlay manifest. A clean
local explicit-layer directory, empty implicit-layer search directory and
process-scoped implicit-layer disable allowed validation without registry or
driver changes. One earlier env-only run used the unchanged pre-validation
executable and is not validation evidence. No errors were filtered or suppressed.

First full renderer run084857/29b86e had correct SR/copy/lease but final production
Vulkan diagnostics reported24 leaked images after its observer detached; its exit0
is **not** a clean pass. The observer now remains alive through the real device
destructor. A separate ordinary startup reproduces the same failure with no NGX
or runtime dependency:24 bindless placeholders retired into the image recycler,
but DestroyAllocator did not drain it. Production now destroys retired native
images before the VMA pool/allocator. The unchanged ordinary fixture SHA256
6C0B0483BB227653091E416A00268908769558014A190544384715E2DAFBC3FB
has explicit exit2/errors1 before, exit0/errors0 after. Full SR path also exits
cleanly with that same fixture. This fixes resource teardown, not steady FPS.

Pending: guest-produced semantic provider, real game input-resource role mapping,
HDR tone mapping insertion, multi-frame presenter accumulation, actual swapchain
re-present, active deferred recorder/partial-SDK-failure and resize/history quality
cases, multiple simultaneous allocator/backend ownership and clean game FPS/image
comparison. A passing synthetic scene does not prove these stages or game support.
