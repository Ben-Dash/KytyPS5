# Fused complete tiled color uploads

The old supported RenderTarget64KB upload detiles into a linear scratch buffer
then copies that full buffer into the native image. A shared TileManager route
now writes exact integer texel bits directly into the image for complete4/8-byte,
single-sample/single-layer/single-mip2D color surfaces. Unsupported metadata,
partial regions, transforms, BC/depth, host format/usage/extent/range capabilities
keep the original route. Real native view allocation errors keep existing fatal
handling; capability rejection happens before view creation.

The original and new kernels share the same swizzle functions. Raw R32Uint and
RGBA16Uint views preserve float encodings including NaNs, subnormals and signed
zero. Source producer, uniform upload, destination layout/access and later source
writer dependencies remain explicit. Cache publication/dirty generations and
readback semantics stay in their existing owners.

## Genuine RED and unchanged GREEN

Native public FindImage/FindTexture fixture128x128 passes both4/8-byte independent
CPU tiled-source/native bit goldens, all input/output guards, legal raw views and
cleanup BEFORE its final cost oracle. Old2AB4DBB4 binary015650 intendedRED:
one actual copyBufferToImage per upload exceeds budget0. CandidateF5602E25
021044 SAME fixture GREEN: copy0, all original controls pass. Test observer
forwards v1/core2/KHR2 calls unchanged; no dropped transfer can fake the result.

## Bounded native GPU A/B/A

Every call uploads a fresh dirty source through the public cache. GPU timestamps
include its required source transfer; completed native output/source guards
remain mandatory. Two warm calls and seven fixed samples per format, one operation
below25ms. Archived A2 uses exactly the original binary and its original source
WIP provenance; a later HEAD in rerun metadata does not relabel old binary source.

| Format bytes | Original A median ns | Candidate B | Original A2 | Reduction versus bracketing A |
|---|---:|---:|---:|---:|
|4|21952|16576|21760|24.1581%|
|8|32800|26560|32928|19.1821%|

All54 real native uploads retain exact bits and guards. This proves an operation
gain at128x128, not4K throughput, game FPS, rendered dimensions or full effects.
Actual game retry remains pending; world baseline is~3FPS.

Fresh shader-compute build021201 and native shared tiler021320 pass358cases/
264format-mode pairs after swizzle extraction. Fresh linked BC4/5/7 cache aliases
and copy-mip neighboring cases021408/409 pass with zero mismatches. New-route odd
127x65/pitch128/native source offset4 controls021857 pass for both formats.
Follow-up022337 build and focused CTest3/3 PASS1.36s also queue an actual writer
to the input after the upload: destination retains original bits, native source
changes to the requested word, all source/output guards stay exact. No fixture
barrier before that writer masks the production read-to-write dependency.
Explicit unsupported capability/partial and stream/deferred new-route neighbors
remain pending; old unsupported paths are exercised by shared tiler/cache cases.

## Artifacts and next check

All native build/test output is under ignored integration/performance-cycle1
checks, with archived EXE/source WIP. Driver-independent A/B/A summary and new
shader source snapshots:
`_Build/analysis/yotei-performance-cycle9-20261013/fused-upload/`.

Commit the tested correction separately, native core/install into a new private
Performance runtime, then one
650-gated30-second in-world title measurement without diagnostics. Inspect images
only after a meaningful FPS increase. Three agents continue independent CPU
attribution/resource proposals; no new push or issue publication.
