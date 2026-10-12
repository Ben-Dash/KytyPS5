#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/pipeline/bindlessTable.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/shader.h"
#include "kernel/memory.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Libs::Graphics {
namespace {
using u32 = uint32_t;

void Require(const char *name, const char *stage, bool ok, const std::string &message) {
  if (!ok) {
    std::fprintf(stderr, "BindlessLifecycleTests: %s failed at %s: %s\n",
                 name, stage, message.c_str());
    std::abort();
  }
}

// Native setup only: the shared fixture below invokes the actual production
// allocator. No scheduler, region, mapping or feedback behavior is mirrored here.
class VulkanHarness {
public:
  struct Image {
    vk::Image image = nullptr;
    VmaAllocation allocation = nullptr;
    vk::ImageView view = nullptr;
    vk::ImageLayout layout = vk::ImageLayout::eUndefined;
  };

  explicit VulkanHarness(bool require_push_descriptors = false) { Init(require_push_descriptors); }
  ~VulkanHarness() {
    if (m_graphics.device != nullptr) {
      RequireVulkanSuccess(m_graphics.device.waitIdle(), "lifecycle device cleanup");
      m_renderer.reset();
      if (m_pool != nullptr) m_graphics.device.destroyCommandPool(m_pool, nullptr);
      if (m_graphics.allocator != nullptr) vmaDestroyAllocator(m_graphics.allocator);
      m_graphics.device.destroy(nullptr);
    }
    if (m_graphics.instance != nullptr) m_graphics.instance.destroy(nullptr);
  }
  [[nodiscard]] vk::Device Device() const { return m_graphics.device; }
  [[nodiscard]] GraphicContext &RuntimeContext() { return m_graphics; }
  [[nodiscard]] RenderContext &RuntimeRenderer() { return *m_renderer; }

  Image CreateImage2D(const char *name, u32 width, u32 height, vk::Format format,
                      vk::ImageUsageFlags usage, const std::vector<u32> &initial,
                      u32 dwords_per_pixel, vk::ImageLayout final_layout) {
    Require(name, "native fixture image", width == 1u && height == 1u &&
                dwords_per_pixel == 4u && initial.size() == 4u &&
                format == vk::Format::eR32G32B32A32Sfloat &&
                final_layout == vk::ImageLayout::eShaderReadOnlyOptimal,
            "standalone bootstrap supports only the fixture's real1x1 RGBA image");
    Image result;
    vk::ImageCreateInfo info{};
    info.imageType = vk::ImageType::e2D;
    info.format = format;
    info.extent = vk::Extent3D{width, height, 1u};
    info.mipLevels = info.arrayLayers = 1u;
    info.samples = vk::SampleCountFlagBits::e1;
    info.tiling = vk::ImageTiling::eOptimal;
    info.usage = usage | vk::ImageUsageFlagBits::eTransferDst;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VkImage native_image = VK_NULL_HANDLE;
    RequireVulkanSuccess(static_cast<vk::Result>(vmaCreateImage(
        m_graphics.allocator, reinterpret_cast<const VkImageCreateInfo *>(&info),
        &allocation, &native_image, &result.allocation, nullptr)), "lifecycle image allocation");
    result.image = native_image;
    vk::ImageViewCreateInfo view{};
    view.image = result.image;
    view.viewType = vk::ImageViewType::e2D;
    view.format = format;
    view.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0u, 1u, 0u, 1u};
    RequireVulkanSuccess(m_graphics.device.createImageView(&view, nullptr, &result.view),
                         "lifecycle image view");

