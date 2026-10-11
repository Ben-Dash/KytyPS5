#include "TextureLayoutRegression.h"

#if defined(KYTY_RESOURCE_REGRESSION)

#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>

namespace Libs::Graphics {
namespace {

std::array<uint32_t, 8> VolumeWords(uint32_t width, uint32_t height, uint32_t depth) {
	std::array<uint32_t, 8> words {};
	// Nonnull, aligned synthetic address; the regression never allocates this backing.
	words[0] = 0x00100000u; // Base40 = 0x10000000.
	words[1] = (static_cast<uint32_t>(Prospero::BufferFormat::k8_8_8_8UNorm) << 20u) |
	           (((width - 1u) & 3u) << 30u);
	words[2] = ((width - 1u) >> 2u) | ((height - 1u) << 14u);
	words[3] = (static_cast<uint32_t>(Prospero::ImageType::kColor3D) << 28u) |
	           (static_cast<uint32_t>(Prospero::TileMode::kStandard4KB) << 20u) |
	           0xfacu; // XYZW identity swizzle; base/last mip zero.
	words[4] = depth - 1u;
	return words; // MAX_MIP=0, no metadata compression, ordinary 256-bit T#.
}

bool GeometryOracle(const std::array<uint32_t, 8>& words, uint32_t width, uint32_t height,
                    uint32_t depth, uint64_t expected_bytes) {
	ShaderRecompiler::IR::DescriptorValue value {};
	value.dword_count = 8u;
	value.dwords = words;
	const auto descriptor = DecodeNativeDescriptor<ShaderTextureResource>(value);
	if (descriptor.Width5() + 1u != width || descriptor.Height5() + 1u != height ||
	    descriptor.Depth() + 1u != depth || descriptor.Type() != Prospero::ImageType::kColor3D ||
	    descriptor.Format() != Prospero::BufferFormat::k8_8_8_8UNorm || descriptor.IsNull()) {
		std::fprintf(stderr, "TextureLayoutRegression: synthetic descriptor decode mismatch\n");
		return false;
	}
	TileSurfaceLayout layout {};
	if (!TileGetTiledTextureLayout(
	        {descriptor.Format(), descriptor.TileMode(), TileSurfaceDimension::Dim3D,
	         width, height, depth, 1u, 1u}, layout) || layout.total_size != expected_bytes) {
		std::fprintf(stderr, "TextureLayoutRegression: independent geometry mismatch expected=%" PRIu64
		                    " actual=%" PRIu64 "\n", expected_bytes, layout.total_size);
		return false;
	}
	return true;
}

bool FitsGeometry() {
	// One mip, four bytes per texel, power-of-two extents aligned to complete tile blocks.
	// The expected values are independent dense-byte oracles, not a copied layout algorithm.
	if (!GeometryOracle(VolumeWords(128u, 128u, 128u), 128u, 128u, 128u, 0x00800000ull) ||
	    !GeometryOracle(VolumeWords(1024u, 1024u, 512u), 1024u, 1024u, 512u, 0x80000000ull)) {
		return false;
	}
	std::printf("TextureLayoutRegression: fit geometry PASS (8MiB and 2GiB; no backing allocation)\n");
	return true;
}

bool RejectsOversized(bool requested, bool eight_gib) {
	const uint32_t width = eight_gib ? 2048u : 1024u;
	const uint64_t expected_bytes = eight_gib ? 0x200000000ull : 0x100000000ull;
	const auto words = VolumeWords(width, 1024u, 1024u);
	if (!GeometryOracle(words, width, 1024u, 1024u, expected_bytes)) {
		return false;
	}
	std::printf("TextureLayoutRegression: expect checked rejection scope=%s bytes=%" PRIu64
	            " before real DescribeTexture\n", requested ? "requested_bindless" : "eager_probe",
	            expected_bytes);
	std::fflush(stdout);
	const auto observed = ProbeTextureDescriptionForRegression(words, requested);
	if (!observed.rejected || observed.description_nonempty || observed.description_size != 0) {
		std::fprintf(stderr, "TextureLayoutRegression: oversized descriptor admitted or not rejected"
		                    " rejected=%d nonempty=%d size=%" PRIu64 "\n", observed.rejected,
		                    observed.description_nonempty, observed.description_size);
		return false;
	}
	std::printf("TextureLayoutRegression: checked rejection PASS scope=%s bytes=%" PRIu64 "\n",
	            requested ? "requested_bindless" : "eager_probe", expected_bytes);
	return true;
}

bool RequiresRequestedFatal(bool soft_scope, bool eight_gib) {
	const uint32_t width = eight_gib ? 2048u : 1024u;
	const uint64_t expected_bytes = eight_gib ? 0x200000000ull : 0x100000000ull;
	const auto words = VolumeWords(width, 1024u, 1024u);
	if (!GeometryOracle(words, width, 1024u, 1024u, expected_bytes)) return false;
	std::printf("TextureLayoutRegression: expect unsupported fatal scope=%s bytes=%" PRIu64 "\n",
	            soft_scope ? "strict_requested_bindless" : "ordinary_requested", expected_bytes);
	std::fflush(stdout);
	(void)ProbeTextureDescriptionForRegression(words, true, soft_scope);
	std::fprintf(stderr, "TextureLayoutRegression: requested unsupported descriptor returned instead of fatal\n");
	return false;
}

} // namespace

int RunTextureLayoutRegression(std::string_view mode) {
	if (mode == "strict-requested" || mode == "strict-requested-8g") {
#if defined(_WIN32)
		if (_putenv_s("KYTY_STRICT_TEXTURES", "1") != 0) return 2;
#else
		if (setenv("KYTY_STRICT_TEXTURES", "1", 1) != 0) return 2;
#endif
	}
	bool passed = false;
	if (mode == "fit") {
		passed = FitsGeometry();
	} else if (mode == "oversized-eager") {
		passed = RejectsOversized(false, false);
	} else if (mode == "oversized-requested") {
		passed = RejectsOversized(true, false);
	} else if (mode == "oversized-eager-8g") {
		passed = RejectsOversized(false, true);
	} else if (mode == "oversized-requested-8g") {
		passed = RejectsOversized(true, true);
	} else if (mode == "ordinary-requested" || mode == "ordinary-requested-8g") {
		passed = RequiresRequestedFatal(false, mode == "ordinary-requested-8g");
	} else if (mode == "strict-requested" || mode == "strict-requested-8g") {
		passed = RequiresRequestedFatal(true, mode == "strict-requested-8g");
	} else if (mode == "boundaries" || mode == "all") {
		passed = FitsGeometry() && RejectsOversized(false, false) && RejectsOversized(true, false) &&
		         RejectsOversized(false, true) && RejectsOversized(true, true);
	} else {
		std::fprintf(stderr, "TextureLayoutRegression: unknown mode; expected fit, oversized-eager,"
		                    " oversized-requested, oversized-eager-8g, oversized-requested-8g, all\n");
		return 2;
	}
	return passed ? 0 : 1;
}

} // namespace Libs::Graphics

#endif
