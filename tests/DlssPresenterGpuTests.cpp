// Synthetic, bounded Vulkan/NGX integration fixture. No guest data or game heuristics.
#define VK_NO_PROTOTYPES
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winver.h>
#include "graphics/presentation/dlss/dlssVulkan.h"
#include "graphics/presentation/dlss/dlssPresentation.h"
#include "graphics/presentation/presenter.h"
#include "graphics/presentation/window/windowInternal.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/commandHooks.h"
#include "graphics/host_gpu/renderer/dlssBoundConstants.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/shader/shader.h"
#include "emulator.h"
#include "common/subsystems.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "kernel/memory.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <renderdoc_app.h>
#include <memory>
#if !defined(KYTY_DLSS_PRESENTER_GPU_TEST_ACCESS)
#error This native fixture requires KYTY_DLSS_PRESENTER_GPU_TEST_ACCESS on every full target source.
#endif

#include <algorithm>
#include <atomic>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Libs::Graphics;
namespace {
void Require(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "PRESENTER GPU FAIL: %s\n", message); std::fflush(nullptr); std::_Exit(2); }
}
void Check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "VULKAN SETUP/EXECUTION ERROR: %s result=%d\n", operation, int(result));
        std::fflush(nullptr); std::_Exit(2);
    }
}
void CheckDlss(DlssStatus result, const char* operation) {
    std::printf("dlss-call: %s status=%d native=0x%08x message=%s\n", operation,
                int(result.code), result.native_result, result.message ? result.message : "");
    Require(bool(result), operation);
}
void LogLoadedRuntime(const std::filesystem::path& expected) {
    const HMODULE module = GetModuleHandleW(L"nvngx_dlss.dll");
    Require(module != nullptr, "actual loaded nvngx_dlss.dll module could not be identified");
    std::vector<wchar_t> path_buffer(32768);
    const DWORD length = GetModuleFileNameW(module, path_buffer.data(), DWORD(path_buffer.size()));
    Require(length > 0 && length < path_buffer.size(), "actual NGX runtime path unavailable/truncated");
    const std::filesystem::path actual(std::wstring(path_buffer.data(), length));
    std::printf("NGX RUNTIME LOADED: %ls\nNGX RUNTIME EXPECTED: %ls\n", actual.c_str(), expected.c_str());
    // Runtime may be deployed beside the test executable. Verify bytes, not only path/version.
    Require(std::filesystem::file_size(actual) == std::filesystem::file_size(expected),
            "loaded DLSS runtime size differs from verified runtime");
    std::ifstream first(actual, std::ios::binary), second(expected, std::ios::binary);
    Require(first.is_open() && second.is_open(), "cannot inspect loaded DLSS runtime bytes");
    std::array<char, 65536> a{}, b{};
    while (first) {
        first.read(a.data(), a.size()); second.read(b.data(), b.size());
        Require(first.gcount() == second.gcount() &&
                std::memcmp(a.data(), b.data(), size_t(first.gcount())) == 0,
                "loaded DLSS runtime differs from verified runtime bytes");
    }
    Require(first.eof() && second.eof(), "runtime provenance file read failed");
    const HMODULE version = LoadLibraryExW(L"version.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    Require(version != nullptr, "system version resource API unavailable");
    const auto get_size = reinterpret_cast<DWORD(WINAPI*)(LPCWSTR, LPDWORD)>(
        GetProcAddress(version, "GetFileVersionInfoSizeW"));
    const auto get_info = reinterpret_cast<BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID)>(
        GetProcAddress(version, "GetFileVersionInfoW"));
    const auto query = reinterpret_cast<BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT)>(
        GetProcAddress(version, "VerQueryValueW"));
    Require(get_size && get_info && query, "version resource entry points unavailable");
    DWORD unused = 0; const DWORD bytes = get_size(actual.c_str(), &unused);
    Require(bytes > 0, "loaded DLSS runtime version resource unavailable");
    std::vector<unsigned char> resource(bytes);
    Require(get_info(actual.c_str(), 0, bytes, resource.data()) != FALSE, "loaded runtime version read failed");
    VS_FIXEDFILEINFO* info = nullptr; UINT info_size = 0;
    Require(query(resource.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &info_size) != FALSE &&
            info != nullptr && info_size >= sizeof(*info) && info->dwSignature == 0xfeef04bdu,
            "loaded runtime fixed version resource invalid");
    std::printf("NGX RUNTIME VERSION: %u.%u.%u.%u exact_expected_bytes=1\n",
        unsigned(HIWORD(info->dwFileVersionMS)), unsigned(LOWORD(info->dwFileVersionMS)),
        unsigned(HIWORD(info->dwFileVersionLS)), unsigned(LOWORD(info->dwFileVersionLS)));
    FreeLibrary(version);
}
uint16_t Half(float value) {
    const uint32_t bits = std::bit_cast<uint32_t>(value);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const int exponent = int((bits >> 23) & 255u) - 127 + 15;
    Require(exponent > 0 && exponent < 31, "synthetic half input outside normal finite range");
    return uint16_t(sign | (uint32_t(exponent) << 10) | ((bits >> 13) & 1023u));
}
float Unhalf(uint16_t value) {
    const uint32_t sign = uint32_t(value & 0x8000u) << 16;
    uint32_t exponent = (value >> 10) & 31u;
    uint32_t mantissa = value & 1023u;
    if (!exponent) {
        if (!mantissa) return std::bit_cast<float>(sign);
        int shift = 0;
        while (!(mantissa & 1024u)) { mantissa <<= 1; ++shift; }
        return std::bit_cast<float>(sign | (uint32_t(127 - 14 - shift) << 23) |
                                    ((mantissa & 1023u) << 13));
    }
    return std::bit_cast<float>(sign | ((exponent == 31u ? 255u : exponent + 112u) << 23) |
                                (mantissa << 13));
}

struct Buffer { VkBuffer handle{}; VkDeviceMemory memory{}; VkDeviceSize bytes{}; void* mapped{}; };
struct Image { VkImage handle{}; VkImageView view{}; VkDeviceMemory memory{};
               VkFormat format{}; DlssExtent size{}; VkImageUsageFlags usage{};
               VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED; };
