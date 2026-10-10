#include "graphics/host_gpu/renderer/pipeline/dispatchReplay.h"

#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "graphics/host_gpu/renderer/cache/samplerCache.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/pipeline/shaderPrecompile.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/ir/BindlessBindings.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shader.h"
#include "graphics/shader/shaderCompiler.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <iterator>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <xxhash.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace Libs::Graphics::DispatchReplay {
namespace {

namespace fs = std::filesystem;
using Json   = nlohmann::ordered_json;

struct Options {
	std::vector<fs::path> snapshots;
	std::string           spv = "recompile"; // recompile | live | <file.spv>
	std::string           validate;          // "" | core | sync | gpuav
	fs::path              dump;
	uint32_t              timeout_ms = 5000;
	uint32_t              repeat     = 1;
};

void PrintUsage() {
	std::printf(
	    "kyty_emulator --dispatch-replay <snapshot dir | dir of snapshots> [options]\n"
	    "  --spv recompile|live|<file.spv>  shader to run (default: translate the record now)\n"
	    "  --validate core|sync|gpuav       Khronos validation layer (gpuav: GPU-assisted)\n"
	    "  --dump <dir>                     write each output of the replay as a file\n"
	    "  --timeout <ms>                   wait this long for the dispatch (default 5000)\n"
	    "  --repeat <n>                     run each dispatch n times, report differing outputs\n"
	    "Environment switches of the recompiler (KYTY_DEBUG_*) apply as in the emulator.\n"
	    "Exit code: 0 all outputs as live, 1 usage, 2 an output differs, 3 device lost/timeout.\n");
}

bool ParseArgs(int argc, char* argv[], Options& o) {
	if (argc < 2) return false;
	const fs::path root = argv[1];
	std::error_code error;
	if (fs::exists(root / "meta.json", error)) {
		o.snapshots.push_back(root);
	} else if (fs::is_directory(root, error)) {
		for (const auto& entry: fs::directory_iterator(root, error)) {
			if (entry.is_directory() && fs::exists(entry.path() / "meta.json", error)) {
				o.snapshots.push_back(entry.path());
			}
		}
		std::sort(o.snapshots.begin(), o.snapshots.end());
	}
	if (o.snapshots.empty()) {
		std::fprintf(stderr, "dispatch-replay: no snapshot (meta.json) in %s\n", root.string().c_str());
		return false;
	}
	for (int i = 2; i < argc; i++) {
		const std::string_view arg   = argv[i];
		const bool             value = i + 1 < argc;
		if (arg == "--spv" && value) {
			o.spv = argv[++i];
		} else if (arg == "--validate" && value) {
			o.validate = argv[++i];
		} else if (arg == "--dump" && value) {
			o.dump = argv[++i];
		} else if (arg == "--timeout" && value) {
			o.timeout_ms = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
		} else if (arg == "--repeat" && value) {
			o.repeat = std::max(1u, static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10)));
		} else {
			std::fprintf(stderr, "dispatch-replay: bad argument %s\n", argv[i]);
			return false;
		}
	}
	return true;
}

// Vulkan-Hpp without exceptions: a failed call ends the replay with its name.
template <typename T>
T Must(vk::ResultValue<T> result, const char* what) {
	if (result.result != vk::Result::eSuccess) {
		std::fprintf(stderr, "dispatch-replay: %s failed: %s\n", what, vk::to_string(result.result).c_str());
		std::exit(4);
	}
	return std::move(result.value);
}

void Must(vk::Result result, const char* what) {
	if (result != vk::Result::eSuccess) {
		std::fprintf(stderr, "dispatch-replay: %s failed: %s\n", what, vk::to_string(result).c_str());
		std::exit(4);
	}
}

std::vector<uint8_t> ReadFile(const fs::path& path) {
	std::ifstream in(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// ---- Device ----

std::vector<std::string> g_messages;
uint32_t                 g_errors = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL OnMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                         VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                         const VkDebugUtilsMessengerCallbackDataEXT* data,
                                         void* /*user*/) {
	if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
		g_errors += severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? 1u : 0u;
		if (g_messages.size() < 20) {
			g_messages.emplace_back(data->pMessage != nullptr ? data->pMessage : "");
		}
	}
	return VK_FALSE;
}

struct Device {
	vk::Instance                       instance;
	vk::PhysicalDevice                 physical;
	vk::Device                         device;
	vk::Queue                          queue;
	uint32_t                           family = 0;
	vk::PhysicalDeviceProperties       properties;
	vk::PhysicalDeviceMemoryProperties memory;
	vk::DebugUtilsMessengerEXT         messenger;
	uint32_t                           min_subgroup = 0, max_subgroup = 0;
	bool                               subgroup_control = false;
	bool                               hw_bounds        = false;
	bool                               native_float16   = false;
	uint32_t                           bindless_images   = 0; // per array
	uint32_t                           bindless_samplers = 0;
	vk::CommandPool                    pool;
};

PFN_vkGetInstanceProcAddr LoadVulkan() {
#if defined(_WIN32)
	HMODULE library = LoadLibraryA("vulkan-1.dll");
	return library == nullptr ? nullptr
	                          : reinterpret_cast<PFN_vkGetInstanceProcAddr>(
	                                GetProcAddress(library, "vkGetInstanceProcAddr"));
#else
	void* library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
	return library == nullptr ? nullptr
	                          : reinterpret_cast<PFN_vkGetInstanceProcAddr>(
	                                dlsym(library, "vkGetInstanceProcAddr"));
#endif
}

bool HasExtension(const std::vector<vk::ExtensionProperties>& list, const char* name) {
	return std::ranges::any_of(list, [&](const auto& e) { return std::strcmp(e.extensionName, name) == 0; });
}

