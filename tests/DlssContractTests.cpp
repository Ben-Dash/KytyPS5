#include "graphics/presentation/dlss/dlssVulkan.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

using namespace Libs::Graphics;

namespace {
int failures = 0;
void Check(bool ok, const char* message) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}
DlssImage Image(uintptr_t id, VkFormat format, DlssExtent extent, bool output = false) {
	DlssImage image;
	image.image = reinterpret_cast<VkImage>(id);
	image.view = reinterpret_cast<VkImageView>(id + 100);
	image.format = format;
	image.extent = extent;
	image.layout = output ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	image.usage = output ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_SAMPLED_BIT;
	return image;
}
DlssFrameInputs Frame() {
	DlssFrameInputs f;
	f.render = {1280, 720};
	f.color = Image(1, VK_FORMAT_R16G16B16A16_SFLOAT, f.render);
	f.output = Image(2, VK_FORMAT_R16G16B16A16_SFLOAT, {1920, 1080}, true);
	f.depth = Image(3, VK_FORMAT_R32_SFLOAT, f.render);
	f.motion_vectors = Image(4, VK_FORMAT_R16G16_SFLOAT, f.render);
	f.jitter_x = 0.25f;
	f.jitter_y = -0.125f;
	f.motion_scale_x = 1.0f;
	f.motion_scale_y = 1.0f;
	f.frame_time_ms = 16.667f;
	f.reset_history = true;
	return f;
}
} // namespace

int main(int argc, char** argv) {
	DlssVulkan backend;
	Check(!backend.Initialized() && !backend.FeatureCreated() && backend.Device() == VK_NULL_HANDLE &&
	          backend.Configuration() == nullptr, "new backend owns no native state");
	Check(backend.RecordEvaluate(VK_NULL_HANDLE, {}).code == DlssStatusCode::InvalidState,
	      "evaluation before creation is rejected without native access");
	DlssOptimalSettings settings;
	Check(backend.GetOptimalSettings({1920, 1080}, DlssQuality::Quality, settings).code ==
	          DlssStatusCode::InvalidState, "optimal settings require runtime initialization");
	Check(bool(backend.ShutdownAfterGpuIdle()) && bool(backend.ShutdownAfterGpuIdle()),
	      "shutdown is idempotent without an initialized runtime");
	if (argc == 2 && std::strcmp(argv[1], "--expect-disabled") == 0) {
		std::vector<std::string> extensions {"old value"};
		Check(DlssVulkan::QueryInstanceExtensions({}, extensions).code == DlssStatusCode::Unsupported &&
		          extensions.empty(), "disabled backend needs no runtime or SDK and clears discovery output");
		Check(backend.Initialize({}).code == DlssStatusCode::Unsupported,
		      "disabled backend explicitly reports Unsupported");
	}
	DlssConfiguration c;
	c.render = {1280, 720};
	c.output = {1920, 1080};
	auto good = Frame();
	const auto status = DlssVulkan::ValidateFrame(c, good);
	Check(static_cast<bool>(status), "well-formed temporal frame is accepted without loading NGX");
	if (!status) std::fprintf(stderr, "validation status=%u: %s\n", unsigned(status.code), status.message);
	const auto invalid = [&](DlssConfiguration config, DlssFrameInputs f, const char* message) {
		Check(DlssVulkan::ValidateFrame(config, f).code == DlssStatusCode::InvalidArgument, message);
	};
	auto f = good; f.motion_vectors = {}; invalid(c, f, "missing motion vectors fail closed");
	f = good; f.depth = {}; invalid(c, f, "missing depth fails closed");
	f = good; f.jitter_x = std::numeric_limits<float>::quiet_NaN(); invalid(c, f, "unknown jitter rejected");
	f = good; f.motion_scale_y = std::numeric_limits<float>::infinity(); invalid(c, f, "infinite motion scale rejected");
	f = good; f.motion_scale_x = 0; invalid(c, f, "zero motion scale cannot fabricate static vectors");
	f = good; f.frame_time_ms = 0; invalid(c, f, "missing frame duration rejected");
	f = good; f.pre_exposure = 0; invalid(c, f, "nonpositive pre-exposure rejected");
	f = good; f.color.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; invalid(c, f, "unreadable input layout rejected");
	f = good; f.output.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; invalid(c, f, "nonwritable output layout rejected");
	f = good; f.output.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT; invalid(c, f, "output requires storage usage");
	f = good; f.depth.extent.width = 1279; invalid(c, f, "undersized depth rejected");
	f = good; f.motion_vectors.extent.height = 719; invalid(c, f, "undersized motion rejected");
	f = good; f.output.extent.height = 1079; invalid(c, f, "undersized output rejected");
	f = good; f.color.samples = VK_SAMPLE_COUNT_4_BIT; invalid(c, f, "unresolved multisample input rejected");
	f = good; f.output.image = f.color.image; invalid(c, f, "in-place color/output alias rejected");
	f = good; f.output.image = f.depth.image; invalid(c, f, "output/depth alias rejected");
	f = good; f.color.subresource.levelCount = 2; invalid(c, f, "multimip semantic view rejected");
	f = good; f.depth.format = VK_FORMAT_R8G8B8A8_UNORM; invalid(c, f, "color texture cannot masquerade as depth");
	f = good; f.motion_vectors.format = VK_FORMAT_R32_SFLOAT; invalid(c, f, "scalar texture cannot masquerade as motion");
	auto no_exposure = c; no_exposure.auto_exposure = false;
	invalid(no_exposure, good, "manual exposure requires exposure resource");
	f = good; f.exposure = Image(5, VK_FORMAT_R32_SFLOAT, {1, 1});
	Check(static_cast<bool>(DlssVulkan::ValidateFrame(no_exposure, f)), "explicit manual exposure accepted");
	auto full_motion = c; full_motion.motion_vectors_at_render_resolution = false;
	invalid(full_motion, good, "output-resolution motion requires output-sized resource");
	f = good; f.motion_vectors.extent = c.output;
	Check(static_cast<bool>(DlssVulkan::ValidateFrame(full_motion, f)), "output-resolution motion accepted");
	auto dlaa = c; dlaa.output = dlaa.render; dlaa.quality = DlssQuality::Dlaa;
	f = good; f.output.extent = dlaa.output;
	Check(static_cast<bool>(DlssVulkan::ValidateFrame(dlaa, f)), "DLAA supports matching render/output extents");
	if (!failures) std::puts("DlssContractTests: valid temporal data and invalid boundaries PASS");
	return failures ? 1 : 0;
}
