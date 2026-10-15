#include "graphics/presentation/dlss/dlssPresentation.h"
#include <cstdio>
#include <limits>

using namespace Libs::Graphics;
namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}
DlssImage NativeImage(uintptr_t id, VkFormat format, DlssExtent extent, bool output = false) {
    DlssImage image;
    image.image = reinterpret_cast<VkImage>(id);
    image.view = reinterpret_cast<VkImageView>(id + 100);
    image.format = format;
    image.extent = extent;
    image.layout = output ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image.usage = output ? VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT : VK_IMAGE_USAGE_SAMPLED_BIT;
    return image;
}
DlssPresentationInput Input() {
    DlssPresentationInput input;
    input.frame_id = 7;
    input.backend = std::make_shared<DlssVulkan>(); // No Initialize/driver/library calls.
    input.resource_lease = std::make_shared<int>(1);
    input.configuration.render = {1280, 720};
    input.configuration.output = {1920, 1080};
    input.configuration.hdr = false; // Already display encoded, matching the current swapchain.
    auto& frame = input.inputs;
    frame.render = input.configuration.render;
    frame.color = NativeImage(1, VK_FORMAT_R16G16B16A16_SFLOAT, frame.render);
    frame.output = NativeImage(2, VK_FORMAT_R16G16B16A16_SFLOAT, input.configuration.output, true);
    frame.depth = NativeImage(3, VK_FORMAT_R32_SFLOAT, frame.render);
    frame.motion_vectors = NativeImage(4, VK_FORMAT_R16G16_SFLOAT, frame.render);
    frame.jitter_x = 0.25f; frame.jitter_y = -0.125f;
    frame.motion_scale_x = frame.motion_scale_y = 1.0f;
    frame.frame_time_ms = 16.667f;
    frame.reset_history = true;
    return input;
}
}
int main() {
    // Fake native identifiers are ONLY pure validation inputs and are never submitted.
    // This target does not initialize NGX, Vulkan, or a window; it is not GPU proof.
    const auto good = Input();
    const auto accepted = ValidateDlssPresentationInput(good, 6);
    Check(static_cast<bool>(accepted), "complete explicitly owned frame can be planned");
    auto hdr = good; hdr.configuration.hdr = true;
    Check(ValidateDlssPresentationInput(hdr, 6).code == DlssStatusCode::Unsupported,
          "HDR scene output requires tone mapping before the current SDR swapchain");
    if (!accepted) std::fprintf(stderr, "status=%u: %s\n", unsigned(accepted.code), accepted.message);
    const auto rejected = [&](const DlssPresentationInput& input, const char* what, uint64_t last = 6) {
        Check(ValidateDlssPresentationInput(input, last).code == DlssStatusCode::InvalidArgument, what);
    };
    auto input = good; input.frame_id = 0; rejected(input, "zero frame identity rejected");
    input = good; rejected(input, "same frame cannot advance history twice", 7);
    input = good; rejected(input, "older frame rejected", 8);
    input = good; input.backend.reset(); rejected(input, "absent explicit backend rejected");
    input = good; input.resource_lease.reset(); rejected(input, "unowned native resources rejected");
    input = good; input.inputs.color = {}; rejected(input, "missing color rejected");
    input = good; input.inputs.depth = {}; rejected(input, "missing depth rejected");
    input = good; input.inputs.motion_vectors = {}; rejected(input, "missing motion rejected");
    input = good; input.inputs.output.usage &= ~VK_IMAGE_USAGE_TRANSFER_SRC_BIT; rejected(input, "noncopyable output rejected");
    input = good; input.inputs.output.image = input.inputs.color.image; rejected(input, "output/input alias rejected");
    input = good; input.inputs.depth.image = input.inputs.color.image; rejected(input, "distinct full-image semantics required");
    input = good; input.inputs.depth.subresource.baseMipLevel = 1; rejected(input, "nonzero mip rejected by current copy contract");
    input = good; input.inputs.output.subresource.baseArrayLayer = 1; rejected(input, "nonzero output layer rejected by current copy contract");
    input = good; input.inputs.output.extent.width = 1919; rejected(input, "undersized output rejected");
    input = good; input.inputs.depth.format = VK_FORMAT_R8G8B8A8_UNORM; rejected(input, "color cannot masquerade as depth");
    input = good; input.inputs.motion_vectors.format = VK_FORMAT_R32_SFLOAT; rejected(input, "scalar cannot masquerade as motion");
    input = good; input.inputs.jitter_x = std::numeric_limits<float>::quiet_NaN(); rejected(input, "unknown jitter rejected");
    input = good; input.inputs.motion_scale_y = std::numeric_limits<float>::infinity(); rejected(input, "nonfinite motion scale rejected");
    input = good; input.inputs.frame_time_ms = 0; rejected(input, "unknown frame duration rejected");
    const auto config = good.configuration;
    Check(DlssPresentationConfigurationMatches(config, config), "actual created configuration matches itself");
    auto changed = config; changed.render.width--; Check(!DlssPresentationConfigurationMatches(config, changed), "render extent mismatch rejected");
    changed = config; changed.output.width--; Check(!DlssPresentationConfigurationMatches(config, changed), "output extent mismatch rejected");
    changed = config; changed.quality = DlssQuality::Balanced; Check(!DlssPresentationConfigurationMatches(config, changed), "quality mismatch rejected");
    changed = config; changed.preset = DlssPreset::TransformerM; Check(!DlssPresentationConfigurationMatches(config, changed), "preset mismatch rejected");
    changed = config; changed.hdr = !config.hdr; Check(!DlssPresentationConfigurationMatches(config, changed), "HDR mismatch rejected");
    changed = config; changed.depth_inverted = !config.depth_inverted; Check(!DlssPresentationConfigurationMatches(config, changed), "depth convention mismatch rejected");
    changed = config; changed.motion_vectors_at_render_resolution = !config.motion_vectors_at_render_resolution; Check(!DlssPresentationConfigurationMatches(config, changed), "motion extent mode mismatch rejected");
    changed = config; changed.motion_vectors_jittered = !config.motion_vectors_jittered; Check(!DlssPresentationConfigurationMatches(config, changed), "motion jitter convention mismatch rejected");
    changed = config; changed.auto_exposure = !config.auto_exposure; Check(!DlssPresentationConfigurationMatches(config, changed), "exposure mode mismatch rejected");
    if (!failures) std::puts("DlssPresentationContractTests: explicit frame inputs and failure boundaries PASS (CPU only)");
    return failures ? 1 : 0;
}