bool CreateDevice(const Options& o, Device& out) {
	auto gipa = LoadVulkan();
	if (gipa == nullptr) {
		std::fprintf(stderr, "dispatch-replay: cannot load the Vulkan loader\n");
		return false;
	}
	VULKAN_HPP_DEFAULT_DISPATCHER.init(gipa);
	vk::ApplicationInfo app {};
	app.pApplicationName = "kyty dispatch replay";
	app.apiVersion       = VK_API_VERSION_1_3;
	std::vector<const char*> layers, extensions;
	std::vector<vk::ValidationFeatureEnableEXT> enables;
	vk::ValidationFeaturesEXT                   features {};
	if (!o.validate.empty()) {
		layers.push_back("VK_LAYER_KHRONOS_validation");
		extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
		if (o.validate == "sync") enables.push_back(vk::ValidationFeatureEnableEXT::eSynchronizationValidation);
		if (o.validate == "gpuav") enables.push_back(vk::ValidationFeatureEnableEXT::eGpuAssisted);
		features.enabledValidationFeatureCount = static_cast<uint32_t>(enables.size());
		features.pEnabledValidationFeatures    = enables.data();
	}
	vk::InstanceCreateInfo ici {};
	ici.pNext                   = o.validate.empty() ? nullptr : &features;
	ici.pApplicationInfo        = &app;
	ici.enabledLayerCount       = static_cast<uint32_t>(layers.size());
	ici.ppEnabledLayerNames     = layers.data();
	ici.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
	ici.ppEnabledExtensionNames = extensions.data();
	if (vk::createInstance(&ici, nullptr, &out.instance) != vk::Result::eSuccess) {
		std::fprintf(stderr, "dispatch-replay: vkCreateInstance failed%s\n",
		             o.validate.empty() ? "" : " (is the Vulkan SDK validation layer installed?)");
		return false;
	}
	VULKAN_HPP_DEFAULT_DISPATCHER.init(out.instance);
	if (!o.validate.empty()) {
		vk::DebugUtilsMessengerCreateInfoEXT mci {};
		mci.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
		                      vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
		mci.messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
		                  vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
		                  vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
		mci.pfnUserCallback = reinterpret_cast<vk::PFN_DebugUtilsMessengerCallbackEXT>(&OnMessage);
		(void)out.instance.createDebugUtilsMessengerEXT(&mci, nullptr, &out.messenger);
	}
	const auto devices = Must(out.instance.enumeratePhysicalDevices(), "vkEnumeratePhysicalDevices");
	if (devices.empty()) return false;
	out.physical = devices[0];
	for (auto p: devices) {
		if (p.getProperties().deviceType == vk::PhysicalDeviceType::eDiscreteGpu) out.physical = p;
	}
	const auto available =
	    Must(out.physical.enumerateDeviceExtensionProperties(), "vkEnumerateDeviceExtensionProperties");

	vk::PhysicalDeviceFloatControlsProperties         float_controls {};
	vk::PhysicalDeviceVulkan12Properties              props12 {};
	vk::PhysicalDeviceSubgroupSizeControlProperties   subgroup {};
	vk::PhysicalDeviceRobustness2PropertiesEXT        robust_props {};
	vk::PhysicalDeviceProperties2                     props2 {};
	props2.pNext       = &subgroup;
	subgroup.pNext     = &props12;
	props12.pNext      = &float_controls;
	float_controls.pNext = &robust_props;
	out.physical.getProperties2(&props2);
	out.properties   = props2.properties;
	out.memory       = out.physical.getMemoryProperties();
	out.min_subgroup = subgroup.minSubgroupSize;
	out.max_subgroup = subgroup.maxSubgroupSize;
	out.bindless_images =
	    std::min(16384u, props12.maxDescriptorSetUpdateAfterBindSampledImages /
	                         ShaderRecompiler::IR::BindlessImageArrayCount);
	out.bindless_samplers = std::min({4096u, props12.maxDescriptorSetUpdateAfterBindSamplers,
	                                  props12.maxPerStageDescriptorUpdateAfterBindSamplers});

	// Every supported feature on, with the extensions the emulator's compute shaders may need.
	std::vector<const char*> device_extensions;
	vk::PhysicalDeviceFeatures2 f2 {};
	vk::PhysicalDeviceVulkan11Features f11 {};
	vk::PhysicalDeviceVulkan12Features f12 {};
	vk::PhysicalDeviceVulkan13Features f13 {};
	vk::PhysicalDeviceRobustness2FeaturesEXT robust {};
	vk::PhysicalDeviceShaderImageAtomicInt64FeaturesEXT image_int64 {};
	vk::PhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR workgroup {};
	f2.pNext  = &f11;
	f11.pNext = &f12;
	f12.pNext = &f13;
	void** tail = &f13.pNext;
	const auto chain = [&](const char* name, auto& feature) {
		if (HasExtension(available, name)) {
			device_extensions.push_back(name);
			*tail = &feature;
			tail  = &feature.pNext;
		}
	};
	chain(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, robust);
	chain(VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME, image_int64);
	chain(VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME, workgroup);
	for (const char* name: {VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME,
	                        VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME}) {
		if (HasExtension(available, name)) device_extensions.push_back(name);
	}
	vk::PhysicalDeviceImageViewMinLodFeaturesEXT min_lod {};
	if (HasExtension(available, VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME)) {
		*tail = &min_lod;
		tail  = &min_lod.pNext;
	}
	out.physical.getFeatures2(&f2);
	out.subgroup_control = f13.subgroupSizeControl == VK_TRUE;
	out.hw_bounds = robust.robustBufferAccess2 == VK_TRUE && robust.nullDescriptor == VK_TRUE &&
	                robust_props.robustStorageBufferAccessSizeAlignment <= sizeof(uint32_t);
	out.native_float16 = f12.shaderFloat16 == VK_TRUE &&
	                     float_controls.shaderDenormPreserveFloat16 == VK_TRUE &&
	                     float_controls.shaderSignedZeroInfNanPreserveFloat16 == VK_TRUE;

	const auto families = out.physical.getQueueFamilyProperties();
	for (uint32_t i = 0; i < families.size(); i++) {
		if (families[i].queueFlags & vk::QueueFlagBits::eCompute) {
			out.family = i;
			break;
		}
	}
	float                     priority = 1.0f;
	vk::DeviceQueueCreateInfo qci {};
	qci.queueFamilyIndex = out.family;
	qci.queueCount       = 1;
	qci.pQueuePriorities = &priority;
	vk::DeviceCreateInfo      dci {};
	dci.pNext                   = &f2;
	dci.queueCreateInfoCount    = 1;
	dci.pQueueCreateInfos       = &qci;
	dci.enabledExtensionCount   = static_cast<uint32_t>(device_extensions.size());
	dci.ppEnabledExtensionNames = device_extensions.data();
	if (out.physical.createDevice(&dci, nullptr, &out.device) != vk::Result::eSuccess) {
		std::fprintf(stderr, "dispatch-replay: vkCreateDevice failed\n");
		return false;
	}
	VULKAN_HPP_DEFAULT_DISPATCHER.init(out.device);
	out.queue = out.device.getQueue(out.family, 0);
	vk::CommandPoolCreateInfo pci {};
	pci.flags            = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
	pci.queueFamilyIndex = out.family;
	out.pool             = Must(out.device.createCommandPool(pci), "vkCreateCommandPool");
	return true;
}

void DestroyDevice(Device& d) {
	if (d.device) {
		(void)d.device.waitIdle();
		d.device.destroyCommandPool(d.pool, nullptr);
		d.device.destroy(nullptr);
	}
	if (d.messenger) d.instance.destroyDebugUtilsMessengerEXT(d.messenger, nullptr);
	if (d.instance) d.instance.destroy(nullptr);
	d = Device {};
}

// ---- Resources ----

struct Allocation {
	vk::DeviceMemory memory;
	void*            mapped = nullptr;
};

class Resources {
public:
	explicit Resources(Device& d): m_d(d) {}
	~Resources() {
		auto device = m_d.device;
		if (!device) return;
		for (auto s: m_samplers) device.destroySampler(s, nullptr);
		for (auto v: m_views) device.destroyImageView(v, nullptr);
		for (auto i: m_images) device.destroyImage(i, nullptr);
		for (auto b: m_buffers) device.destroyBuffer(b, nullptr);
		for (auto m: m_memory) device.freeMemory(m, nullptr);
	}

	uint32_t MemoryType(uint32_t bits, vk::MemoryPropertyFlags flags) const {
		for (uint32_t i = 0; i < m_d.memory.memoryTypeCount; i++) {
			if ((bits & (1u << i)) != 0 && (m_d.memory.memoryTypes[i].propertyFlags & flags) == flags) {
				return i;
			}
		}
		return UINT32_MAX;
	}

	Allocation Allocate(const vk::MemoryRequirements& req, vk::MemoryPropertyFlags flags,
	                    bool device_address = false) {
		auto type = MemoryType(req.memoryTypeBits, flags);
		if (type == UINT32_MAX && (flags & vk::MemoryPropertyFlagBits::eDeviceLocal)) {
			type = MemoryType(req.memoryTypeBits, flags & ~vk::MemoryPropertyFlagBits::eDeviceLocal);
		}
		if (type == UINT32_MAX) return {};
		Allocation a;
		vk::MemoryAllocateInfo info {};
		info.allocationSize  = req.size;
		info.memoryTypeIndex = type;
		vk::MemoryAllocateFlagsInfo address_flags {};
		address_flags.flags = vk::MemoryAllocateFlagBits::eDeviceAddress;
		if (device_address) info.pNext = &address_flags;
		a.memory = Must(m_d.device.allocateMemory(info), "vkAllocateMemory");
		m_memory.push_back(a.memory);
		if (flags & vk::MemoryPropertyFlagBits::eHostVisible) {
			a.mapped = Must(m_d.device.mapMemory(a.memory, 0, VK_WHOLE_SIZE), "vkMapMemory");
		}
		return a;
	}

