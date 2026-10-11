#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace Libs::Graphics {

#if defined(KYTY_RESOURCE_REGRESSION)
struct TextureLayoutRegressionObservation {
	bool rejected = false;
	bool description_nonempty = false;
	uint64_t description_size = 0;
};

// CPU-only test exposure of the actual descriptor planner; no cache/image creation.
TextureLayoutRegressionObservation ProbeTextureDescriptionForRegression(
    const std::array<uint32_t, 8>& words, bool requested, bool soft_scope = true);

int RunTextureLayoutRegression(std::string_view mode);
#endif

} // namespace Libs::Graphics
