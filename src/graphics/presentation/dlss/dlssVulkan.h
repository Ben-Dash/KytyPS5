#ifndef KYTY_GRAPHICS_DLSS_VULKAN_H_
#define KYTY_GRAPHICS_DLSS_VULKAN_H_

#include <vulkan/vulkan.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace Libs::Graphics {

enum class DlssStatusCode { Success, Unsupported, InvalidArgument, InvalidState, NativeError };

struct DlssStatus {
	DlssStatusCode code = DlssStatusCode::Success;
	uint32_t native_result = 0;
	const char* message = "success";
	[[nodiscard]] explicit operator bool() const noexcept { return code == DlssStatusCode::Success; }
};

enum class DlssQuality { Quality, Balanced, Performance, UltraPerformance, Dlaa };
enum class DlssPreset { Default, TransformerK, TransformerL, TransformerM };

struct DlssExtent {
	uint32_t width = 0;
	uint32_t height = 0;
	[[nodiscard]] bool operator==(const DlssExtent&) const = default;
};

struct DlssProjectInfo {
	std::string project_id;
	std::string engine_version = "KytyPS5";
	std::wstring application_data_path;
	std::wstring feature_search_path;
};

struct DlssInitializeInfo {
	DlssProjectInfo project;
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physical_device = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
	PFN_vkGetDeviceProcAddr get_device_proc_addr = nullptr;
	// These must describe what was enabled, not everything the driver advertised.
	std::vector<std::string> enabled_instance_extensions;
	std::vector<std::string> enabled_device_extensions;
};

struct DlssOptimalSettings {
	DlssExtent optimal;
	DlssExtent minimum;
	DlssExtent maximum;
	float sharpness = 0.0f;
};

struct DlssConfiguration {
	DlssExtent render;
	DlssExtent output;
	DlssQuality quality = DlssQuality::Quality;
	DlssPreset preset = DlssPreset::Default;
	bool hdr = true;
	bool depth_inverted = false;
	bool motion_vectors_at_render_resolution = true;
	bool motion_vectors_jittered = false;
	bool auto_exposure = true;
};

struct DlssImage {
	VkImage image = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkFormat format = VK_FORMAT_UNDEFINED;
	DlssExtent extent;
	VkImageSubresourceRange subresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkImageUsageFlags usage = 0;
	VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};

// These inputs are semantic engine outputs. The backend never derives missing motion,
// depth, jitter or exposure from a final screenshot or substitutes fabricated textures.
struct DlssFrameInputs {
	DlssImage color;
	DlssImage output;
	DlssImage depth;
	DlssImage motion_vectors;
	DlssImage exposure; // Optional only when auto_exposure was enabled at creation.
	DlssExtent render;
	// NGX uses render-pixel jitter and scales motion to render PIXELS (not Streamline's NDC).
	float jitter_x = std::numeric_limits<float>::quiet_NaN();
	float jitter_y = std::numeric_limits<float>::quiet_NaN();
	float motion_scale_x = std::numeric_limits<float>::quiet_NaN();
	float motion_scale_y = std::numeric_limits<float>::quiet_NaN();
	float pre_exposure = 1.0f;
	float exposure_scale = 1.0f;
	float frame_time_ms = std::numeric_limits<float>::quiet_NaN();
	bool reset_history = false;
};

class DlssVulkan {
public:
	DlssVulkan();
	// Owner must drain GPU use before destruction, as for the native VkDevice/resources.
	~DlssVulkan();
	DlssVulkan(const DlssVulkan&) = delete;
	DlssVulkan& operator=(const DlssVulkan&) = delete;

	// Query before creating the corresponding Vulkan object; do not hardcode extensions.
	static DlssStatus QueryInstanceExtensions(const DlssProjectInfo& project,
	                                         std::vector<std::string>& extensions);
	static DlssStatus QueryDeviceExtensions(const DlssProjectInfo& project, VkInstance instance,
	                                       VkPhysicalDevice physical_device,
	                                       std::vector<std::string>& extensions);
	DlssStatus Initialize(const DlssInitializeInfo& info);
	DlssStatus GetOptimalSettings(DlssExtent output, DlssQuality quality,
	                              DlssOptimalSettings& settings);

	// Records NGX commands into an existing recording primary graphics command buffer,
	// outside a render pass. No queue submit, wait, layout transition or semaphore mutation.
	// Owner supplies same-queue dependencies and keeps all inputs/features alive to completion.
	DlssStatus RecordCreate(VkCommandBuffer command, const DlssConfiguration& configuration);
	DlssStatus RecordEvaluate(VkCommandBuffer command, const DlssFrameInputs& frame);
	// Pure validation is available for contract tests without loading NGX or submitting GPU work.
	static DlssStatus ValidateFrame(const DlssConfiguration& configuration,
	                                const DlssFrameInputs& frame);
	DlssStatus DestroyFeatureAfterGpuIdle();
	DlssStatus ShutdownAfterGpuIdle();
	[[nodiscard]] bool Initialized() const noexcept;
	[[nodiscard]] bool FeatureCreated() const noexcept;
	[[nodiscard]] VkDevice Device() const noexcept;
	[[nodiscard]] const DlssConfiguration* Configuration() const noexcept;

private:
	struct State;
	std::unique_ptr<State> m_state;
};

} // namespace Libs::Graphics

#endif // KYTY_GRAPHICS_DLSS_VULKAN_H_
