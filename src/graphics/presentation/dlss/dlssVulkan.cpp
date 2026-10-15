#include "graphics/presentation/dlss/dlssVulkan.h"

#include <algorithm>
#include <cmath>
#include <mutex>

#if defined(KYTY_DLSS) && KYTY_DLSS
#include <nvsdk_ngx_helpers_vk.h>
#endif

namespace Libs::Graphics {
namespace {

DlssStatus Invalid(const char* message) {
	return {DlssStatusCode::InvalidArgument, 0, message};
}

DlssStatus WrongState(const char* message) {
	return {DlssStatusCode::InvalidState, 0, message};
}

[[maybe_unused]] DlssStatus Unavailable() {
	return {DlssStatusCode::Unsupported, 0, "DLSS SDK support was not compiled"};
}

bool ExtentValid(DlssExtent value) { return value.width != 0 && value.height != 0; }
bool Contains(DlssExtent outer, DlssExtent inner) {
	return outer.width >= inner.width && outer.height >= inner.height;
}

DlssStatus ValidateConfiguration(const DlssConfiguration& config) {
	if (!ExtentValid(config.render) || !ExtentValid(config.output) ||
	    !Contains(config.output, config.render)) {
		return Invalid("DLSS requires positive render/output dimensions and output >= render");
	}
	if (config.quality < DlssQuality::Quality || config.quality > DlssQuality::Dlaa ||
	    config.preset < DlssPreset::Default || config.preset > DlssPreset::TransformerM) {
		return Invalid("unsupported DLSS quality or model preset");
	}
	if (config.quality == DlssQuality::Dlaa && config.render != config.output) {
		return Invalid("DLAA requires equal render and output dimensions");
	}
	return {};
}

bool ColorFormat(VkFormat format) {
	switch (format) {
		case VK_FORMAT_R16G16B16A16_SFLOAT:
		case VK_FORMAT_R32G32B32A32_SFLOAT:
		case VK_FORMAT_R16G16B16A16_UNORM:
		case VK_FORMAT_R8G8B8A8_UNORM:
		case VK_FORMAT_B8G8R8A8_UNORM:
		case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
		case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return true;
		default: return false;
	}
}

DlssStatus ValidateImage(const DlssImage& image, VkImageUsageFlags required_usage,
                        VkImageAspectFlags required_aspect) {
	if (image.image == VK_NULL_HANDLE || image.view == VK_NULL_HANDLE ||
	    image.format == VK_FORMAT_UNDEFINED || !ExtentValid(image.extent)) {
		return Invalid("DLSS image handle/view/format/extent is missing");
	}
	const bool output = (required_usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0;
	const bool valid_layout = image.layout == VK_IMAGE_LAYOUT_GENERAL ||
	                          (!output && image.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	if (!valid_layout || image.samples != VK_SAMPLE_COUNT_1_BIT ||
	    (image.usage & required_usage) != required_usage ||
	    image.subresource.aspectMask != required_aspect ||
	    image.subresource.levelCount != 1 || image.subresource.layerCount != 1) {
		return Invalid("DLSS requires readable inputs/GENERAL output and single-sample single-mip single-layer views with declared usage/aspect");
	}
	return {};
}

#if defined(KYTY_DLSS) && KYTY_DLSS

// NGX entry points/parameter maps are externally serialized, including discovery calls.
// This mutex never spans a host queue submit or a GPU wait.
std::mutex g_ngx_mutex;

DlssStatus Native(NVSDK_NGX_Result result, const char* message) {
	if (NVSDK_NGX_SUCCEED(result)) return {};
	const auto code = result == NVSDK_NGX_Result_FAIL_FeatureNotSupported
	                      ? DlssStatusCode::Unsupported : DlssStatusCode::NativeError;
	return {code, static_cast<uint32_t>(result), message};
}

struct ProjectContext {
	const wchar_t* paths[1] {};
	NVSDK_NGX_FeatureCommonInfo common {};
	NVSDK_NGX_FeatureDiscoveryInfo discovery {};

	explicit ProjectContext(const DlssProjectInfo& project) {
		if (!project.feature_search_path.empty()) {
			paths[0] = project.feature_search_path.c_str();
			common.PathListInfo = {paths, 1};
		}
		discovery.SDKVersion = NVSDK_NGX_Version_API;
		discovery.FeatureID = NVSDK_NGX_Feature_SuperSampling;
		discovery.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
		discovery.Identifier.v.ProjectDesc = {project.project_id.c_str(),
		    NVSDK_NGX_ENGINE_TYPE_CUSTOM, project.engine_version.c_str()};
		discovery.ApplicationDataPath = project.application_data_path.c_str();
		discovery.FeatureInfo = &common;
	}
};

bool ProjectValid(const DlssProjectInfo& project) {
	return !project.project_id.empty() && !project.engine_version.empty() &&
	       !project.application_data_path.empty();
}

void CopyExtensions(uint32_t count, const VkExtensionProperties* properties,
                    std::vector<std::string>& extensions) {
	extensions.clear();
	for (uint32_t index = 0; index < count; ++index) {
		const std::string name(properties[index].extensionName);
		if (std::find(extensions.begin(), extensions.end(), name) == extensions.end()) {
			extensions.push_back(name);
		}
	}
}

bool HasExtensions(const std::vector<std::string>& enabled,
                    const std::vector<std::string>& required) {
	return std::all_of(required.begin(), required.end(), [&](const auto& name) {
		return std::find(enabled.begin(), enabled.end(), name) != enabled.end();
	});
}

NVSDK_NGX_PerfQuality_Value QualityValue(DlssQuality quality) {
	switch (quality) {
		case DlssQuality::Quality: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
		case DlssQuality::Balanced: return NVSDK_NGX_PerfQuality_Value_Balanced;
		case DlssQuality::Performance: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
		case DlssQuality::UltraPerformance: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
		case DlssQuality::Dlaa: return NVSDK_NGX_PerfQuality_Value_DLAA;
	}
	return NVSDK_NGX_PerfQuality_Value_MaxQuality;
}

NVSDK_NGX_DLSS_Hint_Render_Preset PresetValue(DlssPreset preset) {
	switch (preset) {
		case DlssPreset::Default: return NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
		case DlssPreset::TransformerK: return NVSDK_NGX_DLSS_Hint_Render_Preset_K;
		case DlssPreset::TransformerL: return NVSDK_NGX_DLSS_Hint_Render_Preset_L;
		case DlssPreset::TransformerM: return NVSDK_NGX_DLSS_Hint_Render_Preset_M;
	}
	return NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
}

DlssStatus OptimalSettings(NVSDK_NGX_Parameter* parameters, DlssExtent output,
                           DlssQuality quality, DlssOptimalSettings& settings) {
	if (!ExtentValid(output) || quality < DlssQuality::Quality || quality > DlssQuality::Dlaa) {
		return Invalid("DLSS optimal-settings dimensions or quality are invalid");
	}
	void* callback = nullptr;
	auto result = NVSDK_NGX_Parameter_GetVoidPointer(
	    parameters, NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, &callback);
	if (NVSDK_NGX_FAILED(result) || callback == nullptr) {
		return {DlssStatusCode::Unsupported, static_cast<uint32_t>(result),
		        "NGX optimal-settings callback is unavailable"};
	}
	NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_Width, output.width);
	NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_Height, output.height);
	NVSDK_NGX_Parameter_SetI(parameters, NVSDK_NGX_Parameter_PerfQualityValue, QualityValue(quality));
	NVSDK_NGX_Parameter_SetI(parameters, NVSDK_NGX_Parameter_RTXValue, 0);
	result = reinterpret_cast<PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback>(callback)(parameters);
	if (NVSDK_NGX_FAILED(result)) return Native(result, "NGX optimal-settings query failed");
	DlssOptimalSettings proposed {};
	result = NVSDK_NGX_Parameter_GetUI(parameters, NVSDK_NGX_Parameter_OutWidth, &proposed.optimal.width);
	if (NVSDK_NGX_FAILED(result)) return Native(result, "NGX optimal render width is missing");
	result = NVSDK_NGX_Parameter_GetUI(parameters, NVSDK_NGX_Parameter_OutHeight, &proposed.optimal.height);
	if (NVSDK_NGX_FAILED(result)) return Native(result, "NGX optimal render height is missing");
	proposed.minimum = proposed.maximum = proposed.optimal;
	// Older supported runtimes expose only a fixed optimum, which is a valid narrow range.
	(void)NVSDK_NGX_Parameter_GetUI(parameters, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width,
	                               &proposed.minimum.width);
	(void)NVSDK_NGX_Parameter_GetUI(parameters, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height,
	                               &proposed.minimum.height);
	(void)NVSDK_NGX_Parameter_GetUI(parameters, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width,
	                               &proposed.maximum.width);
	(void)NVSDK_NGX_Parameter_GetUI(parameters, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height,
	                               &proposed.maximum.height);
	(void)NVSDK_NGX_Parameter_GetF(parameters, NVSDK_NGX_Parameter_Sharpness, &proposed.sharpness);
	if (!ExtentValid(proposed.minimum) || !Contains(proposed.maximum, proposed.optimal) ||
	    !Contains(proposed.optimal, proposed.minimum) || !Contains(output, proposed.maximum)) {
		return {DlssStatusCode::NativeError, 0, "NGX returned an inconsistent render-size range"};
	}
	settings = proposed;
	return {};
}

NVSDK_NGX_Resource_VK Resource(const DlssImage& image, bool writable) {
	return NVSDK_NGX_Create_ImageView_Resource_VK(image.view, image.image, image.subresource,
	    image.format, image.extent.width, image.extent.height, writable);
}

#endif
} // namespace

struct DlssVulkan::State {
	bool initialized = false;
	bool created = false;
	bool evaluated = false;
	DlssConfiguration configuration {};
	DlssOptimalSettings settings {};
#if defined(KYTY_DLSS) && KYTY_DLSS
	DlssInitializeInfo info {};
	NVSDK_NGX_Parameter* capability = nullptr;
	NVSDK_NGX_Parameter* parameters = nullptr;
	NVSDK_NGX_Handle* feature = nullptr;
	PFN_vkGetPhysicalDeviceFormatProperties get_format_properties = nullptr;
#endif
};

DlssVulkan::DlssVulkan(): m_state(std::make_unique<State>()) {}
DlssVulkan::~DlssVulkan() { (void)ShutdownAfterGpuIdle(); }

bool DlssVulkan::Initialized() const noexcept { return m_state->initialized; }
bool DlssVulkan::FeatureCreated() const noexcept { return m_state->created; }
VkDevice DlssVulkan::Device() const noexcept {
#if defined(KYTY_DLSS) && KYTY_DLSS
	return m_state->initialized ? m_state->info.device : VK_NULL_HANDLE;
#else
	return VK_NULL_HANDLE;
#endif
}
const DlssConfiguration* DlssVulkan::Configuration() const noexcept {
	return m_state->created ? &m_state->configuration : nullptr;
}

DlssStatus DlssVulkan::QueryInstanceExtensions(const DlssProjectInfo& project,
                                              std::vector<std::string>& extensions) {
	extensions.clear();
#if defined(KYTY_DLSS) && KYTY_DLSS
	if (!ProjectValid(project)) return Invalid("DLSS project identifier/version/data path is missing");
	ProjectContext context(project);
	std::lock_guard lock(g_ngx_mutex);
	uint32_t count = 0;
	VkExtensionProperties* properties = nullptr;
	const auto result = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(
	    &context.discovery, &count, &properties);
	if (NVSDK_NGX_FAILED(result)) return Native(result, "NGX instance-extension discovery failed");
	if (count != 0 && properties == nullptr) return WrongState("NGX returned no instance-extension data");
	CopyExtensions(count, properties, extensions);
	return {};
#else
	(void)project;
	return Unavailable();
#endif
}

DlssStatus DlssVulkan::QueryDeviceExtensions(const DlssProjectInfo& project, VkInstance instance,
                                            VkPhysicalDevice physical_device,
                                            std::vector<std::string>& extensions) {
	extensions.clear();
#if defined(KYTY_DLSS) && KYTY_DLSS
	if (!ProjectValid(project) || instance == VK_NULL_HANDLE || physical_device == VK_NULL_HANDLE) {
		return Invalid("DLSS device-extension discovery requires project/instance/physical device");
	}
	ProjectContext context(project);
	std::lock_guard lock(g_ngx_mutex);
	uint32_t count = 0;
	VkExtensionProperties* properties = nullptr;
	const auto result = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
	    instance, physical_device, &context.discovery, &count, &properties);
	if (NVSDK_NGX_FAILED(result)) return Native(result, "NGX device-extension discovery failed");
	if (count != 0 && properties == nullptr) return WrongState("NGX returned no device-extension data");
	CopyExtensions(count, properties, extensions);
	return {};
#else
	(void)project;
	(void)instance;
	(void)physical_device;
	return Unavailable();
#endif
}

