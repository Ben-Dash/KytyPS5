#ifndef KYTY_GRAPHICS_DLSS_PRESENTATION_H_
#define KYTY_GRAPHICS_DLSS_PRESENTATION_H_

#include "graphics/presentation/dlss/dlssVulkan.h"
#include <array>
#include <memory>

namespace Libs::Graphics {

// Explicit semantic engine outputs; the presenter never chooses resources by
// title, shader identity, address, format similarity or the last bound depth.
// Caller provides full-frame resources on the renderer's SAME graphics device
// and queue, in the backend-readable layouts (GENERAL output) before this point. A current-frame producer must
// precede the evaluate in the supplied command buffer or ordered same queue.
// Current SDR presentation accepts only perceptually encoded, display-ready LDR
// color (configuration.hdr=false). HDR scene output needs an earlier engine
// insertion point followed by real tone mapping; the presenter does not do it.
struct DlssPresentationInput {
    // Strictly increasing guest-flip identity. Re-presenting a prepared frame
    // uses PresentLastFrame and must not evaluate DLSS or advance history again.
    uint64_t frame_id = 0;
    DlssConfiguration configuration;
    DlssFrameInputs inputs;
    // Feature is initialized/created separately by its owner; this API never
    // changes feature configuration or initializes SDK/device state implicitly.
    std::shared_ptr<DlssVulkan> backend;
    // Owns EVERY image/view/allocation until this command buffer retires. A
    // non-owning raw pointer disguised as a lease does not satisfy the contract.
    // The caller must not write/rebind the inputs before that retirement.
    std::shared_ptr<void> resource_lease;
};

// Pure request validation, before image allocation, native recording or SDK use.
inline DlssStatus ValidateDlssPresentationInput(const DlssPresentationInput& request,
                                               uint64_t last_frame_id = 0) {
    if (request.frame_id == 0 || request.frame_id <= last_frame_id)
        return {DlssStatusCode::InvalidArgument, 0, "DLSS requires a new monotonic guest frame"};
    if (!request.backend || !request.resource_lease)
        return {DlssStatusCode::InvalidArgument, 0, "DLSS requires backend and owning resource lease"};
    auto status = DlssVulkan::ValidateFrame(request.configuration, request.inputs);
    if (!status) return status;
    if (request.configuration.hdr)
        return {DlssStatusCode::Unsupported, 0, "HDR DLSS output needs tone mapping before SDR presentation"};
    if ((request.inputs.output.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0)
        return {DlssStatusCode::InvalidArgument, 0, "DLSS presentation output needs transfer-source usage"};
    const std::array<VkImage, 5> images{request.inputs.color.image, request.inputs.output.image,
        request.inputs.depth.image, request.inputs.motion_vectors.image, request.inputs.exposure.image};
    for (std::size_t first = 0; first < images.size(); ++first)
        for (std::size_t second = first + 1; second < images.size(); ++second)
            if (images[first] != VK_NULL_HANDLE && images[first] == images[second])
                return {DlssStatusCode::InvalidArgument, 0, "DLSS presentation roles must use distinct full images"};
    for (const auto& image : std::array<DlssImage, 5>{request.inputs.color, request.inputs.output,
            request.inputs.depth, request.inputs.motion_vectors, request.inputs.exposure})
        if (image.image != VK_NULL_HANDLE && (image.subresource.baseMipLevel != 0 || image.subresource.baseArrayLayer != 0))
            return {DlssStatusCode::InvalidArgument, 0, "DLSS presentation supports only mip/layer zero"};
    return {};
}
inline bool DlssPresentationConfigurationMatches(const DlssConfiguration& left,
                                                 const DlssConfiguration& right) noexcept {
    return left.render == right.render && left.output == right.output && left.quality == right.quality &&
        left.preset == right.preset && left.hdr == right.hdr && left.depth_inverted == right.depth_inverted &&
        left.motion_vectors_at_render_resolution == right.motion_vectors_at_render_resolution &&
        left.motion_vectors_jittered == right.motion_vectors_jittered && left.auto_exposure == right.auto_exposure;
}
} // namespace Libs::Graphics
#endif
