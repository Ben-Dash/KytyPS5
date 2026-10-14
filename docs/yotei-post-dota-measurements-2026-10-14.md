# Yōtei: post-Dota measurements, cycle51

Native Windows, branch `yotei-integration-native`, record HEAD `5dc3f3c3`.
Production remains `42c6c601`; no production change or new optimization applied.
Runtime: `_Build/analysis/yotei-performance-cycle49-20261014/runtime-current-42c6c601`.
Exact EXE SHA256: `8E3D9CD6546A63307CF8559D302C6DB67B2A40E7B93035B89C421751A8633F7C`.

User closed Dota personally. Before launch, Dota, task-native processes and
vmmemWSL were absent; aggregate GPU usage was2%, VRAM2412/16311MiB. After all
runs GPU1%, VRAM2614MiB, clocks457MHz. These are snapshots, not per-process
attribution. No unrelated process was stopped. The earlier0.95FPS run used
source4e/executable035F while Dota was active; this new source42c result cannot
isolate the entire historical loss to Dota alone.

All three launches used the same installed executable/runtime, neutral input,
1920x1080 output, full renderer/replay, compute-chain1, queue0, core1,
spin2000, recording-age0, Performance game preset, label interval2000us.
Inherited KYTY/VK variables were cleared. AGE, BDA_STATS, FunctionProfile,
GPUzones, WAIT trace and LabelCausal were absent. Only the middle launch added
`--drain-stats 5`. Native validation/dumps remained off; bounds/redzone remained
on. Output size does not establish internal rendering resolution. The same
runtime retained compatible caches across runs; caches were not frozen snapshots.

| Run under `_Build/runs/` | Title window | Outcome |
| --- | --- | --- |
| `game-20261014-195656-perf-cycle51-post-dota-clean-base-ad75c5` | gate544/90.229s, completed6s, mean3.85FPS | exit0, no timeout/memory guard; automatic captures withheld |
| `game-20261014-195953-diag-cycle51-post-dota-drain-only-487850` | gate528/90.327s, completed10s, mean3.475FPS | exit0, no timeout/memory guard; instrumented diagnostic |
| `game-20261014-200245-perf-cycle51-post-dota-clean-recovery-1e8184` | gate550/90.145s, first title3.3FPS | exit0 at93.075s before6s sample completed; NO recovery mean |

Recovery ended without a supervisor close request; the reason is unknown.
Do not turn its first title into a completed average. User confirmed that the
current launch displayed the character and world while this recovery was active.
This is user-reported scene confirmation for recovery, not an independently
captured image and not retroactive scene equivalence for both prior runs.
No image was saved: Windows denied foreground acquisition in the first run.

The last three5s diagnostic reports record GPU command-buffer interval busy
129–144ms/frame and gaps143–166ms/frame. Their readback counters report337–394MiB
per5s window, with2097–2266 events, approximately20–22MiB/frame. Tick waits,
blocked polls and full drains overlap across threads/scopes: do not sum them
or treat interval busy as hardware utilization/shader time. Readback counters
are logical recorded volumes, not independently measured PCIe bytes. This ranks
compute-result publication, host assembly and resource synchronization for
investigation; it does not establish a specific defective wait or justify
skipping required transfers.

The sub1FPS outcome is not reproduced after Dota closed. The drain-only result
is close to the clean result; different window lengths, scene/cadence/cache
variation and an incomplete recovery prevent claiming a precise debug penalty.
There is no newly accepted code optimization and no proof of2x the original
3.7FPS baseline (target7.4FPS),60FPS, full effects or cross-game correctness.

## Renderer strategy after these measurements

No full renderer/backend rewrite has been tried in this round. The next bounded
change should target shared transfer/command scheduling mechanisms used by the
game, preserving its real compute-to-CPU dependencies:

1. Classify actual indirect producer API calls with the prepared default-OFF
   census; aggregate PM4 DRAW_INDIRECT counters are not exact nonindexed Vulkan
   hook hit counts. If eligible hits are0, park that candidate. If nonzero,
   measure its existing recorder Drain before applying the already-reproduced
   copied-hook candidate. CPU unchanged GREEN and finite native indirect draw
   proof are required before a game performance trial.
2. Obtain10–20s hardware-queue ETW evidence aligned to title/world to distinguish
   no-ready-packet gaps, GPU execution and driver/preemption stalls. WPR GPU
   profile is available; recording has not been started. Preserve unrelated
   trace sessions and keep tracing out of clean acceptance windows.
3. Observe current upload admission/BDA volume separately. Prepared counts-only
   upload observer avoids broad optional trace rows; typed16-bit tiler and
   proven readonly DMA footprint remain unapplied. Prove actual eligibility
   and independent regression before changing either shared mechanism.

Ignored proposals/reports: `cycle51/indirect-draw-census`,
`cycle50/gpu-upload-research/post-dota-measurement.md`,
`cycle50/gpu-upload-research/label-causal-counts-only.patch.proposal`,
`cycle50/cpu-dma-footprint/clean42c-baseline-launch-audit.md` under
`_Build/analysis/yotei-performance-*`. These are not compiled fixes.

Run commands and explicit flags are preserved in each `run.json`; both output
streams were drained and all task-owned processes disposed. No fresh build was
needed, installed EXE hash is unchanged, production diff to42c is empty.
Preserve user `tests/ComputePrefetchPermutationTests.inc`. No push/issue comment.