DlssStatus DlssVulkan::Initialize(const DlssInitializeInfo& info) {
	if (m_state->initialized) return WrongState("DLSS is already initialized");
#if defined(KYTY_DLSS) && KYTY_DLSS
	if (!ProjectValid(info.project) || info.instance == VK_NULL_HANDLE ||
	    info.physical_device == VK_NULL_HANDLE || info.device == VK_NULL_HANDLE ||
	    info.get_instance_proc_addr == nullptr || info.get_device_proc_addr == nullptr) {
		return Invalid("DLSS initialization requires explicit project/Vulkan handles/dispatch functions");
	}
	std::vector<std::string> required;
	auto status = QueryInstanceExtensions(info.project, required);
	if (!status) return status;
	if (!HasExtensions(info.enabled_instance_extensions, required)) {
		return {DlssStatusCode::Unsupported, 0, "required NGX Vulkan instance extension was not enabled"};
	}
	status = QueryDeviceExtensions(info.project, info.instance, info.physical_device, required);
	if (!status) return status;
	if (!HasExtensions(info.enabled_device_extensions, required)) {
		return {DlssStatusCode::Unsupported, 0, "required NGX Vulkan device extension was not enabled"};
	}
	const auto get_format = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
	    info.get_instance_proc_addr(info.instance, "vkGetPhysicalDeviceFormatProperties"));
	if (get_format == nullptr) return Invalid("Vulkan format-capability query entry point is missing");
	m_state->info = info;
	ProjectContext context(m_state->info.project);
	std::lock_guard lock(g_ngx_mutex);
	NVSDK_NGX_FeatureRequirement requirements {};
	auto result = NVSDK_NGX_VULKAN_GetFeatureRequirements(info.instance, info.physical_device,
	                                                    &context.discovery, &requirements);
	if (NVSDK_NGX_FAILED(result)) return Native(result, "NGX feature-requirements query failed");
	if (requirements.FeatureSupported != NVSDK_NGX_FeatureSupportResult_Supported) {
		return {DlssStatusCode::Unsupported, static_cast<uint32_t>(requirements.FeatureSupported),
		        "NGX reports unsupported hardware/driver/OS for Super Resolution"};
	}
	result = NVSDK_NGX_VULKAN_Init_with_ProjectID(info.project.project_id.c_str(),
	    NVSDK_NGX_ENGINE_TYPE_CUSTOM, info.project.engine_version.c_str(),
	    info.project.application_data_path.c_str(), info.instance, info.physical_device, info.device,
	    info.get_instance_proc_addr, info.get_device_proc_addr, &context.common, NVSDK_NGX_Version_API);
	if (NVSDK_NGX_FAILED(result)) return Native(result, "NGX Vulkan initialization failed");
	m_state->initialized = true;
	result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&m_state->capability);
	int available = 0;
	if (NVSDK_NGX_SUCCEED(result) && m_state->capability != nullptr) {
		result = NVSDK_NGX_Parameter_GetI(m_state->capability,
		    NVSDK_NGX_Parameter_SuperSampling_Available, &available);
	}
	if (NVSDK_NGX_FAILED(result) || m_state->capability == nullptr || available == 0) {
		if (m_state->capability != nullptr) {
			(void)NVSDK_NGX_VULKAN_DestroyParameters(m_state->capability);
			m_state->capability = nullptr;
		}
		(void)NVSDK_NGX_VULKAN_Shutdown1(info.device);
		m_state->initialized = false;
		return {DlssStatusCode::Unsupported, static_cast<uint32_t>(result),
		        "NGX Super Resolution capability is unavailable"};
	}
	result = NVSDK_NGX_VULKAN_AllocateParameters(&m_state->parameters);
	if (NVSDK_NGX_FAILED(result) || m_state->parameters == nullptr) {
		(void)NVSDK_NGX_VULKAN_DestroyParameters(m_state->capability);
		m_state->capability = nullptr;
		(void)NVSDK_NGX_VULKAN_Shutdown1(info.device);
		m_state->initialized = false;
		return Native(NVSDK_NGX_FAILED(result) ? result : NVSDK_NGX_Result_Fail,
		              "NGX feature-parameter allocation failed");
	}
	m_state->get_format_properties = get_format;
	return {};
