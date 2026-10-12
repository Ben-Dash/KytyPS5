# Performance cycle 2

Native Windows, `yotei-integration-native`. Three agents prepared separate proposals:
known ordered-store publication, repeated active subgroup votes, and spatial image
cache invalidation. Root owns production edits and all native builds/tests/runs.
Measure each game candidate for30 seconds using window-title FPS, directly in the
world, with neutral input and complete rendering. No menu navigation; inspect
images only after a measured FPS improvement. Keep loading separately bounded.

## First correction: known ordered-store publication

`WriteOrderedMemory` records the actual native GPU store from CPU-known bytes.
Previously its immediate readback copied those same bytes back through the download
ring, including copy barriers, even though the backing publication already waits
for actual native completion. The correction preserves the GPU store and completion
but retains owned known bytes for that publication. Other dirty bytes use actual
GPU copies. All original dirty ranges remain in-flight until publication; clean
backing readers cannot observe the old CPU label as current. Token/generation
handling, subsequent writers, WAIT conditions and interrupt order remain intact.
This follows the queue-to-host completion contract in the
[Vulkan fence specification](https://docs.vulkan.org/refpages/latest/refpages/source/VkFence.html).

Test-only `LiteralEopPublicationCostTests.inc` uses real Thread_Gpu, finite native
compute producers/stores/consumers and independent backing observations. Eight
cases cover32/64-bit labels, mixed unknown neighbor, GPU rewrite, CPU overwrite,
repeated literals, a false wait and a masked64-bit wait. All setup/value/lifetime/
IRQ assertions run before the cost assertion, after native cleanup.

Intended RED: `checks/20261012-211508-0734855-cycle2-literal-eop-cost-red-with-fault-route`,
EXE SHA256 `BA99ECCF8DF8C0CDFCC4F4785EB20349594B057F42454098E38DC1D2E37503BE`.
All semantic controls pass, then known-only reservation64 fails budget0. Earlier
fixture access violations came from missing test fault-handler setup and are not
the reproduction. The test uses the existing production guest-memory fault route.

Same unchanged eight-case GREEN211809/DD73A957 passes. Final GREEN212334 uses
EXE SHA256 `4BA4B219D51F3300AFC94DF43837C911B0DB12107C7891970BC445129D3C6892`:
ordinary/repeated known stores reserve0 bytes, mixed unknown neighbor64 bytes.
Three span neighbors212333 on the same native binary pass: 64-byte payload with
caller mutation, eight-byte store spanning tracker pages, and unknown GPU prefix/
suffix around a known middle. Existing native EOP/IRQ/generation neighbors211818–
211819 pass. Fresh PM4 wait/resume and command-stream checks212232/F1763B39 pass.

Native builds211726,212053 and212234 passed through `windows-local.cmd`. Bounded
test commands use `run-test.ps1` with `--eop-literal-publication-cost`,
`--eop-literal-publication-span-neighbors`, `--eop-literal-ordering-gpu`,
`--eop-literal-neighbors-gpu`, `--pm4-wait-only` and `--command-stream-only`.
Artifacts are under ignored `_Build/analysis/yotei-performance-cycle1-20261012/checks/`.

Pure known spans remove their native download work. A tiny merged unknown range
split into two aligned copies may reserve128 bytes instead of the original64;
this correction does not universally reduce staging size. No new cross-game
gameplay was available. Tests alone do not prove a speedup or complete effects.

Native core build212431 and install212539 PASS for local revision8c8d0131.
Private installed EXE SHA256
`81F89A92F99A6636B2A860073EB11F45F9FED3A5699911C75A45A86D303F0638`.
World run `_Build/runs/game-20261012-212552-perf-cycle2-eop-title-30s-world-1080-5ee908`
automatically measured30 seconds starting21:29:13UTC/frame650; mean recorded
window-title FPS2.2565 (23 values, min2.1/max2.4), then exited0 at21:29:46UTC.
MaxWS17.83GiB, minphysical26.37GiB/mincommit24.69GiB, no guards. Full rendering,
neutral input, no menu, no GPU diagnostic profiling or images. Exact metadata,
title trace and summary are retained in the run folder. Compatible cache replay
built507/taken480, sync/prefetch0; no cache key override.

This does not improve earlier2.435 first baseline window or later~2.55 warm title
values. Possible slowdown needs a fresh matched control before attributing its
cause; static download cost reduction alone is not a useful FPS improvement.
Native tests also do not establish complete effect correctness or60FPS.

## Other proposals remain unapplied

The subgroup proposal removes duplicate constant-true votes only for supported
emulated wave64 active references. Its actual compiler cost fixture and unchanged
GPU loop/mask neighbors must pass before applying it. The spatial-cache proposal
uses existing range generations instead of unrelated global image registrations;
its actual-predicate scan-cost reproduction includes writer, collision and cache
lifetime controls. Neither proposal has native RED/GREEN or measured FPS yet.

All proposals are preserved under `_Build/analysis/yotei-performance-cycle2-20261012/`.
Do not restore archived sampler WIP or force old shader cache keys. Stable60FPS,
actual1080 rendering and all effects remain pending. No new push or issue post.
