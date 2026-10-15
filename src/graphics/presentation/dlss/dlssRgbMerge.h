#ifndef KYTY_GRAPHICS_DLSS_RGB_MERGE_H_
#define KYTY_GRAPHICS_DLSS_RGB_MERGE_H_

#include "graphics/presentation/dlss/dlssVulkan.h"

namespace Libs::Graphics {

// Shared compute operation: copy source RGB into two HDR outputs while preserving
// each destination's existing alpha. All three views must be distinct RGBA16F,
// single-mip/layer, storage-capable images containing extent in GENERAL layout.
// Views must refer to nonaliasing images. Caller supplies producer/consumer barriers
// and the same-device recording graphics command buffer outside rendering. Bound
// views/images remain alive through the returned lease's GPU use. Every Record
// creates fresh immutable descriptors: no view-handle cache survives retirement.
// Up to16 simultaneously held descriptor leases are supported. Capacity exhaustion
// is explicit Unsupported. Extents
// above16384 in either dimension are unsupported by this bounded operation.
// Calls are serialized. Native pipeline/descriptors are changed by Record; the
// caller must invalidate its cached compute/graphics state afterwards.
class DlssRgbMerge {
public:
    DlssRgbMerge();
    ~DlssRgbMerge(); // Caller must already have drained GPU use.
    DlssRgbMerge(const DlssRgbMerge&) = delete;
    DlssRgbMerge& operator=(const DlssRgbMerge&) = delete;

    [[nodiscard]] DlssStatus Init(VkDevice device, PFN_vkGetDeviceProcAddr get_device_proc_addr);
    // If supplied, retirement_lease must initially be empty. Caller retains it to
    // command completion, even after a later native operation fails. Without an
    // output pointer, this helper retains the lease until DestroyAfterGpuIdle.
    // Device/pipeline stay alive until every returned lease has retired.
    [[nodiscard]] DlssStatus Record(VkCommandBuffer command, VkImageView source_rgba16f,
        VkImageView destination_a, VkImageView destination_b, DlssExtent extent,
        std::shared_ptr<void>* retirement_lease = nullptr);
    [[nodiscard]] DlssStatus DestroyAfterGpuIdle();

private:
    struct State;
    std::unique_ptr<State> m_state;
};

} // namespace Libs::Graphics
#endif
