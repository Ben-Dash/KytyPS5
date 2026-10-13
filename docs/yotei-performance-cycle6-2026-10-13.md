# Performance cycle6: avoid widening already-ready GPU reads

Three proposals: shared ReadMemory ready-range query, remove general modulo for
the tiny channel domain, reduce redundant runtime SSBO length queries. Loop
subgroup reuse rejected by archived census:47Any in47distinct blocks with freshly
loaded BDA-fault predicates. Length census836queries/28views/486same-block repeats
is a source lead, not a proof that the NVIDIA driver fails to eliminate them.

Chosen memory change retains one graphics queue, original fence/publication and
CPU-write epilogue. Fastpath requires valid nonzero fully mapped range and one
live nondeleted cached owner covering it entirely. Intersection-only registration
was rejected during peer review. Unowned/partial/unmapped/null/invalid ranges
retain discovery; unarmed or mixed pages retain the original widened download.

Native source3b016bfc plus test-only fixtures, intendedRED001020/ECA63286:
genuine EOP oldA completion and bounded CPU priority gate, native unknown B still
open on another4KiB page of same16KiB owner. Four setup/native/backing/cleanup
goldens pass before final cost failure: oldA0->64, cleanA64->128, CPU-knownA256->320
ring allocation and unrelatedB armed. NewA truly waits8->9 and producesabcdef09.
SAME unchangedGREEN001450/9941F873, final001641/EBB5F04C keep ready cases' ring
offset unchanged/Bunarmed; newA still downloads and waits10->11. No guessed bytes.

Four typed neighbors002004/C8A3296E: mixed oldarmed/newunarmed span, same-page new
sibling, oldarmed and clean is_write. Exact native/CPU A/B and prefix/suffix pass;
CPU-dirty and BDA epoch markers are observed BEFORE a pointer write can hide an
error. Native CPU upload remains exact. CTests2/2 PASS0.81s. EOP IRQ/generation,
fault protocol, eight BC partial-buffer and three mip-destination ownership
neighbors001451..001453 pass. Request across owner end remains a boundary debt;
conservative full-owner guard keeps old behavior. No other-game runtime available.

Independent channel-modulo fixture nativeRED001021/ECA63286 passes52 literal
requests first (counts1/2/3/4, reverse/constants, end/OOB/prefix, waves/EXEC), then
four invocation-entry divisions fail expected0. Production deferred to keep
memory FPS experiment separate and reuse existing GPU cache. ArrayLength proposal
and same-native-buffer distinct-range fixture remain ignored/uncompiled.

All check artifacts under `_Build/analysis/yotei-performance-cycle1-20261012/checks`
with stated timestamp prefixes archive exact binaries/fixtures/diffs/results.
Three-agent review/census/proposals in performance-cycle6 ignored directory and
cycle3 generic-channel-repetition. Native core/build and full30-second in-game
window-title FPS pending. No source fingerprint forcing, limits raised, menu,
automatic buttons, push or issue publication.

Source329492cf0a6ed3c5efe68c4604f9fdacd8575e77 core002053/install002145 PASS,
private runtime `performance-cycle6-20261013/runtime-329492cf`, executable
1308AC8757CBC80712D22179C372D06F4E7524087A13002EE15513F3C38B2D47.
Game002157-bdc248 loaded182239919bytes/1921records without invalidation. Complete
neutral full-render30second title sample00:25:07UTC/frame650: mean2.6704,min2.5,
max2.8,27logged values; exit0, no guards, maxWS16.55GiB/minphysical28.60GiB.
Small difference versus2.6148bc control is not a useful sustained gain and cannot
be isolated to memory versus converter without completed3b-only benchmark.
No screenshots/image inspection/menu/buttons/reset. Next channel optimization
has valid independentRED and runs separately; preserve all shaders/cache seeds.