#else
	(void)info;
	return Unavailable();
#endif
}

DlssStatus DlssVulkan::GetOptimalSettings(DlssExtent output, DlssQuality quality,
                                         DlssOptimalSettings& settings) {
	if (!m_state->initialized) return WrongState("DLSS optimal settings require initialization");
#if defined(KYTY_DLSS) && KYTY_DLSS
	std::lock_guard lock(g_ngx_mutex);
	return OptimalSettings(m_state->capability, output, quality, settings);
#else
	(void)output;
	(void)quality;
	(void)settings;
	return Unavailable();
#endif
}

DlssStatus DlssVulkan::RecordCreate(VkCommandBuffer command,
                                   const DlssConfiguration& configuration) {
	if (const auto status = ValidateConfiguration(configuration); !status) return status;
	if (command == VK_NULL_HANDLE) return Invalid("DLSS creation requires a recording command buffer");
	if (!m_state->initialized || m_state->created) return WrongState("DLSS creation requires an initialized backend without an existing feature");
#if defined(KYTY_DLSS) && KYTY_DLSS
	std::lock_guard lock(g_ngx_mutex);
	DlssOptimalSettings settings;
	if (const auto status = OptimalSettings(m_state->capability, configuration.output,
	                                       configuration.quality, settings); !status) return status;
	if (!Contains(configuration.render, settings.minimum) || !Contains(settings.maximum, configuration.render)) {
		return Invalid("DLSS render extent is outside the runtime's supported settings range");
	}
	const auto preset = static_cast<unsigned int>(PresetValue(configuration.preset));
	for (const auto* key: {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
	    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
	    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
	    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
	    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance}) {
		NVSDK_NGX_Parameter_SetUI(m_state->parameters, key, preset);
	}
	NVSDK_NGX_DLSS_Create_Params create {};
	create.Feature.InWidth = configuration.render.width;
	create.Feature.InHeight = configuration.render.height;
	create.Feature.InTargetWidth = configuration.output.width;
	create.Feature.InTargetHeight = configuration.output.height;
	create.Feature.InPerfQualityValue = QualityValue(configuration.quality);
	create.InFeatureCreateFlags =
	    (configuration.hdr ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR : 0) |
	    (configuration.depth_inverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0) |
	    (configuration.motion_vectors_at_render_resolution ? NVSDK_NGX_DLSS_Feature_Flags_MVLowRes : 0) |
	    (configuration.motion_vectors_jittered ? NVSDK_NGX_DLSS_Feature_Flags_MVJittered : 0) |
	    (configuration.auto_exposure ? NVSDK_NGX_DLSS_Feature_Flags_AutoExposure : 0);
	const auto result = NGX_VULKAN_CREATE_DLSS_EXT1(m_state->info.device, command, 1, 1,
	    &m_state->feature, m_state->parameters, &create);
	if (NVSDK_NGX_FAILED(result)) return Native(result, "NGX Super Resolution feature creation failed");
	if (m_state->feature == nullptr) return WrongState("NGX creation returned a null feature");
	m_state->configuration = configuration;
	m_state->settings = settings;
	m_state->created = true;
	m_state->evaluated = false;
	return {};
#else
	return Unavailable();
#endif
}