    vk::CommandBufferAllocateInfo allocate{};
    allocate.commandPool = m_pool;
    allocate.level = vk::CommandBufferLevel::ePrimary;
    allocate.commandBufferCount = 1u;
    vk::CommandBuffer command = nullptr;
    RequireVulkanSuccess(m_graphics.device.allocateCommandBuffers(&allocate, &command),
                         "lifecycle image command allocation");
    vk::CommandBufferBeginInfo begin{};
    begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    RequireVulkanSuccess(command.begin(&begin), "lifecycle image command begin");
    vk::ImageMemoryBarrier barrier{};
    barrier.oldLayout = vk::ImageLayout::eUndefined;
    barrier.newLayout = vk::ImageLayout::eTransferDstOptimal;
    barrier.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = result.image;
    barrier.subresourceRange = view.subresourceRange;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                            vk::PipelineStageFlagBits::eTransfer, {},
                            0u, nullptr, 0u, nullptr, 1u, &barrier);
    vk::ClearColorValue clear{};
    for (uint32_t component = 0; component < 4u; ++component)
      clear.float32[component] = std::bit_cast<float>(initial[component]);
    command.clearColorImage(result.image, vk::ImageLayout::eTransferDstOptimal,
                            &clear, 1u, &view.subresourceRange);
    barrier.oldLayout = barrier.newLayout;
    barrier.newLayout = final_layout;
    barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                            vk::PipelineStageFlagBits::eAllCommands, {},
                            0u, nullptr, 0u, nullptr, 1u, &barrier);
    RequireVulkanSuccess(command.end(), "lifecycle image command end");
    vk::FenceCreateInfo fence_info{};
    vk::Fence fence = nullptr;
    RequireVulkanSuccess(m_graphics.device.createFence(&fence_info, nullptr, &fence),
                         "lifecycle image fence");
    vk::SubmitInfo submit{};
    submit.commandBufferCount = 1u;
    submit.pCommandBuffers = &command;
    RequireVulkanSuccess(m_graphics.queue.submit(1u, &submit, fence),
                         "lifecycle image transfer submit");
    // Only a finite transfer/clear command is submitted, bounded independently
    // of the coordinator's lifetime. No guest or synthetic shader is dispatched.
    RequireVulkanSuccess(m_graphics.device.waitForFences(1u, &fence, VK_TRUE,
                                                        10'000'000'000ull),
                         "lifecycle image transfer fence");
    m_graphics.device.destroyFence(fence, nullptr);
    m_graphics.device.freeCommandBuffers(m_pool, 1u, &command);
    result.layout = final_layout;
    return result;
  }
  void DestroyImage(Image *image) {
    m_graphics.device.destroyImageView(image->view, nullptr);
    vmaDestroyImage(m_graphics.allocator, static_cast<VkImage>(image->image), image->allocation);
    *image = {};
  }

