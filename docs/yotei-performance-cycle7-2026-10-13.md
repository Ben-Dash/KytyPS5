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
