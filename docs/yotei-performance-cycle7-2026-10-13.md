# Performance cycle7: bounded channel selection without general division

Shared EmitGenericBufferFormats derives source from selector&3 and positive count
from validated table fields. For any positive count: count3 repeats source3 as0;
otherwise source & min(count-1,3) is exactly source%count. This preserves even
future positive count>4 rather than relying on an unchecked power-of-two claim.
Hoist mask and count3 condition once per resource. Invalid format/from-memory/
constant selector gates, width/bit offsets/normalization, address/bounds and EXEC
remain unchanged. Narrow scope excludes indirect formatted path.

Valid intended nativeRED001021/ECA63286 before production:52 literal native
requests pass including counts1/2/3/4, reverse/constant selectors, exact-end,
partial-OOB/prefix carry, guest waves32/64 and asymmetric EXEC/canaries. Then
real entrypoint four variable-divisor operations/slot fail expected0. Same
GREEN002637/598C217D retains all52 cases and emits0 variable divisions in both
waves. Native100requests/4800lane candidate002637-9134037 matches3b235505 exactly,
0 changed/missing/extra;144 pre-existing UNorm goldens remain. Category/codegen
controls002639/002704 pass; focused CTests2/2 PASS0.93s.

Artifact root performance-cycle1/checks with timestamp prefixes above. Raw lane
comparison `performance-cycle6-20261013/channel-native-lane-comparison.json`;
synthetic fixtures committed asbd8146cf before production correction. Actual
NVIDIA runtime speed remains unproved by opcode counts. Need native core/private
install, shorter actual GPU measurement if ready, then full neutral30second title
FPS. No blind game fallback, buffer bound weakening or key forcing. No push or
issue publication. Stable60/actual1080/full effects remain pending.

Native sourceba3ed1b1 core002906/install003108 PASS; privateEXE
97EEB17B27F31156184DBB22A89D7912E69E026DC5868C118BC7540979AA9BB3.
Before full cold game, new opt-in native proxy uses actual32 live formatted reads,
512byteinput (64bytes would leave28OOB), full output/tail goldens, wave32/64,
channels1/3/4, two hardware timestamps/batch with real WAW barriers, max256
dispatches/<25ms batches. Seven samples after warm-up, no GPU zone observer.

Controlled A/B/A: scoped reversal ONLY the modulo hunk for003515/915786 baseline;
restored production byte-for-byte before003614/668312 candidate; reran archived
baseline003705/915786 with same DLLs and six repeat counts37,39,41,27,28,28.
All six native goldens pass each time. Median29.7us(wave32)/58.49us(wave64),
candidate changes-0.66%..+0.44% versus mean of bracketing baseline medians.
No useful GPU speedup in this one-slot/single-group/uniform-input proxy; it does
not prove no effect in fc6f. Avoid automatic600second cold-game loops on only
opcode-count evidence. Opaque driver cache compatibility is the next iteration
speed work; real in-game30second FPS remains required for any success claim.

Comparison JSON in `yotei-gpu-microbench-20261013-cycle7/modulo-a-b-a-comparison.json`.
Original measured fixture archived unchanged in A/B artifacts. Later safety-only
test refinements clamp repeat count before narrowing and explicitly require the
already tested actual32-subgroup compute/ballot hardware. No shader code change
from those test guards. No image/menu/automatic input or native reset.