private:
  void Init(bool require_push_descriptors) {
    static vk::detail::DynamicLoader loader;
    const auto get_instance_proc_addr = loader.getProcAddress<PFN_vkGetInstanceProcAddr>(
        "vkGetInstanceProcAddr");
    Require("BindlessLifecycle", "Vulkan loader", get_instance_proc_addr != nullptr,
            "Vulkan loader unavailable; this is not a passing regression");
    VULKAN_HPP_DEFAULT_DISPATCHER.init(get_instance_proc_addr);
    vk::ApplicationInfo application{};
    application.pApplicationName = "BindlessLifecycleTests";
    application.apiVersion = VK_API_VERSION_1_3;
    vk::InstanceCreateInfo instance{};
    instance.pApplicationInfo = &application;
    RequireVulkanSuccess(vk::createInstance(&instance, nullptr, &m_graphics.instance),
                         "lifecycle instance creation");
    VULKAN_HPP_DEFAULT_DISPATCHER.init(m_graphics.instance);

    vk::PhysicalDeviceVulkan11Features features11{};
    vk::PhysicalDeviceVulkan12Features features12{};
    vk::PhysicalDeviceVulkan13Features features13{};
    features13.pNext = &features12;
    features12.pNext = &features11;
    vk::PhysicalDeviceFeatures2 features{};
    features.pNext = &features13;
    uint32_t device_count = 0;
    RequireVulkanSuccess(m_graphics.instance.enumeratePhysicalDevices(&device_count, nullptr),
                         "lifecycle physical device count");
    std::vector<vk::PhysicalDevice> devices(device_count);
    RequireVulkanSuccess(m_graphics.instance.enumeratePhysicalDevices(&device_count, devices.data()),
                         "lifecycle physical devices");
    const char *requested = std::getenv("KYTY_TEST_GPU");
    for (const auto physical : devices) {
      const auto properties = physical.getProperties();
      if (requested != nullptr && std::string_view(properties.deviceName.data()).find(requested) ==
                                      std::string_view::npos) continue;
      if (properties.apiVersion < VK_API_VERSION_1_3) continue;
      physical.getFeatures2(&features);
      if (!features.features.shaderInt64 || !features.features.robustBufferAccess ||
          !features11.storageBuffer16BitAccess || !features12.storageBuffer8BitAccess ||
          !features12.timelineSemaphore || !features12.bufferDeviceAddress ||
          !features12.samplerMirrorClampToEdge || !features12.samplerFilterMinmax ||
          !features12.shaderOutputLayer || !features12.shaderOutputViewportIndex ||
          !features12.shaderBufferInt64Atomics || !features13.dynamicRendering ||
          !features13.synchronization2 || !features12.runtimeDescriptorArray ||
          !features12.shaderSampledImageArrayNonUniformIndexing ||
          !features12.descriptorBindingPartiallyBound ||
          !features12.descriptorBindingSampledImageUpdateAfterBind ||
          !features12.descriptorBindingUpdateUnusedWhilePending) continue;
      uint32_t push_limit = 0u;
      if (require_push_descriptors) {
        uint32_t extension_count = 0u;
        RequireVulkanSuccess(physical.enumerateDeviceExtensionProperties(nullptr, &extension_count, nullptr),
                             "fault init extension count");
        std::vector<vk::ExtensionProperties> extensions(extension_count);
        RequireVulkanSuccess(physical.enumerateDeviceExtensionProperties(nullptr, &extension_count, extensions.data()),
                             "fault init extension query");
        const bool advertised = std::any_of(extensions.begin(), extensions.end(), [](const auto& extension) {
          return std::string_view(extension.extensionName.data()) == VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
        });
        if (!advertised) continue;
        vk::PhysicalDevicePushDescriptorPropertiesKHR push_properties{};
        vk::PhysicalDeviceProperties2 push_query{};
        push_query.pNext = &push_properties;
        physical.getProperties2(&push_query);
        push_limit = push_properties.maxPushDescriptors;
        if (push_limit < 2u) continue;
      }
      const auto queues = physical.getQueueFamilyProperties();
      for (uint32_t index = 0; index < queues.size(); ++index) {
        constexpr auto flags = vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute;
        if ((queues[index].queueFlags & flags) != flags) continue;
        m_graphics.physical_device = physical;
        m_graphics.physical_device_properties = properties;
        m_graphics.queue_family = index;
        m_graphics.max_push_descriptors = push_limit;
        break;
      }
      if (m_graphics.physical_device != nullptr) break;
    }
    if (require_push_descriptors)
      Require("FaultBitmapInitializationGpu", "actual parser push-descriptor capability",
              m_graphics.physical_device != nullptr,
              "no device advertises VK_KHR_push_descriptor with maxPushDescriptors >= 2; unsupported setup, not a passing regression");
    Require("BindlessLifecycle", "required native capabilities",
            m_graphics.physical_device != nullptr,
            "no Vulkan1.3 graphics/compute device with production buffer and bindless UAB features; not a passing regression");
    vk::PhysicalDeviceVulkan12Properties properties12{};
    vk::PhysicalDeviceProperties2 properties2{};
    properties2.pNext = &properties12;
    m_graphics.physical_device.getProperties2(&properties2);
    m_graphics.bindless_enabled = true;
    m_graphics.bindless_max_sampled_images = std::min(
        properties12.maxDescriptorSetUpdateAfterBindSampledImages,
        properties12.maxPerStageDescriptorUpdateAfterBindSampledImages);
    m_graphics.bindless_max_samplers = std::min({BindlessTable::MaxSamplers,
        properties12.maxDescriptorSetUpdateAfterBindSamplers,
        properties12.maxPerStageDescriptorUpdateAfterBindSamplers});
    const uint64_t image_descriptors = uint64_t{BindlessTable::ImageArrays} *
        std::min(BindlessTable::MaxImagesPerArray,
                 m_graphics.bindless_max_sampled_images / BindlessTable::ImageArrays);
    Require("BindlessLifecycle", "descriptor capacity",
            m_graphics.bindless_max_sampled_images >= 8u * BindlessTable::ImageArrays &&
                m_graphics.bindless_max_samplers != 0u &&
                properties12.maxDescriptorSetUpdateAfterBindStorageBuffers >= 2u &&
                properties12.maxPerStageDescriptorUpdateAfterBindStorageBuffers >= 2u &&
                image_descriptors + m_graphics.bindless_max_samplers + 2u <=
                    properties12.maxUpdateAfterBindDescriptorsInAllPools,
            "actual device cannot allocate the production bindless layout and slot7; not a passing regression");
    std::printf("[host] lifecycle GPU: %s; driver=%u; real bindless capability checked\n",
                m_graphics.physical_device_properties.deviceName.data(),
                m_graphics.physical_device_properties.driverVersion);
    float priority = 1.0f;
    vk::DeviceQueueCreateInfo queue{};
    queue.queueFamilyIndex = m_graphics.queue_family;
    queue.queueCount = 1u;
    queue.pQueuePriorities = &priority;
    vk::DeviceCreateInfo device{};
    // Enable only features reported supported by this selected device. These
    // are core Vulkan1.1/1.2/1.3 structures. The init mode also exercises the
    // actual parser's KHR push-descriptor path, queried separately above.
    device.pNext = &features13;
    device.pEnabledFeatures = &features.features;
    device.queueCreateInfoCount = 1u;
    device.pQueueCreateInfos = &queue;
    const char* push_extension = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
    if (require_push_descriptors) {
      device.enabledExtensionCount = 1u;
      device.ppEnabledExtensionNames = &push_extension;
      std::printf("[host] init parser VK_KHR_push_descriptor enabled; maxPushDescriptors=%u\n",
                  m_graphics.max_push_descriptors);
    }
    RequireVulkanSuccess(m_graphics.physical_device.createDevice(&device, nullptr, &m_graphics.device),
                         "lifecycle native device creation");
    VULKAN_HPP_DEFAULT_DISPATCHER.init(m_graphics.device);
    m_graphics.device.getQueue(m_graphics.queue_family, 0u, &m_graphics.queue);
    m_graphics.physical_device.getMemoryProperties(&m_graphics.physical_device_memory_properties);
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo allocator{};
    allocator.instance = m_graphics.instance;
    allocator.physicalDevice = m_graphics.physical_device;
    allocator.device = m_graphics.device;
    allocator.vulkanApiVersion = VK_API_VERSION_1_3;
    allocator.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    allocator.pVulkanFunctions = &functions;
    RequireVulkanSuccess(static_cast<vk::Result>(vmaCreateAllocator(&allocator, &m_graphics.allocator)),
                         "lifecycle VMA allocator");
    vk::CommandPoolCreateInfo pool{};
    pool.queueFamilyIndex = m_graphics.queue_family;
    pool.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    RequireVulkanSuccess(m_graphics.device.createCommandPool(&pool, nullptr, &m_pool),
                         "lifecycle transfer command pool");
    m_renderer = std::make_unique<RenderContext>(m_graphics);
  }
  GraphicContext m_graphics{};
  vk::CommandPool m_pool = nullptr;
  std::unique_ptr<RenderContext> m_renderer;
};
} // namespace
} // namespace Libs::Graphics