DlssStatus DlssVulkan::ValidateFrame(const DlssConfiguration& config,
                                    const DlssFrameInputs& frame) {
	if (const auto status = ValidateConfiguration(config); !status) return status;
	if (!ExtentValid(frame.render) || !Contains(config.render, frame.render) ||
	    frame.output.extent != config.output || !Contains(frame.color.extent, frame.render) ||
	    !Contains(frame.depth.extent, frame.render)) {
		return Invalid("DLSS frame dimensions do not fit the created feature");
	}
	for (const auto* image: {&frame.color, &frame.motion_vectors}) {
		if (const auto status = ValidateImage(*image, VK_IMAGE_USAGE_SAMPLED_BIT,
		                                      VK_IMAGE_ASPECT_COLOR_BIT); !status) return status;
	}
	if (const auto status = ValidateImage(frame.output, VK_IMAGE_USAGE_STORAGE_BIT,
	                                      VK_IMAGE_ASPECT_COLOR_BIT); !status) return status;
	const bool native_depth = frame.depth.format == VK_FORMAT_D32_SFLOAT ||
	                          frame.depth.format == VK_FORMAT_D32_SFLOAT_S8_UINT;
	if (const auto status = ValidateImage(frame.depth, VK_IMAGE_USAGE_SAMPLED_BIT,
	    native_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT); !status) return status;
	if (!ColorFormat(frame.color.format) || !ColorFormat(frame.output.format) ||
	    (frame.depth.format != VK_FORMAT_R32_SFLOAT && !native_depth) ||
	    (frame.motion_vectors.format != VK_FORMAT_R16G16_SFLOAT &&
	     frame.motion_vectors.format != VK_FORMAT_R32G32_SFLOAT)) {
		return Invalid("unsupported DLSS color/depth/motion-vector format");
	}
	const auto required_motion = config.motion_vectors_at_render_resolution ? frame.render : config.output;
	if (!Contains(frame.motion_vectors.extent, required_motion)) {
		return Invalid("DLSS motion-vector extent does not match its resolution convention");
	}
	for (const auto* input: {&frame.color, &frame.depth, &frame.motion_vectors}) {
		if (input->image == frame.output.image) return Invalid("DLSS output must not alias an input image");
	}
	if (!std::isfinite(frame.jitter_x) || !std::isfinite(frame.jitter_y) ||
	    !std::isfinite(frame.motion_scale_x) || !std::isfinite(frame.motion_scale_y) ||
	    frame.motion_scale_x == 0.0f || frame.motion_scale_y == 0.0f ||
	    !std::isfinite(frame.pre_exposure) || frame.pre_exposure <= 0.0f ||
	    !std::isfinite(frame.exposure_scale) || frame.exposure_scale <= 0.0f ||
	    !std::isfinite(frame.frame_time_ms) || frame.frame_time_ms <= 0.0f) {
		return Invalid("DLSS temporal/exposure constants must be explicit finite values with positive time/exposure");
	}
	const bool has_exposure = frame.exposure.image != VK_NULL_HANDLE || frame.exposure.view != VK_NULL_HANDLE;
	if (!config.auto_exposure && !has_exposure) return Invalid("DLSS explicit-exposure mode requires an exposure texture");
	if (has_exposure) {
		if (const auto status = ValidateImage(frame.exposure, VK_IMAGE_USAGE_SAMPLED_BIT,
		                                      VK_IMAGE_ASPECT_COLOR_BIT); !status) return status;
		if (frame.exposure.extent != DlssExtent {1, 1} || frame.exposure.format != VK_FORMAT_R32_SFLOAT ||
		    frame.exposure.image == frame.output.image) {
			return Invalid("DLSS exposure must be a non-aliasing R32_SFLOAT 1x1 texture");
		}
	}
	return {};
}