	// A host-visible buffer of `size` bytes, filled from `data` (zeros past it).
	std::pair<vk::Buffer, uint8_t*> HostBuffer(uint64_t size, std::span<const uint8_t> data = {},
	                                           bool device_address = false) {
		size = std::max<uint64_t>(size, 16);
		vk::BufferCreateInfo bci {};
		bci.size  = size;
		bci.usage = vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eUniformBuffer |
		            vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
		            vk::BufferUsageFlagBits::eIndirectBuffer;
		if (device_address) bci.usage |= vk::BufferUsageFlagBits::eShaderDeviceAddress;
		auto buffer = Must(m_d.device.createBuffer(bci), "vkCreateBuffer");
		m_buffers.push_back(buffer);
		auto alloc = Allocate(m_d.device.getBufferMemoryRequirements(buffer),
		                      vk::MemoryPropertyFlagBits::eHostVisible |
		                          vk::MemoryPropertyFlagBits::eHostCoherent |
		                          vk::MemoryPropertyFlagBits::eDeviceLocal,
		                      device_address);
		Must(m_d.device.bindBufferMemory(buffer, alloc.memory, 0), "vkBindBufferMemory");
		auto* bytes = static_cast<uint8_t*>(alloc.mapped);
		std::memset(bytes, 0, size);
		if (!data.empty()) std::memcpy(bytes, data.data(), std::min<uint64_t>(size, data.size()));
		return {buffer, bytes};
	}

	// A device-local buffer only the GPU fills (the BDA page table: 800 MB of mostly zeros).
	vk::Buffer DeviceBuffer(uint64_t size) {
		vk::BufferCreateInfo bci {};
		bci.size  = std::max<uint64_t>(size, 16);
		bci.usage = vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst;
		auto buffer = Must(m_d.device.createBuffer(bci), "vkCreateBuffer");
		m_buffers.push_back(buffer);
		auto alloc = Allocate(m_d.device.getBufferMemoryRequirements(buffer),
		                      vk::MemoryPropertyFlagBits::eDeviceLocal);
		Must(m_d.device.bindBufferMemory(buffer, alloc.memory, 0), "vkBindBufferMemory");
		return buffer;
	}

	vk::Image Image(const vk::ImageCreateInfo& ici) {
		vk::Image image;
		if (m_d.device.createImage(&ici, nullptr, &image) != vk::Result::eSuccess) return nullptr;
		m_images.push_back(image);
		auto alloc = Allocate(m_d.device.getImageMemoryRequirements(image),
		                      vk::MemoryPropertyFlagBits::eDeviceLocal);
		Must(m_d.device.bindImageMemory(image, alloc.memory, 0), "vkBindImageMemory");
		return image;
	}

	vk::ImageView View(const vk::ImageViewCreateInfo& create) {
		vk::ImageView view;
		if (m_d.device.createImageView(&create, nullptr, &view) != vk::Result::eSuccess) return nullptr;
		m_views.push_back(view);
		return view;
	}

	vk::Sampler Sampler(const vk::SamplerCreateInfo& create) {
		auto sampler = Must(m_d.device.createSampler(create), "vkCreateSampler");
		m_samplers.push_back(sampler);
		return sampler;
	}

private:
	Device&                       m_d;
	std::vector<vk::DeviceMemory> m_memory;
	std::vector<vk::Buffer>       m_buffers;
	std::vector<vk::Image>        m_images;
	std::vector<vk::ImageView>    m_views;
	std::vector<vk::Sampler>      m_samplers;
};

// ---- Shader ----

struct Shader {
	ShaderPrecompile::PermutationRecord      record;
	ShaderParams                             params;
	ShaderRecompiler::IR::CompiledShaderInfo program;
	std::vector<uint32_t>                    spirv;      // what runs
	uint64_t                                 recompiled_hash = 0;
	uint64_t                                 live_hash       = 0;
	std::string                              error;
};

bool CompileShader(const fs::path& folder, const Json& meta, const Options& o, Shader& out) {
	const auto bytes = ReadFile(folder / "record.bin");
	if (bytes.empty() || !ShaderPrecompile::Decode(bytes, out.record)) {
		out.error = "no readable record.bin";
		return false;
	}
	auto& record = out.record;
	auto& params = out.params;
	params.code            = record.code;
	params.back_code       = record.back_code;
	params.hash            = record.hash;
	params.user_data_count = record.user_data_count;
	ShaderRecompiler::CompileOptions options;
	options.stage           = record.stage;
	options.shader_hash     = record.hash;
	options.user_data       = std::span(params.user_data).first(params.user_data_count);
	options.back_code       = params.back_code;
	options.user_data_base  = record.user_data_base;
	options.wave_size       = record.wave_size;
	options.dump_ir         = false;
	options.early_dump      = false;
	options.dump_label      = "DispatchReplay";
	options.bindless_images = meta["globals"].value("bindless_images", false);
	auto* info = std::get_if<ShaderComputeInputInfo>(&record.info);
	if (info == nullptr) {
		out.error = "record is not a compute shader";
		return false;
	}
	options.input_info.compute = info;
	auto translated = ShaderRecompiler::TranslateProgram(params.code, options);
	if (translated.skip_dispatch || !translated.program.resource_tracking_complete ||
	    translated.program.info.buffers.size() != record.specialization.buffers.size() ||
	    translated.program.info.images.size() > record.specialization.images.size()) {
		out.error = "the translation no longer fits the record (skip/resources changed)";
		return false;
	}
	try {
		auto compiled = ShaderRecompiler::CompileProgram(std::move(translated), options,
		                                                 record.specialization,
		                                                 record.push_data_start_dword);
		out.spirv   = std::move(compiled.spirv);
		out.program = std::move(compiled.program).TakeCompiledInfo();
	} catch (const std::exception& error) {
		out.error = std::string("SPIR-V emit failed: ") + error.what();
		return false;
	}
	out.recompiled_hash = XXH3_64bits(out.spirv.data(), out.spirv.size() * 4u);
	const auto live     = ReadFile(folder / "live.spv");
	if (!live.empty()) out.live_hash = XXH3_64bits(live.data(), live.size());
	if (o.spv == "live" || (o.spv != "recompile")) {
		const auto file = o.spv == "live" ? live : ReadFile(o.spv);
		if (file.empty() || file.size() % 4 != 0) {
			out.error = "cannot read SPIR-V " + (o.spv == "live" ? (folder / "live.spv").string() : o.spv);
			return false;
		}
		out.spirv.assign(reinterpret_cast<const uint32_t*>(file.data()),
		                 reinterpret_cast<const uint32_t*>(file.data()) + file.size() / 4);
	}
	return true;
}

// ---- One replay ----

struct ImageEntry {
	vk::Image     image;
	vk::Format    format = vk::Format::eUndefined;
	vk::ImageView view;
	const Json*   json = nullptr;
};

std::vector<uint8_t> Blob(const fs::path& blobs, const Json& entry) {
	if (!entry.contains("blob")) return {};
	return ReadFile(blobs / (entry["blob"].get<std::string>() + ".bin"));
}

struct Result {
	bool        ran     = false;
	bool        lost    = false;
	bool        timeout = false;
	uint32_t    outputs = 0, same = 0, different = 0, unknown = 0;
	std::string notes;
};

void TransitionAll(vk::CommandBuffer cmd, vk::Image image, vk::ImageAspectFlags aspect,
                   vk::ImageLayout from, vk::ImageLayout to) {
	vk::ImageMemoryBarrier barrier {};
	barrier.srcAccessMask       = vk::AccessFlagBits::eMemoryWrite;
	barrier.dstAccessMask       = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	barrier.oldLayout           = from;
	barrier.newLayout           = to;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image               = image;
	barrier.subresourceRange    = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eAllCommands,
	                    {}, 0, nullptr, 0, nullptr, 1, &barrier);
}

