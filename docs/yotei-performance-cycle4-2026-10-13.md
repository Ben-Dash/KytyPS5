# Performance cycle4: earlier recording submission

Source baseline79cc9658748469129c9478c8a8450c445a2df40c. Root serial native
Windows, RTX5060Ti. The policy is a host batching experiment from upstreamPR1430,
not a guest semantic shortcut. Draw/dispatch emits real Vulkan commands before
marking work; age checks occur before binding preparation and every16 PM4 packets.
Existing Flush retains recording stream, native ordering and deferred operations.
Unsigned environment budget defaults0;1000us is the tested opt-in setting.

Test fixture first corrects its MUBUF helper call to(vdata,srsrc,vaddr); earlier
wrong-byte setup232913 is not RED. Genuine RED233238/408E8B32 completes both64-lane
load/add/store dispatches with exact+2/sentinels, then rejects retained old tick.
Unchanged GREEN233751/86C5F063 advances it. Artifacts are under
`_Build/analysis/yotei-performance-cycle1-20261012/checks/20261012-*` with those
prefixes, archived executable, test fixture and source diff.

Final build234142 passed; test EXED228C047B48DCF7AF6DD174662141D5F43298961D7647E78B4459F64BF61ECCD.
Five CTests PASS2.00s. The private clock seam exercises the exact production policy
with real16-byte Vulkan fills, negative/999/1000/1001us boundaries, inactive/empty
recordings, unchanged register object identity, deferred callback, reset and async
route. Separate indirect fixture warms native pipelines, writes unequal2,1,1
GPU arguments, submits aged work, produces1..8 with sentinels and preserves CPU-old
arguments/no main download reservation until explicit publication234238.

Active scheduler/WAIT/resume/command-stream controls234009/234010 and registered
EOP publication/IRQ/generation234129 pass. Default strict indirect-count test
fails identically before/after; unregistered-EOP test crashes before/after. These
remain test debt and are not valid recording-policy REDs. Baseline binaries were
rerun without source reversal; initial missing-DLL error is discarded, corrected
native DLL directory used. No reset, skipped command, menu or automatic input.

Native sourcebc93628e core234318/install234404 PASS. Same private native executable
80E746E146459D8D58E09189A0B42D5580718BFFA3DC70865EC45F25B558ABD4 in
`_Build/analysis/yotei-recording-age-20261013-cycle4/runtime-bc93628e`.
Matched title-only/neutral/full-render30second runs fromframe650:

| Run under `_Build/runs/` | Budget | Mean FPS | Min/max | Outcome |
| --- | ---: | ---: | --- | --- |
| game-20261012-234413-perf-cycle4-age1000-title-30s-world-1080-fa4053 |1000us|2.7393|2.6/2.9|exit0/fullsample|
| game-20261012-234809-perf-cycle4-age0-matched-title-30s-world-1080-2c5488 |off|2.6148|2.6/2.7|exit0/fullsample|

Single-pair4.76% is small; no useful sustained improvement claimed. No memory
guards/menu/input/images, no GPU reset. Keep defaultoff. Source/work inputs were
frozen through both runs; new fixtures' line endings normalized afterwards without
changing the cost/golden oracle. Next measured direction is the generic converter
in the saved36–39ms compute hotspot, not an unproven ordinary-fill world port.