DlssStatus DlssVulkan::RecordEvaluate(VkCommandBuffer command, const DlssFrameInputs& frame) {
	if (!m_state->initialized || !m_state->created) return WrongState("DLSS evaluation requires a created feature");
	if (command == VK_NULL_HANDLE) return Invalid("DLSS evaluation requires a recording command buffer");
	if (const auto status = ValidateFrame(m_state->configuration, frame); !status) return status;
	if (!m_state->evaluated && !frame.reset_history) return Invalid("first DLSS evaluation must reset temporal history");
	if (!Contains(frame.render, m_state->settings.minimum)) return Invalid("DLSS frame render extent is below the runtime minimum");
#if defined(KYTY_DLSS) && KYTY_DLSS
	std::lock_guard lock(g_ngx_mutex);
	for (const auto* image: {&frame.color, &frame.depth, &frame.motion_vectors, &frame.output}) {
		VkFormatProperties properties {};
		m_state->get_format_properties(m_state->info.physical_device, image->format, &properties);
		const auto needed = image == &frame.output ? VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT
		                                         : VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
		if ((properties.optimalTilingFeatures & needed) == 0) {
			return {DlssStatusCode::Unsupported, 0, "DLSS image format lacks the required native sampling/storage capability"};
		}
	}
	auto color = Resource(frame.color, false);
	auto output = Resource(frame.output, true);
	auto depth = Resource(frame.depth, false);
	auto motion = Resource(frame.motion_vectors, false);
	auto exposure = Resource(frame.exposure, false);
	NVSDK_NGX_VK_DLSS_Eval_Params evaluate {};
	evaluate.Feature.pInColor = &color;
	evaluate.Feature.pInOutput = &output;
	evaluate.pInDepth = &depth;
	evaluate.pInMotionVectors = &motion;
	evaluate.pInExposureTexture = frame.exposure.image != VK_NULL_HANDLE ? &exposure : nullptr;
	evaluate.InJitterOffsetX = frame.jitter_x;
	evaluate.InJitterOffsetY = frame.jitter_y;
	evaluate.InRenderSubrectDimensions = {frame.render.width, frame.render.height};
	evaluate.InReset = frame.reset_history ? 1 : 0;
	evaluate.InMVScaleX = frame.motion_scale_x;
	evaluate.InMVScaleY = frame.motion_scale_y;
	evaluate.InPreExposure = frame.pre_exposure;
	evaluate.InExposureScale = frame.exposure_scale;
	evaluate.InFrameTimeDeltaInMsec = frame.frame_time_ms;
	const auto result = NGX_VULKAN_EVALUATE_DLSS_EXT(command, m_state->feature,
	                                               m_state->parameters, &evaluate);
	if (NVSDK_NGX_FAILED(result)) return Native(result, "NGX Super Resolution evaluation failed");
	m_state->evaluated = true;
	return {};
#else
	return Unavailable();
#endif
}

