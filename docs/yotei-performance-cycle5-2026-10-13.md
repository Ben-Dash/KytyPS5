# Performance cycle5: mutually exclusive runtime buffer categories

Three proposals reviewed: safe ordinary native buffer fill, separate readback
queue with exact native-writer/reverse dependency, generic formatted conversion
control. Immutable main journal492compute permutations/472hashes and saved GPU
profile identified fc6f at35–39ms/frame; complex buffer kernels54.070ms and65.5%
of overall compute time are excluded by current pure-fill proof. Unknown remaining
fill coverage is not claimed zero. Queue writer/copy races remain unresolved.

Ordinary native fill fixture independently verified nonuniform native primer,
CPU backing remains old until fence, dirty owner,64 exact fill words/sentinels.
Only then intendedRED235325/A23D7856 fails GameDispatch1/BufferCopy0 expected0.
Opt-in fixture saved; no production fill shortcut or CTest registration whileRED.

Chosen shared optimization uses one runtime category selection in the existing
generic formatted component helper. Category comes from actual descriptor;
Float retains width16 check. Integer/scaled/normalized arithmetic, signed
extraction, SNorm clamp, load-site bounds/selectors/EXEC/D16, generic permutation
policy and format specialization remain unchanged. No shader/hash/title selector.

Test-only helper ABI compatibility was written beforeRED. Actual emitted taken
CFG costs, not a desired syntax count:235420/D38503A9 six decisions for each legal
format -> unchanged235504/0F796FBD one decision or Float two. Old nine arithmetic
path budgets pass first in both. Independent100request/4800lane native baseline
235421/candidate235505 have0 changed/missing/extra outputs. Both retain144 known
UNorm endpoint precision errors; no full numeric-suite PASS. Static profile2
neighbor235544 produces0 errors; generic codegen235507 PASS. CTests2/2 PASS0.79s.

Artifacts: `_Build/analysis/yotei-performance-cycle1-20261012/checks/20261012-*`
with listed timestamp prefixes; archived binaries/fixtures/WIPdiff/stdout/runjson.
Comparison/census/proposals under performance-cycle3 `generic-kind-dispatch`
and `uniform-fill-coverage.json`; census old SPIR-V is explicitly historical.
Code review found the five-parameter ABI and nested merge correct; exact GPU byte
comparison confirms this hardware's results. A single SPIR-V choice is not a
guarantee of a single native branch or frame speedup. Native build/install and
30second in-game title comparison next; preserve neutral input/full rendering.

Native source3b016bfc core235714/install235757 PASS; privateEXE
C9F1458F1C9B82A08BEC51C94DD32DDC533EB9ADA9D6C18BFFF3C53A968C721C.
Cold game235805-653813 invalidated old pipeline source key naturally, compiled
new variants and exited0 at300s/frame294 with147MB saved cache. Warm retry
000322-aea57f loaded that cache, exited0 at300s/frame649;182240066bytecache retained.
Neither has complete frame650/30second title benchmark. Startup FPS and partial
world titles cannot be used as a speedup. No guards/resets/menu/buttons/images.
Next candidate changes only readback scheduling, preserving this warmed shader
identity. Repeated identical warming attempts are deferred.
