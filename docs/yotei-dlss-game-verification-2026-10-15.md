# Yōtei: actual game check of the optional DLSS build

Native Windows, 2026-10-15. Branch `yotei-integration-native`, record source
`f3dcf78508c89e172d2aea7fa0aac5faff5bb525`. Root executed the game serially;
three agents audited runtime activation, internal resolution and temporal inputs.
No production behavior changed in this verification round.

**The optional executable renders the actual game, but DLSS is inactive there.**
The passing native SR/presenter fixtures demonstrate host functionality, not
automatic integration into guest rendering.

## Runtime evidence

Artifacts are relative to the repository, outside tracked source.

| Run under `_Build/runs/` | Actual result |
| --- | --- |
| `game-20261015-091119-dlss-sdk-built-game-inactive-61af2f` | Optional SDK build, world visually confirmed at frame653. Twenty-second title sample mean **3.3389 FPS**. Exit0; no timeout or memory guard. |
| `game-20261015-091438-dlss-sdk-inactive-fresh-control-714d55` | Preserved old baseline executable. Exited0 after95.84s before the frame650 gate; **no complete comparison sample or world capture**. No close/stop was requested by the runner. Reason for early closure is unknown. |

The first run used installed executable SHA256
`45F8FDE3C20086900BDAAA615CDF86930D53D4C72FFC3002537E1DDCFB6C6460`.
It was built on360fc774 with exactly the later34e725f4/84a9c1ec production
changes, as recorded in the integration evidence. The runner's sourceRevision
84a9c1ec describes that production content; its embedded title remains360fc77-dirty.
The preserved control SHA256 is
`E46E5CC99DAFA8B4E67177C854FA7A7DE8E8CA1391DE6AB43169F562212E8ABE`,
coreb1a9698e. Its historical run record is explicitly checked because current
production source differs from that historical executable.

Before the optional run, save/cache/download/temp/texture directories were copied
into its independent runtime root. Save and cache file bytes matched the source;
the compiler journal key also matches. Preparation and checksums are in
`_Build/analysis/yotei-dlss-live-20261015/preparation.json`.
Original saves, caches and game files were preserved.

Both launches used the same1920x1080 output, Performance request, neutral input,
bindless/compute-chain settings and disabled profiling/validation as the earlier
clean baseline. The title counts new guest frames, not repeated presentations.
The current saved graphics mode was independently found to be Performance already.

The first sample started after124.06s at frame653. Its owned PrintWindow capture
shows the character, instrument, village, fire and vegetation; no input or
focus changes were made. A single image establishes world phase, not temporal
stability or complete rendering correctness. The synchronous capture occurred
at the start of the sample (metadata +0.281s), so this is not a fully capture-free
benchmark. The previous3.80FPS result is historical, not a complete matched
control for this run. Neither a measured improvement nor a causal regression is
established. No new runtime fatal/device-loss diagnostics were found; normal
gameplay validation was disabled, so this is not validation-clean GPU proof.

## Why this is not DLSS ON

The complete production call path still has:

- `WindowInit` calling `CreateVulkan()` without an NGX project request;
- `FlipQueue` calling ordinary `Presenter::PrepareFrame` with final guest color;
- no production caller for `InitializeDlssBackend` or `PrepareTemporalFrame`.

Only native synthetic tests supply explicit color/depth/full-motion/jitter and
call the temporal path. A compiled SDK or runtime DLL beside the game executable
does not activate it. Do not expose an enabled setting or attribute title FPS
to DLSS until real evaluated and presented guest frames are established.

No verified pre-upscale/pre-HUD color, matching depth/full camera-and-object
motion, jitter convention or insertion point exists for Yōtei yet. Existing
snapshots contain geometry/compute captures, not a temporal resource capture;
the end-of-frame image dumper skips depth and does not establish producer order.
Formats, shader hashes and image dimensions cannot supply semantic proof.

## Next required work

1. Capture a complete guest frame, then map the temporal pass and its producers.
   A signed portable RenderDoc dependency and no-input capture helper are being
   prepared under the ignored analysis directory. They were not executed in
   this round. Native-present-only captures may contain repeated presentation;
   inspect actual draw/dispatch coverage before using them as guest-frame proof.
2. Establish real color/depth/MV/jitter conventions and their lifetimes with
   independent synthetic tests before changing production resource handling.
   Insert before existing temporal reconstruction/HUD and preserve postprocessing.
3. Establish genuinely reduced internal work, then record actual NGX evaluation,
   completed output presentation, image correctness and matched20–30s title FPS.

No game activation, Frame Generation or FPS increase is claimed. No push or
issue publication was performed. The user prefetch fixture remains untouched.