vk::ImageAspectFlags FullAspect(vk::Format format) {
	switch (format) {
		case vk::Format::eD16Unorm:
		case vk::Format::eD32Sfloat:
		case vk::Format::eX8D24UnormPack32: return vk::ImageAspectFlagBits::eDepth;
		case vk::Format::eS8Uint: return vk::ImageAspectFlagBits::eStencil;
		case vk::Format::eD16UnormS8Uint:
		case vk::Format::eD24UnormS8Uint:
		case vk::Format::eD32SfloatS8Uint:
			return vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
		default: return vk::ImageAspectFlagBits::eColor;
	}
}

std::vector<vk::BufferImageCopy> Regions(const Json& entry) {
	std::vector<vk::BufferImageCopy> copies;
	if (!entry.contains("regions")) return copies;
	const auto aspect = static_cast<vk::ImageAspectFlags>(entry.value("aspect", 1u));
	for (const auto& r: entry["regions"]) {
		vk::BufferImageCopy copy {};
		copy.bufferOffset     = r["offset"].get<uint64_t>();
		copy.imageSubresource = {aspect, r["mip"].get<uint32_t>(), r["base_layer"].get<uint32_t>(),
		                         r["layers"].get<uint32_t>()};
		copy.imageExtent = vk::Extent3D {r["extent"][0].get<uint32_t>(), r["extent"][1].get<uint32_t>(),
		                                 r["extent"][2].get<uint32_t>()};
		copies.push_back(copy);
	}
	return copies;
}