constexpr size_t ReadbackGuardBytes = 64;
void VerifyGuard(const Buffer& buffer, size_t payload) {
    Require(payload + ReadbackGuardBytes <= buffer.bytes, "readback guard exceeds staging allocation");
    const auto* data = static_cast<const unsigned char*>(buffer.mapped);
    for (size_t i = 0; i < ReadbackGuardBytes; ++i)
        Require(data[payload + i] == 0xa5u, "native image copy changed trailing readback guard");
}

#define GLOBAL_FUNCS(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties) \
    X(vkEnumerateInstanceLayerProperties)
#define INSTANCE_FUNCS(X) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkEnumerateDeviceExtensionProperties) X(vkGetPhysicalDeviceFormatProperties) X(vkCreateDevice)
#define DEVICE_FUNCS(X) X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandBuffer) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCreateFence) X(vkDestroyFence) \
    X(vkQueueSubmit) X(vkWaitForFences) X(vkCreateBuffer) X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) \
    X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkCreateImage) \
    X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCmdPipelineBarrier) \
    X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdClearColorImage)

struct Vulkan {
    HMODULE library{}; PFN_vkGetInstanceProcAddr gipa{}; PFN_vkGetDeviceProcAddr gdpa{};
    VkInstance instance{}; VkPhysicalDevice physical{}; VkDevice device{}; VkQueue queue{};
    VkCommandPool pool{}; VkCommandBuffer command{}; uint32_t family{};
    VkPhysicalDeviceMemoryProperties memory{};
    std::vector<std::string> instance_extensions, device_extensions;
    std::vector<Buffer> buffers; std::vector<Image> images;
    DlssStatus instance_query{}, device_query{};
    bool validation_enabled = false;
    VkImageLayout input_layout = VK_IMAGE_LAYOUT_GENERAL;
    std::atomic<uint32_t> validation_errors{0};
    VkDebugUtilsMessengerEXT debug_messenger{};
    PFN_vkCreateDebugUtilsMessengerEXT create_debug_messenger{};
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug_messenger{};
#define DECLARE(name) PFN_##name name{};
    GLOBAL_FUNCS(DECLARE) INSTANCE_FUNCS(DECLARE) DEVICE_FUNCS(DECLARE)
#undef DECLARE
    ~Vulkan() { Cleanup(); }
    static VKAPI_ATTR VkBool32 VKAPI_CALL DebugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void* context) {
        auto& self = *static_cast<Vulkan*>(context);
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
            self.validation_errors.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr, "VULKAN VALIDATION %s: %s\n",
            severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "ERROR" : "WARNING",
            data && data->pMessage ? data->pMessage : "(no diagnostic text)");
        return VK_FALSE;
    }
    void Cleanup() {
        // Every successful Submit waits a bounded fence. On an unsignaled fence the process
        // exits immediately instead of running an unbounded driver wait during destruction.
        if (device) {
            for (auto& image : images) {
                if (image.view) vkDestroyImageView(device, image.view, nullptr);
                if (image.handle) vkDestroyImage(device, image.handle, nullptr);
                if (image.memory) vkFreeMemory(device, image.memory, nullptr);
            }
            for (auto& buffer : buffers) {
                if (buffer.mapped) vkUnmapMemory(device, buffer.memory);
                if (buffer.handle) vkDestroyBuffer(device, buffer.handle, nullptr);
                if (buffer.memory) vkFreeMemory(device, buffer.memory, nullptr);
            }
            if (pool) vkDestroyCommandPool(device, pool, nullptr);
            vkDestroyDevice(device, nullptr);
            device = VK_NULL_HANDLE; pool = VK_NULL_HANDLE;
            images.clear(); buffers.clear();
        }
        if (debug_messenger && destroy_debug_messenger) {
            destroy_debug_messenger(instance, debug_messenger, nullptr);
            debug_messenger = VK_NULL_HANDLE;
        }
        if (instance && vkDestroyInstance) vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
        if (library) FreeLibrary(library);
        library = nullptr;
    }
    void ValidateAndCleanup() {
        // Check after native object destruction too, so leaked/in-use object errors fail.
        Cleanup();
        if (validation_enabled) {
            const auto errors = validation_errors.load(std::memory_order_relaxed);
            std::printf("VULKAN VALIDATION RESULT: errors=%u\n", errors);
            Require(errors == 0, "Vulkan validation reported errors");
        }
    }
    static std::vector<const char*> Pointers(const std::vector<std::string>& strings) {
        std::vector<const char*> pointers;
        for (const auto& value : strings) pointers.push_back(value.c_str());
        return pointers;
    }
    void Initialize(const DlssProjectInfo& project) {
        char input_layout_text[32]{};
        if (GetEnvironmentVariableA("KYTY_DLSS_SMOKE_INPUT_LAYOUT", input_layout_text,
                static_cast<DWORD>(sizeof(input_layout_text))) > 0) {
            Require(std::strcmp(input_layout_text, "general") == 0 ||
                    std::strcmp(input_layout_text, "shader-read-only") == 0,
                    "unknown requested DLSS smoke input layout");
            if (std::strcmp(input_layout_text, "shader-read-only") == 0)
                input_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        std::printf("NGX INPUT LAYOUT: %s\n", input_layout == VK_IMAGE_LAYOUT_GENERAL ?
                    "GENERAL" : "SHADER_READ_ONLY_OPTIMAL");
        char validation[16]{};
        validation_enabled = GetEnvironmentVariableA("KYTY_DLSS_SMOKE_VALIDATION", validation,
            static_cast<DWORD>(sizeof(validation))) > 0 && std::strcmp(validation, "0") != 0;
        library = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        Require(library != nullptr, "system Vulkan loader unavailable; not a feature regression");
        gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(library, "vkGetInstanceProcAddr"));
        Require(gipa != nullptr, "vkGetInstanceProcAddr missing");
#define GLOBAL(name) name = reinterpret_cast<PFN_##name>(gipa(nullptr, #name)); Require(name != nullptr, #name);
        GLOBAL_FUNCS(GLOBAL)
#undef GLOBAL
        instance_query = DlssVulkan::QueryInstanceExtensions(project, instance_extensions);
        std::printf("NGX REQUIRED INSTANCE EXTENSIONS: count=%zu\n", instance_extensions.size());
        for (const auto& extension : instance_extensions) std::printf("  NGX instance: %s\n", extension.c_str());
        const char* validation_layer = "VK_LAYER_KHRONOS_validation";
        if (validation_enabled) {
            uint32_t layer_count = 0;
            Check(vkEnumerateInstanceLayerProperties(&layer_count, nullptr), "validation layer count");
            std::vector<VkLayerProperties> layers(layer_count);
            Check(vkEnumerateInstanceLayerProperties(&layer_count, layers.data()), "validation layers");
            Require(std::any_of(layers.begin(), layers.end(), [&](const auto& layer) {
                return std::strcmp(layer.layerName, validation_layer) == 0;
            }), "requested Vulkan validation layer unavailable; explicit setup failure");
            if (std::find(instance_extensions.begin(), instance_extensions.end(),
                          VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == instance_extensions.end())
                instance_extensions.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
        // Keep the native resource control executable against the truthful unsupported stub.
        // A failed query is asserted AFTER the exact upload/readback control has passed.
        uint32_t count = 0;
        Check(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr), "instance extension count");
        std::vector<VkExtensionProperties> available(count);
        Check(vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data()), "instance extensions");
        for (const auto& required : instance_extensions)
            Require(std::any_of(available.begin(), available.end(), [&](const auto& ext) {
                return required == ext.extensionName; }), "required instance extension unsupported");
        auto names = Pointers(instance_extensions);
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "KytyPS5 synthetic DLSS Vulkan smoke";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create.pApplicationInfo = &app;
        create.enabledExtensionCount = uint32_t(names.size()); create.ppEnabledExtensionNames = names.data();
        VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        if (validation_enabled) {
            debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
            debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debug.pfnUserCallback = DebugMessage; debug.pUserData = this;
            create.pNext = &debug;
            create.enabledLayerCount = 1; create.ppEnabledLayerNames = &validation_layer;
        }
        Check(vkCreateInstance(&create, nullptr, &instance), "create instance");
        if (validation_enabled) {
            create_debug_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                gipa(instance, "vkCreateDebugUtilsMessengerEXT"));
            destroy_debug_messenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                gipa(instance, "vkDestroyDebugUtilsMessengerEXT"));
            Require(create_debug_messenger && destroy_debug_messenger,
                    "requested Vulkan validation debug entry points unavailable");
            Check(create_debug_messenger(instance, &debug, nullptr, &debug_messenger),
                  "create validation debug messenger");
            std::puts("VULKAN VALIDATION ENABLED: VK_LAYER_KHRONOS_validation, all errors are fatal to the test");
        }
