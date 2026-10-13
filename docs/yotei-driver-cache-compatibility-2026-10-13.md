# Reuse opaque driver data independently of compiler journal revisions

Existing DriverCacheSignature coupled source-keyed shader records and opaque
Vulkan cache. Changing emitter source discarded all opaque data, including
unchanged pipelines. Khronos defines device compatibility by the native header,
while the driver matches actual module/layout/pipeline create information:
https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineCacheCreateInfo.html
https://docs.vulkan.org/spec/latest/chapters/pipelines.html#VkPipelineCacheHeaderVersionOne

An exact factory extraction preserves old literal KytyPC3 bytes before intended
CPU RED005245/B1EABF29. Synthetic64hex sourceA/sourceB retain identical host;
all host/driver/UUID/optimization/diagnostic/length/checksum/native-header controls
pass before source-only opaque rejection. SAME GREEN005655/51C3C357 passes every
old journal roundtrip/version/repair/key test. JournalSignature remains exact
source-aware. New opaque signatureKytyPC4 retains host/driver/optimization and
diagnostic suffix. Reader accepts intact legacy64hex wrapper only with matching
full host/config/suffix, reads its real139byteoffset, validates exact sizes,
UINT32 bound, checksum and native32byteVERSION_ONE vendor/device/UUID. Vulkan
rejection still falls back to an empty cache; Release/known-source gates stay.

Native roundtrip010845/21E43738 constructs a finite pipeline with real cache,
executes independent full-EXEC output, gets15132actual opaque driver bytes, writes
one private legacy test wrapper and invokes actual LoadDriverCacheShard. Exact
15132bytes load; same pipeline full/asymmetricEXEC and tail canaries pass. Actual
feedback valid1/applicationcacheHit1. Wrong host, corrupted checksum and rehashed
nativeUUID mismatch each load0/empty. Four unique task-owned files removed
individually; no user cache bytes edited or recursive deletion. Focused CPU/native
CTest2/2 PASS0.53s; final native build010943 PASS.

Artifacts under performance-cycle1/checks with prefixes above; shared policy and
CPU/native fixtures are reusable. This proof establishes loader compatibility and
actual tiny-pipeline cache reuse, not all-game render correctness. Existing182MB
Yotei blob and shorter cold iteration still need a native game retry. Changed
SPIR-V legitimately compiles anew. Larger shard/device/config protections stay;
no journal key pinning, fallback work dropped, new bounds or FPS assertion.
