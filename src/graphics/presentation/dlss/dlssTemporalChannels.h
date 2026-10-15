#ifndef KYTY_GRAPHICS_DLSS_TEMPORAL_CHANNELS_H_
#define KYTY_GRAPHICS_DLSS_TEMPORAL_CHANNELS_H_

#include <vulkan/vulkan.h>

namespace Libs::Graphics {

// The declared role determines which sampled channels a physical image copy
// must preserve when its private replacement view uses identity swizzles.
enum class DlssTemporalChannels { ColorRgba, DepthR, MotionRg };

// The copy operates on physical image components, not sampled view results.
// Require identity only for channels consumed by the declared role; Vulkan's
// mapping enums must still be legal in every component, including unused ones.
inline bool DlssTemporalMappingPreservesConsumedChannels(
    const VkComponentMapping& mapping, DlssTemporalChannels channels) noexcept {
    const auto legal = [](VkComponentSwizzle swizzle) {
        switch (swizzle) {
            case VK_COMPONENT_SWIZZLE_IDENTITY:
            case VK_COMPONENT_SWIZZLE_ZERO:
            case VK_COMPONENT_SWIZZLE_ONE:
            case VK_COMPONENT_SWIZZLE_R:
            case VK_COMPONENT_SWIZZLE_G:
            case VK_COMPONENT_SWIZZLE_B:
            case VK_COMPONENT_SWIZZLE_A: return true;
            default: return false;
        }
    };
    if (!legal(mapping.r) || !legal(mapping.g) || !legal(mapping.b) || !legal(mapping.a)) return false;
    if (mapping.r != VK_COMPONENT_SWIZZLE_IDENTITY && mapping.r != VK_COMPONENT_SWIZZLE_R) return false;
    if (channels == DlssTemporalChannels::DepthR) return true;
    if (mapping.g != VK_COMPONENT_SWIZZLE_IDENTITY && mapping.g != VK_COMPONENT_SWIZZLE_G) return false;
    if (channels == DlssTemporalChannels::MotionRg) return true;
    if (channels != DlssTemporalChannels::ColorRgba) return false;
    return (mapping.b == VK_COMPONENT_SWIZZLE_IDENTITY || mapping.b == VK_COMPONENT_SWIZZLE_B) &&
           (mapping.a == VK_COMPONENT_SWIZZLE_IDENTITY || mapping.a == VK_COMPONENT_SWIZZLE_A);
}

} // namespace Libs::Graphics
#endif