#define INSTANCE(name) name = reinterpret_cast<PFN_##name>(gipa(instance, #name)); Require(name != nullptr, #name);
        INSTANCE_FUNCS(INSTANCE)
#undef INSTANCE
        gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance, "vkGetDeviceProcAddr"));
        Require(gdpa != nullptr, "vkGetDeviceProcAddr missing");
        Check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "physical count");
        std::vector<VkPhysicalDevice> adapters(count);
        Check(vkEnumeratePhysicalDevices(instance, &count, adapters.data()), "physical adapters");
        for (auto candidate : adapters) {
            VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(candidate, &properties);
            if (properties.vendorID == 0x10de && properties.apiVersion >= VK_API_VERSION_1_3) {
                physical = candidate;
                std::printf("adapter: %s vendor=%x device=%x driver=%u api=%u\n", properties.deviceName,
                            properties.vendorID, properties.deviceID, properties.driverVersion, properties.apiVersion);
                break;
            }
        }
        Require(physical != VK_NULL_HANDLE, "NVIDIA Vulkan1.3 adapter unavailable; not a feature regression");
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        bool found = false;
        for (uint32_t i = 0; i < count; ++i) if ((families[i].queueFlags &
                (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            family = i; found = true; break;
        }
        Require(found, "graphics+compute queue unavailable");
        device_query = DlssVulkan::QueryDeviceExtensions(project, instance, physical, device_extensions);
        std::printf("NGX REQUIRED DEVICE EXTENSIONS: count=%zu\n", device_extensions.size());
        for (const auto& extension : device_extensions) std::printf("  NGX device: %s\n", extension.c_str());
        Check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr), "device extension count");
        available.resize(count);
        Check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, available.data()), "device extensions");
        for (const auto& required : device_extensions)
            Require(std::any_of(available.begin(), available.end(), [&](const auto& ext) {
                return required == ext.extensionName; }), "required device extension unsupported");
        // This isolated fixture enables only features the physical device actually advertises.
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
        VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
        vkGetPhysicalDeviceFeatures2(physical, &f2);
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qc.queueFamilyIndex = family; qc.queueCount = 1; qc.pQueuePriorities = &priority;
        names = Pointers(device_extensions);
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f11};
        dc.queueCreateInfoCount = 1; dc.pQueueCreateInfos = &qc; dc.pEnabledFeatures = &f2.features;
        dc.enabledExtensionCount = uint32_t(names.size()); dc.ppEnabledExtensionNames = names.data();
        Check(vkCreateDevice(physical, &dc, nullptr, &device), "create device");
#define DEVICE(name) name = reinterpret_cast<PFN_##name>(gdpa(device, #name)); Require(name != nullptr, #name);
        DEVICE_FUNCS(DEVICE)
