#include "graphics/presentation/dlss/dlssTemporalChannels.h"
#include <array>
#include <cstdio>
#include <cstdlib>

using namespace Libs::Graphics;
namespace {
void Require(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
}

int main() {
    const VkComponentMapping identity{VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    const VkComponentMapping rgba{VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G,
        VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
    for (const auto role : {DlssTemporalChannels::ColorRgba, DlssTemporalChannels::DepthR,
                            DlssTemporalChannels::MotionRg}) {
        Require(DlssTemporalMappingPreservesConsumedChannels(identity, role), "identity view must preserve every role");
        Require(DlssTemporalMappingPreservesConsumedChannels(rgba, role), "explicit RGBA view must preserve every role");
    }
    // Independent observable contract: copying the physical image preserves depth
    // R and motion RG even when the source view's UNUSED outputs are replicated or
    // constant. These synthetic mappings do not contain captured guest bytes.
    const VkComponentMapping rrrr{VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
        VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R};
    const VkComponentMapping rg01{VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G,
        VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE};
    Require(DlssTemporalMappingPreservesConsumedChannels(rrrr, DlssTemporalChannels::DepthR),
            "depth RRRR must be accepted: consumed R is unchanged");
    Require(DlssTemporalMappingPreservesConsumedChannels(rg01, DlssTemporalChannels::MotionRg),
            "motion RG01 must be accepted: consumed RG are unchanged");
    constexpr std::array<VkComponentSwizzle, 7> values{VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R,
        VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
    constexpr std::array<VkComponentSwizzle, 4> physical{VK_COMPONENT_SWIZZLE_R,
        VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
    unsigned checked = 0, accepted = 0;
    for (const auto role : {DlssTemporalChannels::ColorRgba, DlssTemporalChannels::DepthR,
                            DlssTemporalChannels::MotionRg}) {
        const size_t consumed = role == DlssTemporalChannels::DepthR ? 1 :
            role == DlssTemporalChannels::MotionRg ? 2 : 4;
        for (auto r : values) for (auto g : values) for (auto b : values) for (auto a : values) {
            const std::array<VkComponentSwizzle, 4> channels{r, g, b, a};
            // An independently resolved view sample must return the ORIGINAL
            // physical component at each consumed output, not a constant or other channel.
            bool expected = true;
            for (size_t i = 0; i < consumed; ++i) {
                const auto sampled = channels[i] == VK_COMPONENT_SWIZZLE_IDENTITY ? physical[i] : channels[i];
                expected &= sampled == physical[i];
            }
            Require(DlssTemporalMappingPreservesConsumedChannels({r, g, b, a}, role) == expected,
                    "channel guard differs from independently resolved consumed view sample");
            ++checked; accepted += expected;
        }
        for (size_t i = 0; i < 4; ++i) {
            auto invalid = rgba;
            if (i == 0) invalid.r = VK_COMPONENT_SWIZZLE_MAX_ENUM;
            if (i == 1) invalid.g = VK_COMPONENT_SWIZZLE_MAX_ENUM;
            if (i == 2) invalid.b = VK_COMPONENT_SWIZZLE_MAX_ENUM;
            if (i == 3) invalid.a = VK_COMPONENT_SWIZZLE_MAX_ENUM;
            Require(!DlssTemporalMappingPreservesConsumedChannels(invalid, role),
                    "invalid Vulkan swizzle enum must remain rejected even in unused components");
        }
    }
    Require(!DlssTemporalMappingPreservesConsumedChannels(identity,
                static_cast<DlssTemporalChannels>(99)), "unknown semantic role must remain unsupported");
    std::printf("PASS: %u independent component mappings (%u accepted), invalid enums/roles rejected\n",
                checked, accepted);
}