DlssStatus DlssVulkan::DestroyFeatureAfterGpuIdle() {
#if defined(KYTY_DLSS) && KYTY_DLSS
	if (m_state->feature != nullptr) {
		std::lock_guard lock(g_ngx_mutex);
		const auto result = NVSDK_NGX_VULKAN_ReleaseFeature(m_state->feature);
		if (NVSDK_NGX_FAILED(result)) return Native(result, "NGX feature release failed");
		m_state->feature = nullptr;
	}
#endif
	m_state->created = false;
	m_state->evaluated = false;
	return {};
}

DlssStatus DlssVulkan::ShutdownAfterGpuIdle() {
	if (const auto status = DestroyFeatureAfterGpuIdle(); !status) return status;
#if defined(KYTY_DLSS) && KYTY_DLSS
	std::lock_guard lock(g_ngx_mutex);
	if (m_state->parameters != nullptr) {
		const auto result = NVSDK_NGX_VULKAN_DestroyParameters(m_state->parameters);
		if (NVSDK_NGX_FAILED(result)) return Native(result, "NGX parameter release failed");
		m_state->parameters = nullptr;
	}
	if (m_state->capability != nullptr) {
		const auto result = NVSDK_NGX_VULKAN_DestroyParameters(m_state->capability);
		if (NVSDK_NGX_FAILED(result)) return Native(result, "NGX capability release failed");
		m_state->capability = nullptr;
	}
	if (m_state->initialized) {
		const auto result = NVSDK_NGX_VULKAN_Shutdown1(m_state->info.device);
		if (NVSDK_NGX_FAILED(result)) return Native(result, "NGX Vulkan shutdown failed");
	}
#endif
	m_state->initialized = false;
	return {};
}

} // namespace Libs::Graphics