#include "BindlessLifecycleTests.inc"
#include "BindlessLifecycleNeighborTests.inc"
#include "BindlessImageAccessAgeTests.inc"
#include "BindlessImageAccessNeighborTests.inc"
#include "BindlessDescriptorRetirementTests.inc"
#include "BindlessDescriptorRetirementNeighborTests.inc"
#include "EmbeddedFetchGpuTests.inc"
#include "VertexValueTraceGpuTests.inc"
#include "GenericBufferCodegenTests.inc"
#include "EndOfPipeWriteGpuTests.inc"
#include "EndOfPipeLiteralGpuTests.inc"
#include "EndOfPipeLiteralNeighborTests.inc"
#include "FaultBufferProtocolGpuTests.inc"
#include "FaultBufferProtocolNeighborTests.inc"
#include "FaultBitmapInitializationGpuTests.inc"
#include "FaultBitmapInitializationNeighborTests.inc"
#include "FaultFirstBindingDeferredGpuTests.inc"
#include "FaultFirstBindingDeferredNeighborTests.inc"

int main(int argc, char **argv) {
  using namespace Libs::Graphics;
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  Require("BindlessLifecycle", "arguments", argc == 1 ||
              (argc == 2 && (std::strcmp(argv[1], "--bindless-lifecycle-only") == 0 ||
                            std::strcmp(argv[1], "--bindless-preparation-neighbors") == 0 ||
                            std::strcmp(argv[1], "--bindless-feedback-neighbors") == 0 ||
                            std::strcmp(argv[1], "--bindless-image-access-age") == 0 ||
                            std::strcmp(argv[1], "--bindless-image-access-neighbors") == 0 ||
                            std::strcmp(argv[1], "--bindless-descriptor-retirement-readback") == 0 ||
                            std::strcmp(argv[1], "--bindless-descriptor-retirement-neighbors") == 0 ||
                            std::strcmp(argv[1], "--embedded-fetch-gpu-readback") == 0 ||
                            std::strcmp(argv[1], "--vertex-value-trace-gpu-readback") == 0 ||
                            std::strcmp(argv[1], "--vertex-nf-trace-gpu-readback") == 0 ||
                            std::strcmp(argv[1], "--generic-buffer-codegen-cost") == 0 ||
                            std::strcmp(argv[1], "--generic-buffer-numeric-readback") == 0 ||
                            std::strcmp(argv[1], "--generic-buffer-numeric-measurement") == 0 ||
                            std::strcmp(argv[1], "--eop-write-ordering-gpu") == 0 ||
                            std::strcmp(argv[1], "--eop-literal-ordering-gpu") == 0 ||
                            std::strcmp(argv[1], "--eop-literal-neighbors-gpu") == 0 ||
                            std::strcmp(argv[1], "--fault-buffer-protocol-gpu") == 0 ||
                            std::strcmp(argv[1], "--fault-buffer-protocol-neighbors-gpu") == 0 ||
                            std::strcmp(argv[1], "--fault-bitmap-initialization-gpu") == 0 ||
                            std::strcmp(argv[1], "--fault-bitmap-initialization-neighbors-gpu") == 0 ||
                            std::strcmp(argv[1], "--fault-first-binding-deferred-gpu") == 0 ||
                            std::strcmp(argv[1], "--fault-first-binding-state-neighbors-gpu") == 0)),
          "usage: bindless_lifecycle_tests [--bindless-lifecycle-only | --bindless-preparation-neighbors | --bindless-feedback-neighbors | --bindless-image-access-age | --bindless-image-access-neighbors | --bindless-descriptor-retirement-readback | --bindless-descriptor-retirement-neighbors | --embedded-fetch-gpu-readback | --vertex-value-trace-gpu-readback | --vertex-nf-trace-gpu-readback | --generic-buffer-codegen-cost | --generic-buffer-numeric-readback | --generic-buffer-numeric-measurement | --eop-write-ordering-gpu | --eop-literal-ordering-gpu | --eop-literal-neighbors-gpu | --fault-buffer-protocol-gpu | --fault-buffer-protocol-neighbors-gpu | --fault-bitmap-initialization-gpu | --fault-bitmap-initialization-neighbors-gpu | --fault-first-binding-deferred-gpu | --fault-first-binding-state-neighbors-gpu]");
  if (argc == 2 && std::strcmp(argv[1], "--vertex-value-trace-gpu-readback") == 0)
    ConfigureVertexTraceGpuEnvironment();
  if (argc == 2 && (std::strcmp(argv[1], "--fault-first-binding-deferred-gpu") == 0 ||
                    std::strcmp(argv[1], "--fault-first-binding-state-neighbors-gpu") == 0))
    ConfigureFaultFirstBindingDeferredGpu();
  if (argc == 2 && std::strcmp(argv[1], "--vertex-nf-trace-gpu-readback") == 0)
    ConfigureVertexNonFiniteTraceGpuEnvironment();
  if (argc == 2 && (std::strcmp(argv[1], "--generic-buffer-codegen-cost") == 0 ||
                    std::strcmp(argv[1], "--generic-buffer-numeric-readback") == 0 ||
                    std::strcmp(argv[1], "--generic-buffer-numeric-measurement") == 0))
    ConfigureGenericBufferTests();
  Common::InitializeThreads();
  Common::Subsystems subsystems;
  subsystems.Initialize<Config::Lifecycle>();
  Config::ConfigOptions options;
  options.printf_direction = Config::LogDirection::Silent;
  options.gpu_frames_ahead = 0;
  options.label_flush_interval_us = 0;
  Config::Load(options);
  subsystems.Initialize<Log::Lifecycle>();
  subsystems.Initialize<Libs::LibKernel::Memory::Lifecycle>();
  ShaderInit();
  VulkanHarness vulkan(argc == 2 &&
      (std::strcmp(argv[1], "--fault-bitmap-initialization-gpu") == 0 ||
       std::strcmp(argv[1], "--fault-bitmap-initialization-neighbors-gpu") == 0 ||
       std::strcmp(argv[1], "--fault-first-binding-deferred-gpu") == 0 ||
       std::strcmp(argv[1], "--fault-first-binding-state-neighbors-gpu") == 0));
  if (argc == 2 && std::strcmp(argv[1], "--bindless-preparation-neighbors") == 0)
    CheckBindlessPreparationNeighbors(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--bindless-feedback-neighbors") == 0)
    CheckBindlessFeedbackNeighbors(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--bindless-image-access-age") == 0)
    CheckBindlessImageAccessAge(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--bindless-image-access-neighbors") == 0)
    CheckBindlessImageAccessNeighbors(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--bindless-descriptor-retirement-readback") == 0)
    CheckBindlessDescriptorRetirementReadback(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--bindless-descriptor-retirement-neighbors") == 0)
    CheckBindlessDescriptorRetirementNeighbors(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--embedded-fetch-gpu-readback") == 0)
    CheckEmbeddedFetchGpu(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--vertex-value-trace-gpu-readback") == 0)
    CheckVertexValueTraceGpu(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--vertex-nf-trace-gpu-readback") == 0)
    CheckVertexNonFiniteTraceGpu(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--generic-buffer-codegen-cost") == 0)
    CheckGenericBufferCodegenCost(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--generic-buffer-numeric-readback") == 0)
    CheckGenericBufferNumeric(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--generic-buffer-numeric-measurement") == 0)
    return MeasureGenericBufferNumeric(vulkan) == 0u ? 0 : 2;
  else if (argc == 2 && std::strcmp(argv[1], "--eop-write-ordering-gpu") == 0)
    CheckEndOfPipeWriteGpu(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--eop-literal-ordering-gpu") == 0)
    CheckEndOfPipeLiteralGpu(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--eop-literal-neighbors-gpu") == 0)
    CheckEndOfPipeLiteralNeighbors(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--fault-buffer-protocol-gpu") == 0)
    CheckFaultBufferProtocolGpu(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--fault-buffer-protocol-neighbors-gpu") == 0)
    CheckFaultBufferProtocolNeighborsGpu(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--fault-bitmap-initialization-gpu") == 0)
    CheckFaultBitmapInitializationGpu(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--fault-bitmap-initialization-neighbors-gpu") == 0)
    CheckFaultBitmapInitializationNeighborsGpu(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--fault-first-binding-deferred-gpu") == 0)
    CheckFaultFirstBindingDeferredGpu(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--fault-first-binding-state-neighbors-gpu") == 0)
    CheckFaultFirstBindingDeferredNeighborsGpu(vulkan);
  else
    CheckBindlessLifecycle(vulkan);
  return 0;
}