#undef DEVICE
        vkGetDeviceQueue(device, family, 0, &queue);
        VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pc.queueFamilyIndex = family; pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        Check(vkCreateCommandPool(device, &pc, nullptr, &pool), "create command pool");
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
        Check(vkAllocateCommandBuffers(device, &ca, &command), "allocate command");
    }
    uint32_t MemoryType(uint32_t mask, VkMemoryPropertyFlags flags) const {
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((mask & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
        throw std::runtime_error("required native memory type unsupported");
    }
    size_t MakeBuffer(VkDeviceSize bytes) {
        Buffer buffer{}; buffer.bytes = bytes;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = bytes; info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        Check(vkCreateBuffer(device, &info, nullptr, &buffer.handle), "create staging buffer");
        VkMemoryRequirements requirements{}; vkGetBufferMemoryRequirements(device, buffer.handle, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = MemoryType(requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        Check(vkAllocateMemory(device, &allocation, nullptr, &buffer.memory), "allocate staging");
        Check(vkBindBufferMemory(device, buffer.handle, buffer.memory, 0), "bind staging");
        Check(vkMapMemory(device, buffer.memory, 0, bytes, 0, &buffer.mapped), "map staging");
        buffers.push_back(buffer); return buffers.size() - 1;
    }
    size_t MakeImage(DlssExtent size, VkFormat format, bool output = false) {
        Image image{}; image.size = size; image.format = format;
        image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT | (output ? VK_IMAGE_USAGE_STORAGE_BIT : 0u);
        VkFormatProperties properties{}; vkGetPhysicalDeviceFormatProperties(physical, format, &properties);
        const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                            (output ? VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT : 0u);
        Require((properties.optimalTilingFeatures & needed) == needed, "synthetic native format unsupported");
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D; info.extent = {size.width, size.height, 1};
        info.mipLevels = info.arrayLayers = 1; info.format = format; info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL; info.usage = image.usage;
        Check(vkCreateImage(device, &info, nullptr, &image.handle), "create image");
        VkMemoryRequirements requirements{}; vkGetImageMemoryRequirements(device, image.handle, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(vkAllocateMemory(device, &allocation, nullptr, &image.memory), "allocate image");
        Check(vkBindImageMemory(device, image.handle, image.memory, 0), "bind image");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image.handle; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        Check(vkCreateImageView(device, &view, nullptr, &image.view), "create image view");
        images.push_back(image); return images.size() - 1;
    }
    void Begin() {
        Check(vkResetCommandBuffer(command, 0), "reset command");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(vkBeginCommandBuffer(command, &begin), "begin command");
    }
    void Submit() {
        Check(vkEndCommandBuffer(command), "end command");
        VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{};
        Check(vkCreateFence(device, &info, nullptr, &fence), "create bounded fence");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command; Check(vkQueueSubmit(queue, 1, &submit, fence), "submit");
        const auto result = vkWaitForFences(device, 1, &fence, VK_TRUE, 10'000'000'000ull);
        if (result != VK_SUCCESS) {
            std::fprintf(stderr, "BOUNDED GPU FAILURE result=%d; no unbounded teardown wait\n", int(result));
            std::fflush(nullptr); std::_Exit(3);
        }
        vkDestroyFence(device, fence, nullptr);
    }
    void Barrier(Image& image, VkImageLayout layout, VkAccessFlags destination) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = image.layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0u :
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = destination; barrier.oldLayout = image.layout; barrier.newLayout = layout;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image.handle; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        image.layout = layout;
    }
    static VkBufferImageCopy Region(const Image& image) {
        VkBufferImageCopy region{}; region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {image.size.width, image.size.height, 1}; return region;
    }
    void Upload(size_t image_index, size_t buffer_index) {
        auto& image = images[image_index]; auto& buffer = buffers[buffer_index];
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT; host.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer = buffer.handle; host.size = buffer.bytes;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 1, &host, 0, nullptr);
        Barrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT);
        auto region = Region(image);
        vkCmdCopyBufferToImage(command, buffer.handle, image.handle,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        Barrier(image, input_layout, VK_ACCESS_SHADER_READ_BIT);
    }
    void Download(size_t image_index, size_t buffer_index) {
        auto& image = images[image_index]; auto& buffer = buffers[buffer_index];
        Barrier(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT);
        auto region = Region(image);
        vkCmdCopyImageToBuffer(command, image.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              buffer.handle, 1, &region);
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer = buffer.handle; host.size = buffer.bytes;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 0, nullptr, 1, &host, 0, nullptr);
    }
    DlssImage Describe(size_t index) const {
        const auto& image = images[index]; DlssImage result{};
        result.image = image.handle; result.view = image.view; result.format = image.format;
        result.extent = image.size; result.layout = image.layout; result.usage = image.usage;
        return result;
    }
};

void ResourceControl(Vulkan& vk) {
    const std::array<VkFormat, 3> formats{VK_FORMAT_R16G16B16A16_SFLOAT,
                                        VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT};
    const std::array<uint32_t, 3> sizes{8, 4, 8};
    for (size_t n = 0; n < formats.size(); ++n) {
        const size_t image = vk.MakeImage({8, 8}, formats[n]);
        const auto bytes = 8u * 8u * sizes[n];
        const size_t upload = vk.MakeBuffer(bytes), download = vk.MakeBuffer(bytes + ReadbackGuardBytes);
        std::memset(vk.buffers[download].mapped, 0xa5, size_t(vk.buffers[download].bytes));
        auto* data = static_cast<unsigned char*>(vk.buffers[upload].mapped);
        // Finite, valid values avoid introducing NaN canonicalization into a layout control.
        if (n == 0) {
            auto* values = reinterpret_cast<uint16_t*>(data);
            for (uint32_t i = 0; i < bytes / 2; ++i) values[i] = Half(0.125f + float(i % 7) * 0.0625f);
        } else {
            auto* values = reinterpret_cast<float*>(data);
            for (uint32_t i = 0; i < bytes / 4; ++i)
                values[i] = n == 1 ? 0.25f + float(i % 7) * 0.0625f : (i % 2 ? -1.25f : 0.75f);
        }
        vk.Begin(); vk.Upload(image, upload); vk.Download(image, download); vk.Submit();
        Require(std::memcmp(data, vk.buffers[download].mapped, bytes) == 0, "native resource roundtrip mismatch");
        VerifyGuard(vk.buffers[download], bytes);
    }
    std::puts("CONTROL PASS: native color/depth/motion image allocation + exact upload/readback");
}

float Halton(unsigned index, unsigned base) {
    float scale = 1.0f, result = 0.0f;
    while (index) { scale /= float(base); result += scale * float(index % base); index /= base; }
    return result - 0.5f;
}
void Generate(Vulkan& vk, size_t color, size_t depth, size_t motion, DlssExtent size,
              unsigned frame, float jitter_x, float jitter_y) {
    auto* c = static_cast<uint16_t*>(vk.buffers[color].mapped);
    auto* d = static_cast<float*>(vk.buffers[depth].mapped);
    auto* m = static_cast<float*>(vk.buffers[motion].mapped);
    const float camera = float(frame) * 0.75f;
    const float rectangle = float(size.width) * 0.35f + float(frame) * 2.0f;
    for (uint32_t y = 0; y < size.height; ++y) for (uint32_t x = 0; x < size.width; ++x) {
        const size_t index = size_t(y) * size.width + x;
        const float world_x = float(x) + 0.5f + jitter_x + camera;
        const float world_y = float(y) + 0.5f + jitter_y;
        const bool object = world_x >= rectangle && world_x < rectangle + float(size.width) * 0.20f &&
                            world_y >= float(size.height) * 0.30f && world_y < float(size.height) * 0.70f;
        const bool checker = (int(world_x / 24.0f) + int(world_y / 24.0f)) & 1;
        const float background = checker ? 0.08f : 0.14f;
        c[index * 4 + 0] = Half(object ? 0.85f : background);
        c[index * 4 + 1] = Half(object ? 0.20f : background);
        c[index * 4 + 2] = Half(object ? 0.10f : background);
        c[index * 4 + 3] = Half(1.0f);
        d[index] = object ? 0.25f : 0.90f; // Visible orthographic planes, conventional Z.
        // Backward motion in render pixels; camera and object motion exclude raster jitter.
        m[index * 2] = frame ? (object ? -1.25f : 0.75f) : 0.0f;
        m[index * 2 + 1] = 0.0f;
    }
}
void CheckOutput(const void* memory, DlssExtent size, unsigned frame) {
    const auto* pixels = static_cast<const uint16_t*>(memory);
    size_t finite = 0; float brightest = 0.0f;
    for (size_t i = 0; i < size_t(size.width) * size.height; ++i) {
        const float r = Unhalf(pixels[i * 4]);
        if (std::isfinite(r)) ++finite;
        brightest = std::max(brightest, r);
    }
    Require(finite == size_t(size.width) * size.height, "DLSS output contains NaN/Inf");
    // Interior samples deliberately avoid ML reconstruction edges and disocclusions.
    const auto sample = [&](float x, float y) {
        return Unhalf(pixels[(size_t(uint32_t(y * size.height)) * size.width + uint32_t(x * size.width)) * 4]);
    };
    const float object = sample(0.45f, 0.50f), background = sample(0.10f, 0.50f);
    std::printf("frame=%u finite=%zu bright=%g object=%g background=%g\n", frame, finite,
                brightest, object, background);
    Require(object > background + 0.20f && brightest > 0.45f,
            "DLSS output did not reconstruct the synthetic visible object");
}
} // namespace

// Full renderer integration draft: the standalone helper class above is reused
// only for native resource allocation/barriers. Its Initialize/Begin/Submit are
// deliberately NOT invoked; all GPU work uses the actual renderer scheduler.
namespace {
struct RendererValidation final {
    VkInstance instance{};
    VkDebugUtilsMessengerEXT messenger{};
    PFN_vkDestroyDebugUtilsMessengerEXT destroy{};
    std::atomic<uint32_t> errors{0};
    bool handed_to_window = false;
    static VKAPI_ATTR VkBool32 VKAPI_CALL Message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void* context) {
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
            static_cast<RendererValidation*>(context)->errors.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr, "PRESENTER VULKAN %s: %s\n",
            severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "ERROR" : "WARNING",
            data && data->pMessage ? data->pMessage : "(no diagnostic text)");
        return VK_FALSE;
    }
    void Attach(GraphicContext& graphics, bool required) {
        if (!required) return;
        // WindowContext only creates this production messenger when it actually
        // enabled the Khronos validation layer. Its fallback must not pass here.
        Require(graphics.debug_messenger != nullptr, "requested real validation layer was not enabled");
        const auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
        Require(gipa != nullptr, "validation observation loader unavailable");
        instance = static_cast<VkInstance>(graphics.instance);
        const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            gipa(instance, "vkCreateDebugUtilsMessengerEXT"));
        destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            gipa(instance, "vkDestroyDebugUtilsMessengerEXT"));
        Require(create && destroy, "requested real validation debug entry points unavailable");
        VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        info.pfnUserCallback = Message; info.pUserData = this;
        Check(create(instance, &info, nullptr, &messenger), "attach actual renderer validation observer");
        // Test-only transfer: WindowContext owns exactly one active counting
        // messenger. Its unchanged destructor destroys the VkDevice first,
        // then this messenger, then the instance. The callback context itself
        // is declared before WindowContext and outlives that entire teardown.
        const auto previous = static_cast<VkDebugUtilsMessengerEXT>(graphics.debug_messenger);
        destroy(instance, previous, nullptr);
        graphics.debug_messenger = messenger;
        handed_to_window = true;
        std::puts("PRESENTER VALIDATION ENABLED: real renderer instance; counter includes final device destruction");
    }
    ~RendererValidation() {
        if (!messenger) return;
        Require(handed_to_window, "validation observer did not transfer its native messenger owner");
        const auto count = errors.load(std::memory_order_relaxed);
        std::printf("PRESENTER VALIDATION RESULT: errors=%u\n", count);
        Require(count == 0, "actual renderer Vulkan validation reported errors including final WindowContext teardown");
        std::puts("PRESENTER VALIDATION PASS: final renderer device teardown");
    }
};
struct BorrowedRendererResources final : Vulkan {
    explicit BorrowedRendererResources(GraphicContext& graphics) {
        gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
        Require(gipa != nullptr, "SDL Vulkan loader unavailable");
        const auto raw_instance = static_cast<VkInstance>(graphics.instance);
        const auto raw_device = static_cast<VkDevice>(graphics.device);
        gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(raw_instance, "vkGetDeviceProcAddr"));
        Require(gdpa != nullptr, "device resolver unavailable");
#define BORROW_INSTANCE(name) name = reinterpret_cast<PFN_##name>(gipa(raw_instance, #name)); Require(name != nullptr, #name);
        INSTANCE_FUNCS(BORROW_INSTANCE)
#undef BORROW_INSTANCE
#define BORROW_DEVICE(name) name = reinterpret_cast<PFN_##name>(gdpa(raw_device, #name)); Require(name != nullptr, #name);
        DEVICE_FUNCS(BORROW_DEVICE)
#undef BORROW_DEVICE
        instance = raw_instance; device = raw_device;
        physical = static_cast<VkPhysicalDevice>(graphics.physical_device);
        queue = static_cast<VkQueue>(graphics.queue); family = graphics.queue_family;
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    }
    ~BorrowedRendererResources() {
        // The ownership lease is retired by the real producer timeline. It owns
        // these images/views/allocations, but never the renderer device/instance.
        for (auto& image : images) {
            if (image.view) vkDestroyImageView(device, image.view, nullptr);
            if (image.handle) vkDestroyImage(device, image.handle, nullptr);
            if (image.memory) vkFreeMemory(device, image.memory, nullptr);
        }
        for (auto& buffer : buffers) {
            if (buffer.mapped) vkUnmapMemory(device, buffer.memory);
            if (buffer.handle) vkDestroyBuffer(device, buffer.handle, nullptr);
            if (buffer.memory) vkFreeMemory(device, buffer.memory, nullptr);
        }
        images.clear(); buffers.clear();
        device = VK_NULL_HANDLE; instance = VK_NULL_HANDLE; physical = VK_NULL_HANDLE;
        queue = VK_NULL_HANDLE; pool = VK_NULL_HANDLE; library = nullptr;
    }
};
void CompleteBounded(GraphicContext& graphics, CommandScheduler& scheduler, uint64_t tick) {
    const vk::Semaphore semaphore = scheduler.GetMasterSemaphore().Handle();
    const vk::SemaphoreWaitInfo wait{.semaphoreCount = 1, .pSemaphores = &semaphore, .pValues = &tick};
    const auto result = graphics.device.waitSemaphores(&wait, 10'000'000'000ull);
    if (result != vk::Result::eSuccess) {
        std::fprintf(stderr, "PRESENTER BOUNDED GPU FAILURE result=%d tick=%llu\n", int(result),
                     static_cast<unsigned long long>(tick));
        std::fflush(nullptr); std::_Exit(3);
    }
    scheduler.GetMasterSemaphore().Refresh();
    scheduler.PopPendingOperations();
    Require(scheduler.IsFree(tick), "producer timeline completion was not published");
}
void ReadbackBorrowed(Vulkan& recorder, const DlssImage& image, Buffer& destination) {
    // This wrapper does not own the Presenter Frame and never destroys it. The
    // exact prior layout is restored so the Frame's real layout tracker remains valid.
    Image raw{}; raw.handle = image.image; raw.view = image.view;
    raw.format = image.format; raw.size = image.extent; raw.usage = image.usage;
    raw.layout = image.layout;
    const auto original = raw.layout;
    recorder.Barrier(raw, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT);
    const auto region = Vulkan::Region(raw);
    recorder.vkCmdCopyImageToBuffer(recorder.command, raw.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    destination.handle, 1, &region);
    VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = destination.handle; host.size = destination.bytes;
    recorder.vkCmdPipelineBarrier(recorder.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                  0, 0, nullptr, 1, &host, 0, nullptr);
    recorder.Barrier(raw, original, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
}
void CaptureRenderDocBootstrap(GraphicContext& graphics, const std::filesystem::path& artifacts) {
    const auto module = GetModuleHandleW(L"renderdoc.dll");
    Require(module != nullptr, "bootstrap capture requires injected RenderDoc DLL");
    const auto get_api = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI"));
    void* raw_api = nullptr;
    Require(get_api && get_api(eRENDERDOC_API_Version_1_6_0, &raw_api) == 1 && raw_api,
            "bootstrap injected RenderDoc API unavailable");
    const auto api = static_cast<RENDERDOC_API_1_6_0*>(raw_api);
    const auto soft_mib = api->GetCaptureOptionU32(eRENDERDOC_Option_SoftMemoryLimit);
    std::printf("RENDERDOC BOOTSTRAP: before=%u softMemoryLimitMiB=%u\n", api->IsFrameCapturing(), soft_mib);
    Require(api->IsFrameCapturing() == 0 && soft_mib == 512,
            "bootstrap capture requires no active capture and injected 512MiB soft limit");
    const auto capture_template = (artifacts / "renderdoc-bootstrap").string();
    api->SetCaptureFilePathTemplate(capture_template.c_str());
    const auto prior_count = api->GetNumCaptures();
    const auto instance = static_cast<VkInstance>(graphics.instance);
    const auto device = RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(instance);
    api->StartFrameCapture(device, nullptr);
    std::printf("RENDERDOC BOOTSTRAP: started IsFrameCapturing=%u\n", api->IsFrameCapturing());
    Require(api->IsFrameCapturing() != 0, "Vulkan RenderDoc frame capturer not registered");
    Require(api->EndFrameCapture(device, nullptr) != 0 && api->IsFrameCapturing() == 0,
            "bootstrap RenderDoc capture failed to finish");
    const auto count = api->GetNumCaptures();
    uint32_t length = 0;
    Require(count > prior_count && api->GetCapture(count - 1, nullptr, &length, nullptr) && length,
            "bootstrap capture did not publish a new capture path");
    std::string path(length, '\0');
    Require(api->GetCapture(count - 1, path.data(), &length, nullptr) != 0,
            "bootstrap capture path unavailable");
    path.resize(std::strlen(path.c_str()));
    Require(std::filesystem::is_regular_file(path), "bootstrap capture file missing");
    std::printf("RENDERDOC BOOTSTRAP PASS: IsFrameCapturing=0 capture=%s bytes=%llu\n", path.c_str(),
                static_cast<unsigned long long>(std::filesystem::file_size(path)));
    std::fflush(stdout);
}
#include "DlssHostStartupTests.inc"
#include "DlssBoundConstantsTests.inc"
} // namespace

int main(int argc, char** argv) {
    if (const auto* host_startup = std::getenv("KYTY_DLSS_HOST_STARTUP_TEST");
        host_startup && host_startup[0] == '1') return RunHostStartupTest();
    if (const auto* bound_constants = std::getenv("KYTY_DLSS_BOUND_CONSTANTS_TEST");
        bound_constants && std::strcmp(bound_constants, "1") == 0) return RunBoundConstantsTest();
    // A failed check exits immediately: no failure path can fall into an
    // unbounded device.waitIdle destructor after a bad GPU submission.
    const auto runtime = argc > 1 ? std::filesystem::absolute(argv[1]) : std::filesystem::current_path();
    const auto artifacts = argc > 2 ? std::filesystem::absolute(argv[2]) :
                                     std::filesystem::current_path() / "dlss-presenter-artifacts";
    std::filesystem::create_directories(artifacts);
    const auto* bootstrap_environment = std::getenv("KYTY_DLSS_PRESENTER_BOOTSTRAP_ONLY");
    const bool bootstrap_only = bootstrap_environment && bootstrap_environment[0] == '1';
    if (!bootstrap_only)
        Require(std::filesystem::exists(runtime / "nvngx_dlss.dll"), "verified runtime dependency missing");
    _putenv_s("KYTY_RECORD_THREAD", "0"); // Test-only deterministic native command recording.
    const auto* validation_environment = std::getenv("KYTY_DLSS_SMOKE_VALIDATION");
    const bool validation_requested = validation_environment && validation_environment[0] == '1';
    // Before our collector is attached, genuine production validation errors
    // must already fail the test. After Attach, our collector owns diagnostics.
    if (validation_requested) _putenv_s("KYTY_DEBUG_VK_ERRORS_NONFATAL", "0");
    Common::InitializeThreads();
    Common::Subsystems subsystems;
    subsystems.Initialize<Config::Lifecycle>();
    Config::ConfigOptions options;
    options.printf_direction = Config::LogDirection::Silent;
    options.gpu_frames_ahead = 0; options.label_flush_interval_us = 0;
    options.screen_width = 1920; options.screen_height = 1080;
    options.vulkan_validation_enabled = validation_requested;
    if (bootstrap_only) options.bindless_images_enabled = true;
    Config::Load(options);
    subsystems.Initialize<Log::Lifecycle>();
    subsystems.Initialize<Libs::LibKernel::Memory::Lifecycle>();
    ShaderInit();
    Require(SDL_InitSubSystem(SDL_INIT_VIDEO), SDL_GetError());
    RendererValidation validation; // MUST outlive WindowContext and its instance.
    WindowContext window;
    window.graphic_ctx.screen_width = 1920; window.graphic_ctx.screen_height = 1080;
    window.window = SDL_CreateWindow("Kyty DLSS Presenter GPU fixture", 1920, 1080,
                                     SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    Require(window.window != nullptr, SDL_GetError());
    DlssProjectInfo project;
    project.project_id = "2b495df0-739f-4900-8f48-8e5ef313ce70";
    project.engine_version = "KytyPS5-synthetic-presenter-1";
    project.application_data_path = artifacts.wstring(); project.feature_search_path = runtime.wstring();
    window.CreateVulkan(bootstrap_only ? nullptr : &project);
    validation.Attach(window.graphic_ctx, validation_requested);
    if (bootstrap_only) {
        Require(validation_requested, "bootstrap regression requires real validation, not just exit status");
        Require(window.DlssNegotiationStatus().code == DlssStatusCode::Unsupported &&
                !window.requested_dlss_project.has_value(), "ordinary bootstrap unexpectedly negotiated DLSS");
        Require(window.render_context && window.presenter && window.graphic_ctx.bindless_enabled,
                "ordinary bootstrap did not construct the actual bindless renderer");
        const auto* renderdoc_capture = std::getenv("KYTY_RENDERDOC_BOOTSTRAP_CAPTURE");
        if (renderdoc_capture && std::strcmp(renderdoc_capture, "1") == 0) {
            Require(argc > 2, "bootstrap capture requires an explicit artifact directory argument");
            CaptureRenderDocBootstrap(window.graphic_ctx, artifacts);
        }
        // Release the normal owners exactly as WindowContext does, retaining the
        // real allocator/device for an observational cache snapshot only. Their
        // destruction invokes actual production DeleteImage, never test Clear.
        window.presenter.reset();
        Libs::LibKernel::Memory::InstallGpuResources(nullptr);
        window.render_context.reset();
        size_t retained_count = 0;
        const auto retained_bytes = NativeImageRecyclerRetainedBytesForTests(
            window.graphic_ctx.allocator, &retained_count);
        std::printf("BOOTSTRAP CONTROL: ordinary renderer retired cached images=%zu bytes=%llu; no NGX Init/Eval\n",
                    retained_count, static_cast<unsigned long long>(retained_bytes));
        Require(retained_count > 0 && retained_bytes > 0,
                "ordinary bootstrap did not exercise native image retention before allocator teardown");
        // Unchanged actual WindowContext destructor now destroys allocator and
        // VkDevice; the transferred collector remains active through it. A leak
        // must produce exit2 from RendererValidation after that destructor.
        return 0;
    }
    CheckDlss(window.DlssNegotiationStatus(), "WindowContext optional extension negotiation");
    Require(window.render_context && window.presenter, "actual renderer/Presenter was not constructed");
    auto backend = std::make_shared<DlssVulkan>();
    CheckDlss(window.InitializeDlssBackend(*backend), "InitializeDlssBackend on actual renderer device");
    DlssOptimalSettings optimal;
    const DlssExtent output{1920, 1080};
    CheckDlss(backend->GetOptimalSettings(output, DlssQuality::Performance, optimal), "GetOptimalSettings");
    const auto input = optimal.optimal;
    Require(input.width > 0 && input.height > 0, "invalid SDK optimal render extent");
    std::printf("PRESENTER dimensions input=%ux%u output=%ux%u\n", input.width, input.height,
                output.width, output.height);
    auto& graphics = window.graphic_ctx;
    auto& scheduler = window.render_context->GetCommandScheduler();
    auto& presenter = *window.presenter;
    HW::Context registers{}; HW::UserConfig user_config{}; HW::Shader shaders{};
    scheduler.Begin(registers, user_config, shaders);
    DlssConfiguration config;
    config.render = input; config.output = output; config.quality = DlssQuality::Performance;
    config.preset = DlssPreset::TransformerM;
    config.hdr = false; // Explicit display-ready LDR; the presenter performs no tone mapping.
    CheckDlss(backend->RecordCreate(static_cast<VkCommandBuffer>(scheduler.Current().Handle()), config),
              "RecordCreate on actual renderer producer");
    scheduler.NoteCommandRecorded();
    CompleteBounded(graphics, scheduler, scheduler.Submit());

    // Readback allocations are independent from the SDK input lease and survive
    // its retirement, so a second Frame readback can prove independent ownership.
    BorrowedRendererResources readbacks(graphics);
    const size_t payload = size_t(output.width) * output.height * 8;
    const size_t sdk_readback = readbacks.MakeBuffer(payload + ReadbackGuardBytes);
    const size_t frame_readback = readbacks.MakeBuffer(payload + ReadbackGuardBytes);
    const size_t independent_readback = readbacks.MakeBuffer(payload + ReadbackGuardBytes);
    for (auto& buffer : readbacks.buffers) std::memset(buffer.mapped, 0xa5, size_t(buffer.bytes));
    auto resources = std::make_shared<BorrowedRendererResources>(graphics);
    const size_t ci = resources->MakeImage(input, VK_FORMAT_R16G16B16A16_SFLOAT);
    const size_t di = resources->MakeImage(input, VK_FORMAT_R32_SFLOAT);
    const size_t mi = resources->MakeImage(input, VK_FORMAT_R32G32_SFLOAT);
    const size_t oi = resources->MakeImage(output, VK_FORMAT_R16G16B16A16_SFLOAT, true);
    const size_t cu = resources->MakeBuffer(uint64_t(input.width) * input.height * 8);
    const size_t du = resources->MakeBuffer(uint64_t(input.width) * input.height * 4);
    const size_t mu = resources->MakeBuffer(uint64_t(input.width) * input.height * 8);
    const float jitter_x = Halton(1, 2), jitter_y = Halton(1, 3);
    Generate(*resources, cu, du, mu, input, 0, jitter_x, jitter_y);
    scheduler.Begin(registers, user_config, shaders);
    auto& producer = scheduler.Current(); producer.EndRendering(); DrainGpuThreadCommands();
    const auto command = static_cast<VkCommandBuffer>(producer.Handle());
    resources->command = command; readbacks.command = command;
    resources->Upload(ci, cu); resources->Upload(di, du); resources->Upload(mi, mu);
    resources->Barrier(resources->images[oi], VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT);
    scheduler.NoteCommandRecorded();
    DlssPresentationInput request;
    request.frame_id = 1; request.configuration = config;
    request.backend = backend; request.resource_lease = resources;
    request.inputs.color = resources->Describe(ci); request.inputs.depth = resources->Describe(di);
    request.inputs.motion_vectors = resources->Describe(mi); request.inputs.output = resources->Describe(oi);
    request.inputs.render = input; request.inputs.jitter_x = jitter_x; request.inputs.jitter_y = jitter_y;
    request.inputs.motion_scale_x = request.inputs.motion_scale_y = 1.0f;
    request.inputs.frame_time_ms = 1000.0f / 60.0f;
    request.inputs.reset_history = false; // First success must force reset inside the real Presenter.
    DlssStatus status;
    auto* frame = presenter.PrepareTemporalFrame(producer, request, status);
    CheckDlss(status, "actual Presenter::PrepareTemporalFrame");
    Require(frame != nullptr, "actual Presenter did not return an owned Frame");
    Require(presenter.TestLastTemporalFrameId() == 1 && presenter.TestEvaluationAttempts() == 1,
            "successful actual evaluation did not advance exactly one guest frame");
    Require(presenter.TestLastEvaluationReset(), "first actual SDK evaluation did not reset temporal history");
    const auto owned = Presenter::TestFrameImage(*frame);
    Require(owned.image != request.inputs.output.image && owned.extent == output &&
            owned.format == VK_FORMAT_R16G16B16A16_SFLOAT &&
            (owned.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT), "Presenter Frame is not an independent readable image");
    DlssStatus duplicate;
    Require(presenter.PrepareTemporalFrame(producer, request, duplicate) == nullptr &&
            duplicate.code == DlssStatusCode::InvalidArgument && presenter.TestEvaluationAttempts() == 1 &&
            presenter.TestLastTemporalFrameId() == 1, "duplicate guest id evaluated or advanced temporal history");
    // The production bridge promises to restore the SDK output to GENERAL. We
    // use that actual contract as oldLayout; an enabled validation layer checks it.
    ReadbackBorrowed(readbacks, request.inputs.output, readbacks.buffers[sdk_readback]);
    ReadbackBorrowed(readbacks, owned, readbacks.buffers[frame_readback]);
    scheduler.NoteCommandRecorded();
    const std::weak_ptr<BorrowedRendererResources> weak = resources;
    request.resource_lease.reset(); resources.reset();
    Require(!weak.expired(), "Presenter released native input ownership before producer submission");
    const auto first_tick = scheduler.Submit();
    CompleteBounded(graphics, scheduler, first_tick);
    Require(weak.expired(), "Presenter retained resource lease after completed producer callbacks were drained");
    VerifyGuard(readbacks.buffers[sdk_readback], payload); VerifyGuard(readbacks.buffers[frame_readback], payload);
    Require(std::memcmp(readbacks.buffers[sdk_readback].mapped, readbacks.buffers[frame_readback].mapped,
                        payload) == 0, "Presenter-owned output differs from real SDK output");
    CheckOutput(readbacks.buffers[frame_readback].mapped, output, 0);
    LogLoadedRuntime(runtime / "nvngx_dlss.dll");
    std::puts("PRESENTER PASS: real evaluate/copy; duplicate id rejected; first reset; producer lease retired");

    scheduler.Begin(registers, user_config, shaders);
    scheduler.Current().EndRendering(); DrainGpuThreadCommands();
    readbacks.command = static_cast<VkCommandBuffer>(scheduler.Current().Handle());
    ReadbackBorrowed(readbacks, Presenter::TestFrameImage(*frame), readbacks.buffers[independent_readback]);
    scheduler.NoteCommandRecorded();
    CompleteBounded(graphics, scheduler, scheduler.Submit());
    VerifyGuard(readbacks.buffers[independent_readback], payload);
    Require(std::memcmp(readbacks.buffers[sdk_readback].mapped, readbacks.buffers[independent_readback].mapped,
                        payload) == 0, "owned Frame changed after source images/views/allocations were destroyed");
    presenter.Discard(*frame);
    CheckDlss(backend->DestroyFeatureAfterGpuIdle(), "DestroyFeatureAfterGpuIdle");
    CheckDlss(backend->ShutdownAfterGpuIdle(), "ShutdownAfterGpuIdle");
    backend.reset();
    std::puts("PRESENTER GPU CORE CHECKS COMPLETE: independent Frame survives input retirement; final teardown validation pending");
    // This fixture does not call PresentLastFrame, claim a game provider or measure guest FPS.
    return 0;
}