Result ReplayOne(Device& d, const Options& o, const fs::path& folder, uint32_t repeat_index) {
	Result     result;
	const auto blobs = folder.parent_path() / "blobs";
	Json       meta;
	try {
		meta = Json::parse(ReadFile(folder / "meta.json"));
	} catch (const std::exception& error) {
		result.notes = std::string("bad meta.json: ") + error.what();
		return result;
	}
	const auto& globals = meta["globals"];
	ShaderRecompiler::SetHardwareStorageBufferBounds(globals.value("hw_bounds", d.hw_bounds));
	ShaderRecompiler::SetNativeFloat16(globals.value("native_fp16", d.native_float16));
	ShaderRecompiler::SetTableMode(globals.value("table_mode", false));
	if (globals.value("hw_bounds", false) && !d.hw_bounds) result.notes += " [device lacks hw bounds]";
	if (meta["flags"].value("uses_bindless", false))
		result.notes += " [bindless: heap entries are placeholders]";
	if (meta["flags"].value("table", false)) result.notes += " [table mode: addresses not rebased]";

	Shader shader;
	if (!CompileShader(folder, meta, o, shader)) {
		result.notes = shader.error + result.notes;
		return result;
	}
	if (repeat_index == 0 && shader.live_hash != 0) {
		result.notes += shader.recompiled_hash == shader.live_hash ? " [spirv as live]" : " [spirv CHANGED vs live]";
	}
	const auto& program = shader.program;
	if (program.bindings.push_data_start_dword != meta.value("push_data_start", program.bindings.push_data_start_dword) ||
	    program.bindings.ShaderDataDwords() != meta["shader_data"].size()) {
		result.notes += " [shader-data layout changed: push/shader data as recorded may not fit]";
	}

	Resources res(d);
	// The host memory regions the buffers view (several V#s may view one region), then each
	// buffer by resource index as a view of its region.
	std::map<uint32_t, std::pair<vk::Buffer, uint8_t*>> regions;
	for (const auto& entry: meta["buffer_regions"]) {
		const auto id   = entry["id"].get<uint32_t>();
		const auto data = Blob(blobs, entry);
		const auto size = entry.value("size", uint64_t {0});
		if (data.empty()) result.notes += " [buffer region " + std::to_string(id) + " not captured]";
		regions[id] = res.HostBuffer(std::max<uint64_t>(size, data.size()), data);
	}
	std::map<uint32_t, vk::DescriptorBufferInfo> buffers;
	for (const auto& entry: meta["buffers"]) {
		const auto index = entry["index"].get<uint32_t>();
		const auto found = entry.contains("region") ? regions.find(entry["region"].get<uint32_t>()) : regions.end();
		if (found == regions.end()) {
			if (entry.contains("skipped")) {
				result.notes += " [buffer " + std::to_string(index) + " " + entry["skipped"].get<std::string>() + "]";
			}
			buffers[index] = {nullptr, 0, VK_WHOLE_SIZE};
			continue;
		}
		buffers[index] = {found->second.first, entry["offset"].get<uint64_t>(), entry["size"].get<uint64_t>()};
	}
	const auto special = [&](const char* key, uint64_t fallback) -> std::pair<vk::Buffer, uint64_t> {
		const auto& entry = meta[key];
		const auto  data  = entry.is_object() ? Blob(blobs, entry) : std::vector<uint8_t> {};
		const auto  size  = entry.is_object() && entry.contains("size") ? entry["size"].get<uint64_t>()
		                                                                 : fallback;
		if (size == 0 && data.empty()) return {nullptr, 0};
		auto [buffer, mapped] = res.HostBuffer(size == VK_WHOLE_SIZE ? data.size() : size, data);
		(void)mapped;
		return {buffer, std::max<uint64_t>(size == VK_WHOLE_SIZE ? data.size() : size, 16)};
	};
	const auto flattened   = special("flattened_srt", 0);
	const auto shader_data = special("shader_data_buffer", 0);
	const auto gds         = special("gds", 0);
	const auto shared      = res.HostBuffer(std::max<uint64_t>(meta.value("shared_memory", uint64_t {0}), 16));
	// BDA: the page table and fault bitmap at their live sizes only for a shader that uses them.
	const bool     dma         = program.info.uses_dma;
	const uint64_t table_size  = dma ? std::max<uint64_t>(meta.value("bda_page_table", uint64_t {0}), 256) : 256;
	const uint64_t fault_size  = dma ? std::max<uint64_t>(meta.value("fault_buffer", uint64_t {0}), 256) : 256;
	const auto     page_table  = std::pair<vk::Buffer, uint8_t*> {res.DeviceBuffer(table_size), nullptr};
	const auto     faults      = res.HostBuffer(fault_size);

	vk::CommandBufferAllocateInfo cai {};
	cai.commandPool        = d.pool;
	cai.level              = vk::CommandBufferLevel::ePrimary;
	cai.commandBufferCount = 1;
	auto cmd = Must(d.device.allocateCommandBuffers(cai), "vkAllocateCommandBuffers")[0];
	vk::CommandBufferBeginInfo begin {};
	begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
	Must(cmd.begin(begin), "vkBeginCommandBuffer");

	// Images: one per resource index, from its first element; every element uploaded and viewed.
	std::map<std::pair<uint32_t, uint32_t>, ImageEntry> images;
	std::map<uint32_t, vk::Image>                       image_of;
	for (const auto& entry: meta["images"]) {
		const auto index   = entry["index"].get<uint32_t>();
		const auto element = entry["element"].get<uint32_t>();
		if (!entry.contains("image") || !entry.contains("view")) {
			result.notes += " [image " + std::to_string(index) + " not captured]";
			continue;
		}
		const auto& ji = entry["image"];
		const auto& jv = entry["view"];
		const auto  format = static_cast<vk::Format>(ji["format"].get<int>());
		if (!image_of.contains(index)) {
			vk::ImageCreateInfo ici {};
			ici.flags       = static_cast<vk::ImageCreateFlags>(ji["flags"].get<uint32_t>());
			ici.imageType   = static_cast<vk::ImageType>(ji["type"].get<int>());
			ici.format      = format;
			ici.extent      = vk::Extent3D {ji["extent"][0].get<uint32_t>(), ji["extent"][1].get<uint32_t>(),
			                                ji["extent"][2].get<uint32_t>()};
			ici.mipLevels   = ji["mips"].get<uint32_t>();
			ici.arrayLayers = ji["layers"].get<uint32_t>();
			ici.samples     = static_cast<vk::SampleCountFlagBits>(ji["samples"].get<uint32_t>());
			ici.tiling      = vk::ImageTiling::eOptimal;
			ici.usage       = static_cast<vk::ImageUsageFlags>(ji["usage"].get<uint32_t>()) |
			            vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc;
			ici.initialLayout = vk::ImageLayout::eUndefined;
			const auto image = res.Image(ici);
			if (image == nullptr) {
				result.notes += " [image " + std::to_string(index) + " create failed]";
				continue;
			}
			image_of[index] = image;
			TransitionAll(cmd, image, FullAspect(format), vk::ImageLayout::eUndefined,
			              vk::ImageLayout::eTransferDstOptimal);
		}
		const auto image = image_of[index];
		const auto data  = Blob(blobs, entry);
		const auto copies = Regions(entry);
		if (!data.empty() && !copies.empty()) {
			auto [staging, mapped] = res.HostBuffer(data.size(), data);
			(void)mapped;
			cmd.copyBufferToImage(staging, image, vk::ImageLayout::eTransferDstOptimal,
			                      static_cast<uint32_t>(copies.size()), copies.data());
		} else if (entry.contains("skipped")) {
			result.notes += " [image " + std::to_string(index) + " " + entry["skipped"].get<std::string>() + "]";
		}
		vk::ImageViewUsageCreateInfo usage {};
		const bool storage = entry.value("storage", false);
		usage.usage = storage ? vk::ImageUsageFlagBits::eStorage
		                      : static_cast<vk::ImageUsageFlags>(ji["usage"].get<uint32_t>()) &
		                            ~vk::ImageUsageFlagBits::eStorage;
		vk::ImageViewMinLodCreateInfoEXT min_lod {};
		if (jv.value("min_lod", 0u) != 0) {
			min_lod.minLod = static_cast<float>(jv["base_level"].get<uint32_t>()) +
			                 static_cast<float>(jv["min_lod"].get<uint32_t>()) / 256.0f;
			usage.pNext = &min_lod;
		}
		vk::ImageViewCreateInfo create {};
		create.pNext    = &usage;
		create.image    = image;
		create.viewType = static_cast<vk::ImageViewType>(jv["type"].get<int>());
		create.format   = static_cast<vk::Format>(jv["format"].get<int>());
		create.components = vk::ComponentMapping {
		    static_cast<vk::ComponentSwizzle>(jv["mapping"][0].get<int>()),
		    static_cast<vk::ComponentSwizzle>(jv["mapping"][1].get<int>()),
		    static_cast<vk::ComponentSwizzle>(jv["mapping"][2].get<int>()),
		    static_cast<vk::ComponentSwizzle>(jv["mapping"][3].get<int>())};
		create.subresourceRange = {static_cast<vk::ImageAspectFlags>(jv["aspect"].get<uint32_t>()),
		                           jv["base_level"].get<uint32_t>(), jv["level_count"].get<uint32_t>(),
		                           jv["base_layer"].get<uint32_t>(), jv["layer_count"].get<uint32_t>()};
		const auto view = res.View(create);
		if (view == nullptr) result.notes += " [image " + std::to_string(index) + " view failed]";
		images[{index, element}] = {image, format, view, &entry};
	}
	for (const auto& [index, image] : image_of) {
		vk::Format format = vk::Format::eUndefined;
		for (const auto& [key, entry]: images) {
			if (key.first == index) format = entry.format;
		}
		TransitionAll(cmd, image, FullAspect(format), vk::ImageLayout::eTransferDstOptimal,
		              vk::ImageLayout::eGeneral);
	}

	// The page table: zero, then each captured page's entry its copy's device address, and the
	// zero page's entry (the last) a zeroed page (bufferCache.cpp, spirvEmitterMemory.cpp).
	constexpr uint64_t PageBits = 14;
	constexpr uint64_t PageSize = uint64_t {1} << PageBits;
	constexpr uint64_t Lower    = uint64_t {1} << 40u;      // LOWER_ADDRESS_SIZE
	constexpr uint64_t Extended = 0x080000000000ull;         // kExtendedMemoryBase
	const auto page_index = [&](uint64_t address) {
		return (address >= Extended ? address - (Extended - Lower) : address) >> PageBits;
	};
	uint32_t bda_pages = 0;
	if (dma) {
		cmd.fillBuffer(page_table.first, 0, VK_WHOLE_SIZE, 0);
		vk::MemoryBarrier filled {};
		filled.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		filled.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
		cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer, {}, 1,
		                    &filled, 0, nullptr, 0, nullptr);
		uint64_t total = PageSize; // the zero page first
		for (const auto& run: meta["bda_pages"]) total += run.value("size", uint64_t {0});
		auto [pages, bytes] = res.HostBuffer(total, {}, true);
		vk::BufferDeviceAddressInfo address_info {};
		address_info.buffer = pages;
		const auto base     = d.device.getBufferAddress(address_info);
		const auto entry    = [&](uint64_t index, uint64_t value) {
            cmd.updateBuffer(page_table.first, index * 8u, 8u, &value);
		};
		entry(table_size / 8u - 1u, base);
		uint64_t offset = PageSize;
		for (const auto& run: meta["bda_pages"]) {
			const auto data    = Blob(blobs, run);
			const auto address = run["address"].get<uint64_t>();
			std::memcpy(bytes + offset, data.data(), data.size());
			for (uint64_t page = 0; page * PageSize < data.size(); page++) {
				entry(page_index(address + page * PageSize), base + offset + page * PageSize);
				bda_pages++;
			}
			offset += run.value("size", uint64_t {0});
		}
		result.notes += fmt::format(" [BDA: {} pages captured, {} seed pages unmapped live]", bda_pages,
		                            meta.value("bda_unmapped_pages", 0u));
	}

	// Samplers, made as SamplerCache makes them.
	std::map<uint32_t, vk::Sampler> samplers;
	for (const auto& entry: meta["samplers"]) {
		if (!entry.contains("words")) continue;
		ShaderSamplerResource r {};
		for (uint32_t i = 0; i < 4; i++) r.fields[i] = entry["words"][i].get<uint32_t>();
		vk::SamplerReductionModeCreateInfo reduction {};
		auto create = SamplerCache::MakeCreateInfo(r, entry.value("integer_border", false),
		                                           d.properties.limits.maxSamplerLodBias, reduction);
		samplers[entry["index"].get<uint32_t>()] = res.Sampler(create);
	}

	// Set 0 as CommitBindings writes it (descriptors.cpp append_writes).
	using Kind = ShaderRecompiler::IR::DescriptorBindingKind;
	std::vector<vk::DescriptorSetLayoutBinding> layout_bindings;
	std::vector<vk::WriteDescriptorSet>         writes;
	std::vector<vk::DescriptorBufferInfo>       buffer_infos;
	std::vector<vk::DescriptorImageInfo>        image_infos;
	size_t                                      total = 0;
	for (const auto& binding: program.bindings.descriptors) total += NativeDescriptorCount(binding);
	buffer_infos.reserve(total);
	image_infos.reserve(total);
	std::map<uint32_t, uint32_t> occurrences;
	std::map<vk::DescriptorType, uint32_t> pool_counts;
	for (const auto& binding: program.bindings.descriptors) {
		const auto type   = NativeDescriptorType(binding.kind);
		const auto count  = NativeDescriptorCount(binding);
		const auto number = ShaderRecompiler::IR::NativeBinding(program.stage, binding.kind);
		layout_bindings.push_back({number, type, count, vk::ShaderStageFlagBits::eCompute, nullptr});
		pool_counts[type] += count;
		vk::WriteDescriptorSet write {};
		write.dstBinding      = number;
		write.descriptorType  = type;
		write.descriptorCount = count;
		const auto buffer_start = buffer_infos.size();
		const auto image_start  = image_infos.size();
		if (ShaderRecompiler::IR::ImageBindingResourceClass(binding.kind) !=
		    ShaderRecompiler::IR::ImageResourceClass::None) {
			for (const auto resource: binding.resources) {
				const auto element = occurrences[resource]++;
				const auto found   = images.find({resource, element});
				vk::ImageView view = found != images.end() ? found->second.view : nullptr;
				if (view == nullptr) result.notes += " [image binding " + std::to_string(resource) + " null]";
				image_infos.push_back({nullptr, view, vk::ImageLayout::eGeneral});
			}
		} else {
			const auto add = [&](std::pair<vk::Buffer, uint64_t> b) {
				buffer_infos.push_back({b.first, 0, b.first ? b.second : VK_WHOLE_SIZE});
			};
			switch (binding.kind) {
				case Kind::Buffers:
					for (const auto resource: binding.resources) {
						const auto found = buffers.find(resource);
						buffer_infos.push_back(found != buffers.end() ? found->second
						                                              : vk::DescriptorBufferInfo {nullptr, 0, VK_WHOLE_SIZE});
					}
					break;
				case Kind::Samplers:
					for (const auto resource: binding.resources) {
						const auto found = samplers.find(resource);
						if (found == samplers.end()) result.notes += " [sampler missing]";
						image_infos.push_back({found != samplers.end() ? found->second : nullptr, nullptr,
						                       vk::ImageLayout::eUndefined});
					}
					break;
				case Kind::BdaPagetable: add({page_table.first, table_size}); break;
				case Kind::FaultBuffer: add({faults.first, fault_size}); break;
				case Kind::FlattenedSrt: add(flattened); break;
				case Kind::ShaderData: add(shader_data); break;
				case Kind::SharedMemory:
					add({shared.first, std::max<uint64_t>(meta.value("shared_memory", uint64_t {0}), 16)});
					break;
				case Kind::Gds: add(gds); break;
				default: break;
			}
		}
		if (buffer_infos.size() != buffer_start) write.pBufferInfo = buffer_infos.data() + buffer_start;
		if (image_infos.size() != image_start) write.pImageInfo = image_infos.data() + image_start;
		writes.push_back(write);
	}

	auto device = d.device;
	std::vector<vk::DescriptorSetLayout> set_layouts;
	const auto make_layout = [&](const vk::DescriptorSetLayoutCreateInfo& create) {
		return Must(device.createDescriptorSetLayout(create), "vkCreateDescriptorSetLayout");
	};
	{
		vk::DescriptorSetLayoutCreateInfo create {};
		create.bindingCount = static_cast<uint32_t>(layout_bindings.size());
		create.pBindings    = layout_bindings.data();
		set_layouts.push_back(make_layout(create));
	}
	const bool bindless = std::ranges::any_of(program.info.images, &ShaderRecompiler::IR::ImageResource::bindless) ||
	                      std::ranges::any_of(program.info.samplers, &ShaderRecompiler::IR::SamplerResource::bindless);
	vk::DescriptorPool bindless_pool;
	vk::DescriptorSet  bindless_set;
	if (bindless) {
		// As BindlessTable: every array's slot 0 a 1x1 placeholder, translation and feedback zero.
		set_layouts.push_back(make_layout({}));
		using namespace ShaderRecompiler::IR;
		const auto n = std::max(d.bindless_images, 1u);
		const auto s = std::max(d.bindless_samplers, 1u);
		const std::array<vk::DescriptorSetLayoutBinding, 11> bl {{
		    {BindlessImages2D, vk::DescriptorType::eSampledImage, n, vk::ShaderStageFlagBits::eAll},
		    {BindlessImages2DArray, vk::DescriptorType::eSampledImage, n, vk::ShaderStageFlagBits::eAll},
		    {BindlessImagesCube, vk::DescriptorType::eSampledImage, n, vk::ShaderStageFlagBits::eAll},
		    {BindlessImages3D, vk::DescriptorType::eSampledImage, n, vk::ShaderStageFlagBits::eAll},
		    {BindlessTranslation, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eAll},
		    {BindlessFeedback, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eAll},
		    {BindlessSamplers, vk::DescriptorType::eSampler, s, vk::ShaderStageFlagBits::eAll},
		    {BindlessImages2DUint, vk::DescriptorType::eSampledImage, n, vk::ShaderStageFlagBits::eAll},
		    {BindlessImages2DArrayUint, vk::DescriptorType::eSampledImage, n, vk::ShaderStageFlagBits::eAll},
		    {BindlessImages2DSint, vk::DescriptorType::eSampledImage, n, vk::ShaderStageFlagBits::eAll},
		    {BindlessImages2DArraySint, vk::DescriptorType::eSampledImage, n, vk::ShaderStageFlagBits::eAll},
		}};
		constexpr auto img = vk::DescriptorBindingFlagBits::ePartiallyBound |
		                     vk::DescriptorBindingFlagBits::eUpdateAfterBind |
		                     vk::DescriptorBindingFlagBits::eUpdateUnusedWhilePending;
		const std::array<vk::DescriptorBindingFlags, 11> flags {img, img, img, img, {}, {}, img, img, img, img, img};
		vk::DescriptorSetLayoutBindingFlagsCreateInfo fci {};
		fci.bindingCount  = static_cast<uint32_t>(flags.size());
		fci.pBindingFlags = flags.data();
		vk::DescriptorSetLayoutCreateInfo lci {};
		lci.pNext        = &fci;
		lci.flags        = vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool;
		lci.bindingCount = static_cast<uint32_t>(bl.size());
		lci.pBindings    = bl.data();
		set_layouts.push_back(make_layout(lci));
		const std::array<vk::DescriptorPoolSize, 3> sizes {{{vk::DescriptorType::eSampledImage, n * 8},
		                                                     {vk::DescriptorType::eStorageBuffer, 2},
		                                                     {vk::DescriptorType::eSampler, s}}};
		vk::DescriptorPoolCreateInfo bpi {};
		bpi.flags         = vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind;
		bpi.maxSets       = 1;
		bpi.poolSizeCount = static_cast<uint32_t>(sizes.size());
		bpi.pPoolSizes    = sizes.data();
		bindless_pool     = Must(device.createDescriptorPool(bpi), "vkCreateDescriptorPool");
		vk::DescriptorSetAllocateInfo bai {};
		bai.descriptorPool     = bindless_pool;
		bai.descriptorSetCount = 1;
		bai.pSetLayouts        = &set_layouts[2];
		bindless_set           = Must(device.allocateDescriptorSets(bai), "vkAllocateDescriptorSets")[0];
		const auto translation = res.HostBuffer(4u << 20u);
		const auto feedback    = res.HostBuffer(4u << 20u);
		const auto placeholder = [&](vk::ImageType type, vk::Format format, uint32_t layers,
		                             vk::ImageViewType view_type) {
			vk::ImageCreateInfo ici {};
			ici.flags = view_type == vk::ImageViewType::eCube ? vk::ImageCreateFlagBits::eCubeCompatible
			                                                  : vk::ImageCreateFlags {};
			ici.imageType = type;
			ici.format = format;
			ici.extent = vk::Extent3D {1, 1, 1};
			ici.mipLevels = 1;
			ici.arrayLayers = layers;
			ici.usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
			auto image = res.Image(ici);
			TransitionAll(cmd, image, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined,
			              vk::ImageLayout::eGeneral);
			vk::ImageViewCreateInfo create {};
			create.image            = image;
			create.viewType         = view_type;
			create.format           = format;
			create.subresourceRange = vk::ImageSubresourceRange {vk::ImageAspectFlagBits::eColor, 0, 1, 0, layers};
			return res.View(create);
		};
		struct Array { uint32_t binding; vk::ImageView view; };
		const std::array<Array, 8> arrays {{
		    {BindlessImages2D, placeholder(vk::ImageType::e2D, vk::Format::eR8G8B8A8Unorm, 1, vk::ImageViewType::e2D)},
		    {BindlessImages2DArray, placeholder(vk::ImageType::e2D, vk::Format::eR8G8B8A8Unorm, 1, vk::ImageViewType::e2DArray)},
		    {BindlessImagesCube, placeholder(vk::ImageType::e2D, vk::Format::eR8G8B8A8Unorm, 6, vk::ImageViewType::eCube)},
		    {BindlessImages3D, placeholder(vk::ImageType::e3D, vk::Format::eR8G8B8A8Unorm, 1, vk::ImageViewType::e3D)},
		    {BindlessImages2DUint, placeholder(vk::ImageType::e2D, vk::Format::eR32Uint, 1, vk::ImageViewType::e2D)},
		    {BindlessImages2DArrayUint, placeholder(vk::ImageType::e2D, vk::Format::eR32Uint, 1, vk::ImageViewType::e2DArray)},
		    {BindlessImages2DSint, placeholder(vk::ImageType::e2D, vk::Format::eR32Sint, 1, vk::ImageViewType::e2D)},
		    {BindlessImages2DArraySint, placeholder(vk::ImageType::e2D, vk::Format::eR32Sint, 1, vk::ImageViewType::e2DArray)},
		}};
		std::vector<vk::DescriptorImageInfo> infos(arrays.size() + 1);
		std::vector<vk::WriteDescriptorSet>  bw;
		const auto write = [&](uint32_t binding, vk::DescriptorType type, const vk::DescriptorImageInfo* image,
		                       const vk::DescriptorBufferInfo* buffer) {
			vk::WriteDescriptorSet w {};
			w.dstSet          = bindless_set;
			w.dstBinding      = binding;
			w.descriptorCount = 1;
			w.descriptorType  = type;
			w.pImageInfo      = image;
			w.pBufferInfo     = buffer;
			bw.push_back(w);
		};
		for (size_t i = 0; i < arrays.size(); i++) {
			infos[i] = {nullptr, arrays[i].view, vk::ImageLayout::eGeneral};
			write(arrays[i].binding, vk::DescriptorType::eSampledImage, &infos[i], nullptr);
		}
		ShaderSamplerResource default_sampler {};
		std::ranges::copy(BindlessDefaultSampler, default_sampler.fields);
		vk::SamplerReductionModeCreateInfo reduction {};
		infos.back() = {res.Sampler(SamplerCache::MakeCreateInfo(default_sampler, false,
		                                                         d.properties.limits.maxSamplerLodBias, reduction)),
		                nullptr, vk::ImageLayout::eUndefined};
		write(BindlessSamplers, vk::DescriptorType::eSampler, &infos.back(), nullptr);
		const std::array<vk::DescriptorBufferInfo, 2> bb {{{translation.first, 0, VK_WHOLE_SIZE},
		                                                   {feedback.first, 0, VK_WHOLE_SIZE}}};
		write(BindlessTranslation, vk::DescriptorType::eStorageBuffer, nullptr, &bb[0]);
		write(BindlessFeedback, vk::DescriptorType::eStorageBuffer, nullptr, &bb[1]);
		device.updateDescriptorSets(bw, {});
	}
	const vk::PushConstantRange push_range {vk::ShaderStageFlagBits::eCompute, 0,
	                                        ShaderRecompiler::IR::NativePushConstantSize};
	vk::PipelineLayoutCreateInfo plci {};
	plci.setLayoutCount         = static_cast<uint32_t>(set_layouts.size());
	plci.pSetLayouts            = set_layouts.data();
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges    = &push_range;
	const auto pipeline_layout  = Must(device.createPipelineLayout(plci), "vkCreatePipelineLayout");

	std::vector<vk::DescriptorPoolSize> pool_sizes;
	for (const auto& [type, count]: pool_counts) pool_sizes.push_back({type, count});
	vk::DescriptorPool pool;
	vk::DescriptorSet  set;
	if (!pool_sizes.empty()) {
		vk::DescriptorPoolCreateInfo dpi {};
		dpi.maxSets       = 1;
		dpi.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
		dpi.pPoolSizes    = pool_sizes.data();
		pool              = Must(device.createDescriptorPool(dpi), "vkCreateDescriptorPool");
		vk::DescriptorSetAllocateInfo dai {};
		dai.descriptorPool     = pool;
		dai.descriptorSetCount = 1;
		dai.pSetLayouts        = &set_layouts[0];
		set                    = Must(device.allocateDescriptorSets(dai), "vkAllocateDescriptorSets")[0];
		for (auto& write: writes) write.dstSet = set;
		device.updateDescriptorSets(writes, {});
	}

	// Pipeline, as PrepareComputePipeline (shaders.cpp).
	vk::ShaderModuleCreateInfo smci {};
	smci.codeSize     = shader.spirv.size() * 4u;
	smci.pCode        = shader.spirv.data();
	const auto module = Must(device.createShaderModule(smci), "vkCreateShaderModule");
	vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup {};
	vk::ComputePipelineCreateInfo                         cpi {};
	cpi.stage.stage  = vk::ShaderStageFlagBits::eCompute;
	cpi.stage.module = module;
	cpi.stage.pName  = "main";
	if (d.subgroup_control && program.wave_size >= d.min_subgroup && program.wave_size <= d.max_subgroup) {
		subgroup.requiredSubgroupSize = program.wave_size;
		cpi.stage.pNext               = &subgroup;
	}
	cpi.layout            = pipeline_layout;
	cpi.basePipelineIndex = -1;
	vk::Pipeline pipeline;
	const auto   compile_start = std::chrono::steady_clock::now();
	const auto   created       = device.createComputePipelines(nullptr, 1, &cpi, nullptr, &pipeline);
	const auto   compile_ms =
	    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - compile_start).count();
	if (created != vk::Result::eSuccess) {
		result.notes = "vkCreateComputePipelines failed: " + vk::to_string(created) + result.notes;
		return result;
	}

	// Push data: the shader data at its start dword (as CommitBindings).
	ShaderRecompiler::IR::PushData push {};
	if (program.bindings.UsesPushData()) {
		const auto start = program.bindings.push_data_start_dword;
		uint32_t   i     = 0;
		for (const auto& word: meta["shader_data"]) {
			if (start + i >= push.dwords.size()) break;
			push.dwords[start + i++] = word.get<uint32_t>();
		}
	}
	std::array<uint32_t, 3> groups {meta["groups"][0].get<uint32_t>(), meta["groups"][1].get<uint32_t>(),
	                                meta["groups"][2].get<uint32_t>()};
	if (meta.value("indirect", false)) {
		const auto args = Blob(blobs, meta["args"]);
		if (args.size() >= 12) std::memcpy(groups.data(), args.data(), 12);
	}

	vk::MemoryBarrier upload {};
	upload.srcAccessMask = vk::AccessFlagBits::eMemoryWrite | vk::AccessFlagBits::eHostWrite;
	upload.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands | vk::PipelineStageFlagBits::eHost,
	                    vk::PipelineStageFlagBits::eComputeShader, {}, 1, &upload, 0, nullptr, 0, nullptr);
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
	if (set) cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, pipeline_layout, 0, 1, &set, 0, nullptr);
	if (bindless) {
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, pipeline_layout,
		                       ShaderRecompiler::IR::BindlessDescriptorSet, 1, &bindless_set, 0, nullptr);
	}
	cmd.pushConstants(pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), push.dwords.data());
	cmd.dispatch(groups[0], groups[1], groups[2]);
	vk::MemoryBarrier done {};
	done.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
	done.dstAccessMask = vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eHostRead;
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
	                    vk::PipelineStageFlagBits::eTransfer | vk::PipelineStageFlagBits::eHost, {}, 1,
	                    &done, 0, nullptr, 0, nullptr);

	// Outputs: written buffers are read in place; storage images copied out like the capture did.
	struct Output {
		std::string          name;
		const Json*          json  = nullptr;
		const uint8_t*       bytes = nullptr;
		uint64_t             size  = 0;
	};
	std::vector<Output> outputs;
	const auto& jout = meta["outputs"];
	if (jout.is_object()) {
		for (const auto& entry: jout["regions"]) {
			const auto id    = entry["id"].get<uint32_t>();
			const auto found = regions.find(id);
			outputs.push_back({"region " + std::to_string(id), &entry,
			                   found != regions.end() ? found->second.second : nullptr,
			                   entry.value("size", uint64_t {0})});
		}
		for (const auto& entry: jout["images"]) {
			const auto index   = entry["index"].get<uint32_t>();
			const auto element = entry["element"].get<uint32_t>();
			const auto found   = images.find({index, element});
			const auto copies  = Regions(entry);
			const auto size    = entry.value("size", uint64_t {0});
			Output     output {"image " + std::to_string(index) + "." + std::to_string(element), &entry};
			if (found != images.end() && !copies.empty() && size != 0) {
				auto [readback, mapped] = res.HostBuffer(size);
				cmd.copyImageToBuffer(found->second.image, vk::ImageLayout::eGeneral, readback,
				                      static_cast<uint32_t>(copies.size()), copies.data());
				output.bytes = mapped;
				output.size  = size;
			}
			outputs.push_back(output);
		}
		vk::MemoryBarrier host {};
		host.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		host.dstAccessMask = vk::AccessFlagBits::eHostRead;
		cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost, {}, 1,
		                    &host, 0, nullptr, 0, nullptr);
	} else {
		result.notes += " [no live outputs: the game ended before this dispatch completed]";
	}
	Must(cmd.end(), "vkEndCommandBuffer");

	const auto fence = Must(device.createFence({}), "vkCreateFence");
	vk::SubmitInfo submit {};
	submit.commandBufferCount = 1;
	submit.pCommandBuffers    = &cmd;
	const auto start          = std::chrono::steady_clock::now();
	auto       submitted      = d.queue.submit(1, &submit, fence);
	vk::Result waited         = submitted;
	if (submitted == vk::Result::eSuccess) {
		waited = device.waitForFences(1, &fence, VK_TRUE, uint64_t {o.timeout_ms} * 1000000ull);
	}
	const auto gpu_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	result.ran = true;
	if (waited == vk::Result::eErrorDeviceLost) {
		result.lost = true;
	} else if (waited == vk::Result::eTimeout) {
		result.timeout = true;
	}
	if (!result.lost && !result.timeout) {
		for (const auto& output: outputs) {
			result.outputs++;
			const auto live = output.json != nullptr ? Blob(blobs, *output.json) : std::vector<uint8_t> {};
			if (output.bytes == nullptr || live.empty()) {
				result.unknown++;
				continue;
			}
			const auto size = std::min<uint64_t>(output.size, live.size());
			uint64_t   first = UINT64_MAX, differing = 0;
			for (uint64_t i = 0; i < size; i++) {
				if (output.bytes[i] != live[i]) {
					differing++;
					first = std::min(first, i);
				}
			}
			if (differing == 0 && output.size == live.size()) {
				result.same++;
			} else {
				result.different++;
				result.notes += fmt::format(" [{} differs: {} of {} bytes, first at 0x{:x}]", output.name,
				                            differing, size, first == UINT64_MAX ? size : first);
			}
			if (!o.dump.empty()) {
				std::error_code error;
				fs::create_directories(o.dump, error);
				auto name = folder.filename().string() + "_" + output.name;
				std::replace(name.begin(), name.end(), ' ', '_');
				std::ofstream(o.dump / (name + ".bin"), std::ios::binary)
				    .write(reinterpret_cast<const char*>(output.bytes), static_cast<std::streamsize>(output.size));
			}
		}
	}
	if (dma && !result.lost && !result.timeout) {
		// Pages the shader read that the snapshot lacks (or that were unmapped live as well).
		const auto* words = reinterpret_cast<const uint32_t*>(faults.second);
		std::vector<uint64_t> missing;
		for (uint64_t w = 0; w < fault_size / 4u; w++) {
			for (uint32_t bit = 0; words[w] != 0 && bit < 32; bit++) {
				if ((words[w] >> bit) & 1u) {
					const auto packed = (w * 32u + bit) << PageBits;
					missing.push_back(packed >= Lower ? packed + (Extended - Lower) : packed);
				}
			}
		}
		if (!missing.empty()) {
			std::string list;
			for (size_t i = 0; i < missing.size() && i < 12; i++) list += fmt::format(" 0x{:x}", missing[i]);
			result.notes += fmt::format(" [BDA: read {} pages not in the snapshot:{}{}; add them with "
			                            "KYTY_DISPATCH_SNAPSHOT_PAGES]",
			                            missing.size(), list, missing.size() > 12 ? " ..." : "");
		}
	}
	result.notes = fmt::format("pipeline {:.1f} ms, gpu {:.1f} ms", compile_ms, gpu_ms) + result.notes;
	if (!result.lost) {
		(void)device.waitIdle();
		device.destroyFence(fence, nullptr);
		device.destroyPipeline(pipeline, nullptr);
		device.destroyShaderModule(module, nullptr);
		device.destroyPipelineLayout(pipeline_layout, nullptr);
		for (auto layout: set_layouts) device.destroyDescriptorSetLayout(layout, nullptr);
		if (pool) device.destroyDescriptorPool(pool, nullptr);
		if (bindless_pool) device.destroyDescriptorPool(bindless_pool, nullptr);
		device.freeCommandBuffers(d.pool, 1, &cmd);
	}
	return result;
}

} // namespace

