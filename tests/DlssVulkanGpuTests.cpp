// Synthetic, bounded Vulkan/NGX integration fixture. No guest data or game heuristics.
#define VK_NO_PROTOTYPES
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winver.h>
#include "graphics/presentation/dlss/dlssVulkan.h"

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
    if (!value) throw std::runtime_error(message);
}
void Check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "VULKAN SETUP/EXECUTION ERROR: %s result=%d\n", operation, int(result));
        throw std::runtime_error(operation);
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

int main(int argc, char** argv) {
    try {
        const auto runtime = argc > 1 ? std::filesystem::absolute(argv[1]) : std::filesystem::current_path();
        const auto artifacts = argc > 2 ? std::filesystem::absolute(argv[2]) :
                                         std::filesystem::current_path() / "dlss-smoke-artifacts";
        std::filesystem::create_directories(artifacts);
        Require(std::filesystem::exists(runtime / "nvngx_dlss.dll"),
                "runtime DLL missing: dependency failure is not a regression reproduction");
        DlssProjectInfo project{};
        project.project_id = "2b495df0-739f-4900-8f48-8e5ef313ce70";
        project.engine_version = "KytyPS5-synthetic-smoke-1";
        project.application_data_path = artifacts.wstring(); project.feature_search_path = runtime.wstring();
        Vulkan vk; vk.Initialize(project); ResourceControl(vk);
        CheckDlss(vk.instance_query, "QueryInstanceExtensions after resource controls");
        CheckDlss(vk.device_query, "QueryDeviceExtensions after resource controls");
        DlssVulkan dlss;
        DlssInitializeInfo initialize{}; initialize.project = project;
        initialize.instance = vk.instance; initialize.physical_device = vk.physical; initialize.device = vk.device;
        initialize.get_instance_proc_addr = vk.gipa; initialize.get_device_proc_addr = vk.gdpa;
        initialize.enabled_instance_extensions = vk.instance_extensions;
        initialize.enabled_device_extensions = vk.device_extensions;
        CheckDlss(dlss.Initialize(initialize), "Initialize");
        DlssOptimalSettings optimal{};
        const DlssExtent output{1920, 1080};
        CheckDlss(dlss.GetOptimalSettings(output, DlssQuality::Performance, optimal), "GetOptimalSettings");
        const auto input = optimal.optimal;
        Require(input.width && input.height && input.width <= output.width && input.height <= output.height,
                "invalid SDK optimal dimensions");
        std::printf("dimensions: input=%ux%u output=%ux%u\n", input.width, input.height, output.width, output.height);
        const size_t ci = vk.MakeImage(input, VK_FORMAT_R16G16B16A16_SFLOAT);
        const size_t di = vk.MakeImage(input, VK_FORMAT_R32_SFLOAT);
        const size_t mi = vk.MakeImage(input, VK_FORMAT_R32G32_SFLOAT);
        const size_t oi = vk.MakeImage(output, VK_FORMAT_R16G16B16A16_SFLOAT, true);
        const size_t cu = vk.MakeBuffer(uint64_t(input.width) * input.height * 8);
        const size_t du = vk.MakeBuffer(uint64_t(input.width) * input.height * 4);
        const size_t mu = vk.MakeBuffer(uint64_t(input.width) * input.height * 8);
        const size_t output_payload = size_t(output.width) * output.height * 8;
        const size_t readback = vk.MakeBuffer(output_payload + ReadbackGuardBytes);
        DlssConfiguration configuration{}; configuration.render = input; configuration.output = output;
        configuration.quality = DlssQuality::Performance;
        configuration.preset = DlssPreset::TransformerM;
        configuration.hdr = true; configuration.auto_exposure = true;
        vk.Begin(); CheckDlss(dlss.RecordCreate(vk.command, configuration), "RecordCreate"); vk.Submit();
        for (unsigned frame = 0; frame < 8; ++frame) {
            std::memset(vk.buffers[readback].mapped, 0xa5, size_t(vk.buffers[readback].bytes));
            const float jitter_x = Halton(frame + 1, 2), jitter_y = Halton(frame + 1, 3);
            Generate(vk, cu, du, mu, input, frame, jitter_x, jitter_y);
            vk.Begin(); vk.Upload(ci, cu); vk.Upload(di, du); vk.Upload(mi, mu);
            vk.Barrier(vk.images[oi], VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT);
            DlssFrameInputs inputs{}; inputs.color = vk.Describe(ci); inputs.depth = vk.Describe(di);
            inputs.motion_vectors = vk.Describe(mi); inputs.output = vk.Describe(oi); inputs.render = input;
            inputs.jitter_x = jitter_x; inputs.jitter_y = jitter_y;
            inputs.motion_scale_x = inputs.motion_scale_y = 1.0f;
            inputs.frame_time_ms = 1000.0f / 60.0f; inputs.reset_history = frame == 0 || frame == 4;
            CheckDlss(DlssVulkan::ValidateFrame(configuration, inputs), "ValidateFrame");
            CheckDlss(dlss.RecordEvaluate(vk.command, inputs), "RecordEvaluate");
            vk.Download(oi, readback); vk.Submit();
            if (frame == 0) LogLoadedRuntime(runtime / "nvngx_dlss.dll");
            VerifyGuard(vk.buffers[readback], output_payload);
            CheckOutput(vk.buffers[readback].mapped, output, frame);
        }
        CheckDlss(dlss.DestroyFeatureAfterGpuIdle(), "DestroyFeatureAfterGpuIdle");
        CheckDlss(dlss.ShutdownAfterGpuIdle(), "ShutdownAfterGpuIdle");
        vk.ValidateAndCleanup();
        std::puts("PASS: real DLSS Vulkan SR evaluated 8 temporal frames with bounded GPU readback; no guest/game proof");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1;
    }
}