int Run(int argc, char* argv[]) {
	Options o;
	if (!ParseArgs(argc, argv, o)) {
		PrintUsage();
		return 1;
	}
	static Common::Subsystems subsystems;
	Common::InitializeThreads();
	subsystems.Initialize<Config::Lifecycle>();
	Config::ConfigOptions config;
	config.printf_direction = Config::LogDirection::Silent;
	Config::Load(config);
	subsystems.Initialize<Log::Lifecycle>();
	ShaderInit();

	Device device;
	if (!CreateDevice(o, device)) return 1;
	std::printf("device: %s, hw bounds %s, native fp16 %s, validation %s\n",
	            device.properties.deviceName.data(), device.hw_bounds ? "on" : "off",
	            device.native_float16 ? "on" : "off", o.validate.empty() ? "off" : o.validate.c_str());
	int exit_code = 0;
	for (const auto& folder: o.snapshots) {
		for (uint32_t pass = 0; pass < o.repeat; pass++) {
			g_errors = 0;
			g_messages.clear();
			const auto  result = ReplayOne(device, o, folder, pass);
			const char* verdict = !result.ran       ? "NICHT GELAUFEN"
			                      : result.lost     ? "ABSTURZ (device lost)"
			                      : result.timeout  ? "HAENGT (timeout)"
			                      : result.different != 0 ? "ANDERS als live"
			                      : result.outputs == result.same && result.outputs != 0 ? "wie live"
			                                                                             : "ohne Vergleich";
			std::printf("%s%s: %s, outputs %u (gleich %u, anders %u, ohne live %u), validation %u; %s\n",
			            folder.filename().string().c_str(),
			            o.repeat > 1 ? fmt::format(" #{}", pass).c_str() : "", verdict, result.outputs,
			            result.same, result.different, result.unknown, g_errors, result.notes.c_str());
			for (const auto& message: g_messages) std::printf("  validation: %s\n", message.c_str());
			std::fflush(stdout);
			if (result.lost || result.timeout) {
				exit_code = 3;
				// The device is gone: start a new one for the next snapshot.
				DestroyDevice(device);
				if (!CreateDevice(o, device)) return exit_code;
				break;
			}
			if (result.different != 0 && exit_code == 0) exit_code = 2;
		}
	}
	DestroyDevice(device);
	return exit_code;
}

} // namespace Libs::Graphics::DispatchReplay
