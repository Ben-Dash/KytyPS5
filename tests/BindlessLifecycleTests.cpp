#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/pipeline/bindlessTable.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/drainStats.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/graphicsRun.h"
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

  explicit VulkanHarness(bool require_push_descriptors = false, bool require_readback_queue = false, bool require_occlusion_queries = false) {
    Init(require_push_descriptors, require_readback_queue, require_occlusion_queries);
  }
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
  void Init(bool require_push_descriptors, bool require_readback_queue, bool require_occlusion_queries) {
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
        uint32_t transfer_family = UINT32_MAX;
        if (require_readback_queue) {
          for (uint32_t candidate = 0; candidate < queues.size(); ++candidate) {
            if (candidate == index || queues[candidate].queueCount == 0 ||
                !(queues[candidate].queueFlags & vk::QueueFlagBits::eTransfer)) continue;
            transfer_family = candidate;
            if (!(queues[candidate].queueFlags &
                  (vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute))) break;
          }
          if (transfer_family == UINT32_MAX) continue;
        }
        constexpr auto flags = vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute;
        if ((queues[index].queueFlags & flags) != flags) continue;
        m_graphics.physical_device = physical;
        m_graphics.physical_device_properties = properties;
        m_graphics.queue_family = index;
        if (require_readback_queue) {
          m_graphics.readback_family = transfer_family;
          m_graphics.shared_queue_families = {index, transfer_family};
        }
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
    std::array<vk::DeviceQueueCreateInfo, 2> queue_infos{};
    queue_infos[0].queueFamilyIndex = m_graphics.queue_family;
    queue_infos[0].queueCount = 1u;
    queue_infos[0].pQueuePriorities = &priority;
    if (require_readback_queue) {
      queue_infos[1].queueFamilyIndex = m_graphics.readback_family;
      queue_infos[1].queueCount = 1u;
      queue_infos[1].pQueuePriorities = &priority;
    }
    vk::DeviceCreateInfo device{};
    // Enable only features reported supported by this selected device. These
    // are core Vulkan1.1/1.2/1.3 structures. The init mode also exercises the
    // actual parser's KHR push-descriptor path, queried separately above.
    if (require_occlusion_queries) {
      Require("NativeOcclusion", "actual queried native capabilities",
              features12.hostQueryReset == VK_TRUE && features.features.pipelineStatisticsQuery == VK_TRUE,
              "hostQueryReset/pipeline-statistics unsupported; not intended RED");
      m_graphics.host_query_reset_enabled = features12.hostQueryReset == VK_TRUE;
      m_graphics.occlusion_query_precise = features.features.occlusionQueryPrecise == VK_TRUE;
    }
    device.pNext = &features13;
    device.pEnabledFeatures = &features.features;
    device.queueCreateInfoCount = require_readback_queue ? 2u : 1u;
    device.pQueueCreateInfos = queue_infos.data();
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
    if (require_readback_queue) {
      m_graphics.device.getQueue(m_graphics.readback_family, 0u, &m_graphics.readback_queue);
      Require("ReadbackQueueBootstrap", "actual distinct queues",
          m_graphics.readback_queue != nullptr && m_graphics.readback_queue != m_graphics.queue &&
          m_graphics.readback_family != m_graphics.queue_family,
          "distinct native transfer queue unavailable; not the intended RED");
      std::printf("[host] real readback queues: graphicsFamily=%u transferFamily=%u AGE=off\n",
                  m_graphics.queue_family, m_graphics.readback_family);
    }
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
#include "BcAliasOwnershipTests.inc"
#include "FusedTiledUploadCostTests.inc"
#include "RetiredImageByteBudgetTests.inc"
#include "RetiredImageByteBudgetNeighbors.inc"
#include "RetiredImageMixedSizeTests.inc"
#include "FusedTiledUpload2ByteCapabilityTests.inc"
#include "FusedTiledUpload1080Bench.inc"
#include "FusedTiledUploadOddOffsetNeighbors.inc"
#include "FusedTiledUpload2ByteNeighbors.inc"
#include "FusedTiledUploadGpuMicrobench.inc"
#include "BcPartialBufferWriteTests.inc"
#include "BcAliasPartialTailPaddingTests.inc"
#include "CopyMipDestinationOwnershipTests.inc"
#include "NggVertexCountTests.inc"
#include "VertexValueTraceGpuTests.inc"
#include "GenericBufferCodegenTests.inc"
#include "DriverCacheNativeRoundTripTests.inc"
#include "ArrayLengthDescriptorCostTests.inc"
#include "GenericGpuMicrobenchFixture.inc"
#include "GenericChannelRepetitionTests.inc"
#include "GenericFormattedConversionPathCostTests.inc"
#include "PackedBufferOffsetTests.inc"
#include "EndOfPipeWriteGpuTests.inc"
#include "EndOfPipeLiteralGpuTests.inc"
#include "EndOfPipeLiteralNeighborTests.inc"
#include "OcclusionNativeCostTests.inc"
#include "ReadbackQueueBootstrapTests.inc"
#include "ReadbackQueuePreparedRolloverTests.inc"
#include "LiteralEopPublicationCostTests.inc"
#include "WaitRegMemNativeNonblockingCostTests.inc"
#include "WaitRegMemNativeQueuedNeighborTests.inc"
#include "ArmedReadbackWindowCostTests.inc"
#include "EagerReadbackAdjacentPagesCostTests.inc"
#include "GpuReadMemoryExactPageCostTests.inc"
#include "CpuWriteFaultWindowCostTests.inc"
#include "CpuWriteFaultWindowNeighbors.inc"
#include "CpuWriteFaultWindowBoundary.inc"
#include "GpuReadMemoryImageAliasGuards.inc"
#include "CpuWriteFaultImageWindowGuards.inc"
#include "GpuReadMemoryForeignFaultLocality.inc"
#include "GpuReadMemoryCrossOwner.inc"
#include "PollExactPageReadbackCostTests.inc"
#include "EmptyPm4TailSubmitCostTests.inc"
#include "EmptyPm4TailRequiredWorkNeighbors.inc"
#include "PrepareBdaUploadBatchCostTests.inc"
#include "BdaUploadUntouchedComputeNeighbor.inc"
#include "BdaUploadOverlapAndConsumerNeighbors.inc"
#include "PrepareBdaUploadBatchAsyncNeighbors.inc"
#include "EmptyPm4TailDiagnosticWorkTests.inc"
#include "ForeignReadbackQueuedStateCostTests.inc"
#include "ForeignReadbackQueuedArmedNeighborTests.inc"
#include "ForeignReadbackReadyAfterWaitCostTests.inc"
#include "ForeignReadbackReadyAfterWaitNeighbors.inc"
#include "ForeignReadbackReadyNewWriterNeighbor.inc"
#include "ForeignReadbackReadyAfterWaitDirtyImageGuardTests.inc"
#include "ForeignReadbackReadyAfterWaitPendingImageGuardTests.inc"
#include "ArmedReadbackTypedNeighbors.inc"
#include "WaitRegMemNativePollNeighbors.inc"
#include "LiteralEopPublicationSpanNeighbors.inc"
#include "LateBdaImageFringeTests.inc"
#include "FirstUseBdaPointerChainTests.inc"
#include "FirstUseDmaBaseTests.inc"
#include "DmaBaseOriginsTests.inc"
#include "DmaBaseRuntimeBoundsTests.inc"
#include "FaultBufferProtocolGpuTests.inc"
#include "FaultBufferProtocolNeighborTests.inc"
#include "FaultBitmapInitializationGpuTests.inc"
#include "FaultBitmapInitializationNeighborTests.inc"
#include "FaultFirstBindingDeferredGpuTests.inc"
#include "FaultFirstBindingDeferredNeighborTests.inc"
#include "DispatchSnapshotSelectorTests.inc"
#include "DispatchSnapshotJobSelectorTests.inc"
#include "NativeThreadNamesTests.inc"

#include "UserServiceGamePresetTests.inc"
#include "FunctionProfileSmokeTests.inc"
#include "CommandStreamIdleSpinCostTests.inc"
#include "RepeatedGpuDirtyStateCostTests.inc"
#include "RepeatedGpuDirtyStateNeighbors.inc"

static bool IsRepeatedGpuDirtyStateMode(const char* mode) {
  return std::strcmp(mode, "--repeated-gpu-dirty-state-cost-gpu") == 0 ||
      std::strcmp(mode, "--repeated-gpu-dirty-state-neighbors-gpu") == 0;
}

static bool IsForeignReadbackReadyMode(const char* mode) {
  return std::strcmp(mode, "--foreign-readback-ready-after-wait-cost-gpu") == 0 ||
         std::strcmp(mode, "--foreign-readback-ready-mixed-range-neighbor-gpu") == 0 ||
         std::strcmp(mode, "--foreign-readback-ready-unpublished-neighbor-gpu") == 0 ||
         std::strcmp(mode, "--foreign-readback-ready-new-writer-neighbor-gpu") == 0 ||
         std::strcmp(mode, "--foreign-readback-ready-dirty-image-neighbor-gpu") == 0 ||
         std::strcmp(mode, "--foreign-readback-ready-pending-image-neighbor-gpu") == 0;
}

int main(int argc, char **argv) {
  if (argc == 2 && IsRepeatedGpuDirtyStateMode(argv[1]))
    Libs::Graphics::ConfigureRepeatedGpuDirtyStateCostTests();
  if (argc == 2 && IsForeignReadbackReadyMode(argv[1]))
    Libs::Graphics::ConfigureArmedReadbackWindowCostTests();
  if (argc == 2 && std::strcmp(argv[1], "--cpu-write-fault-image-window-guards-gpu") == 0)
    Libs::Graphics::ConfigureCpuWriteFaultImageWindowGuards();
  if (argc == 2 && std::strcmp(argv[1], "--cpu-write-fault-window-cost-gpu") == 0)
    Libs::Graphics::ConfigureCpuWriteFaultWindowCostTests();
  if (argc == 2 && std::strcmp(argv[1], "--cpu-write-fault-window-neighbors-gpu") == 0)
    Libs::Graphics::ConfigureCpuWriteFaultWindowCostTests();
  if (argc == 2 && std::strcmp(argv[1], "--cpu-write-fault-window-boundary-gpu") == 0)
    Libs::Graphics::ConfigureCpuWriteFaultWindowCostTests();
  if (argc == 2 && (std::strcmp(argv[1], "--prepare-bda-upload-batch-cost-gpu") == 0 || std::strcmp(argv[1], "--prepare-bda-upload-batch-cost-legacy-gpu") == 0 || std::strcmp(argv[1], "--prepare-bda-upload-batch-cost-checked-gpu") == 0))
    Libs::Graphics::ConfigurePrepareBdaUploadBatchCostTests();
  if (argc == 2 && (std::strcmp(argv[1], "--empty-pm4-tail-submit-cost-gpu") == 0 ||
                    (std::strcmp(argv[1], "--prepare-bda-upload-batch-cost-gpu") == 0 || std::strcmp(argv[1], "--prepare-bda-upload-batch-cost-legacy-gpu") == 0 || std::strcmp(argv[1], "--prepare-bda-upload-batch-cost-checked-gpu") == 0) ||
                    std::strcmp(argv[1], "--empty-pm4-tail-required-work-neighbors-gpu") == 0))
    Libs::Graphics::ConfigureEmptyPm4TailSubmitCostTests();
  if (argc == 2 && (std::strcmp(argv[1], "--eager-readback-adjacent-pages-cost-gpu") == 0 || std::strcmp(argv[1], "--gpu-read-memory-exact-page-cost-gpu") == 0 || std::strcmp(argv[1], "--gpu-read-memory-foreign-fault-locality-gpu") == 0 || (std::strcmp(argv[1], "--gpu-read-memory-cross-owner-gpu") == 0 || std::strcmp(argv[1], "--gpu-read-memory-image-alias-guards-gpu") == 0)))
    Libs::Graphics::ConfigureEagerReadbackAdjacentPagesCostTests();
  if (argc == 2 && (std::strcmp(argv[1], "--poll-exact-page-readback-cost-gpu") == 0 ||
                    std::strcmp(argv[1], "--foreign-readback-queued-state-cost-gpu") == 0 ||
                    std::strcmp(argv[1], "--foreign-readback-queued-armed-neighbor-gpu") == 0))
    Libs::Graphics::ConfigureArmedReadbackWindowCostTests();
  if (argc == 2 && std::strcmp(argv[1], "--wait-reg-mem-nonblocking-cost-async-gpu") == 0)
    Libs::Graphics::Require("WaitRegMemNativeNonblockingCostAsync", "recording profile setup",
        _putenv_s("KYTY_RECORD_THREAD", "1") == 0,
        "could not pin deferred recording before configuration and native hooks");
  if (argc == 2 && (std::strcmp(argv[1], "--poll-exact-page-readback-cost-gpu") == 0 ||
                    std::strcmp(argv[1], "--wait-reg-mem-notification-lifetime-gpu") == 0 ||
                    std::strcmp(argv[1], "--wait-reg-mem-ready-continuation-cost-gpu") == 0 ||
                    std::strcmp(argv[1], "--wait-reg-mem-poll-neighbors-gpu") == 0 ||
                    std::strcmp(argv[1], "--wait-reg-mem-nonblocking-cost-gpu") == 0 ||
                    std::strcmp(argv[1], "--wait-reg-mem-queued-neighbor-gpu") == 0))
    Libs::Graphics::Require("WaitRegMemNativeNonblockingCost", "direct-record profile setup",
        _putenv_s("KYTY_RECORD_THREAD", "0") == 0, "could not pin direct recording before getters");
  if (argc == 2 && (std::strcmp(argv[1], "--retired-image-byte-budget-gpu") == 0 || (std::strcmp(argv[1], "--retired-image-byte-budget-neighbors-gpu") == 0 || std::strcmp(argv[1], "--retired-image-mixed-size-gpu") == 0))) {
    Libs::Graphics::Require("RetiredImageByteBudget","standalone process configuration",
        _putenv_s("KYTY_RECORD_THREAD","0")==0&&_putenv_s("KYTY_IMAGE_RECYCLE","1")==0,
        "direct recording/recycle configuration failed before native setup");
  }
  if (argc == 2 && (std::strcmp(argv[1], "--fused-tiled-upload-cost-gpu") == 0 ||
                    std::strcmp(argv[1], "--fused-tiled-upload-2byte-cost-gpu") == 0 ||
                    std::strcmp(argv[1], "--fused-tiled-upload-2byte-capability-gpu") == 0 ||
                    std::strcmp(argv[1], "--fused-tiled-upload-2byte-neighbors-gpu") == 0 ||
                    std::strcmp(argv[1], "--fused-tiled-upload-2byte-1080-pair-gpu") == 0 ||
                    std::strcmp(argv[1], "--fused-tiled-upload-1080-pair-gpu") == 0 ||
                    std::strcmp(argv[1], "--fused-tiled-upload-odd-offset-gpu") == 0 ||
                   std::strcmp(argv[1], "--fused-tiled-upload-gpu-microbench") == 0))
    Libs::Graphics::ConfigureFusedTiledUploadTests();
  if (argc == 2 && (std::strcmp(argv[1], "--user-game-preset-priority-only") == 0 ||
                   std::strcmp(argv[1], "--user-game-preset-users-only") == 0)) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Common::InitializeThreads();
    Common::Subsystems subsystems;
    subsystems.Initialize<Config::Lifecycle>();
    Config::ConfigOptions options;
    options.printf_direction = Config::LogDirection::Silent;
    Config::Load(options);
    subsystems.Initialize<Log::Lifecycle>();
    if (std::strcmp(argv[1], "--user-game-preset-users-only") == 0) {
      for (const int32_t user : {0, 1001}) {
        options.user_id = user;
        Libs::Graphics::CheckUserServiceGamePresetPriority(options);
      }
    } else {
      Libs::Graphics::CheckUserServiceGamePresetPriority(options);
    }
    return 0;
  }
  const bool drain_stats_mode = argc == 3 && std::strcmp(argv[1], "--drain-stats") == 0;
  if (drain_stats_mode) { --argc; ++argv; }
  struct DrainStatsScope {
    bool previous;
    explicit DrainStatsScope(bool enabled)
        : previous(Libs::Graphics::DrainStats::g_enabled.load(std::memory_order_relaxed)) {
      if (enabled) Libs::Graphics::DrainStats::g_enabled.store(true, std::memory_order_relaxed);
    }
    ~DrainStatsScope() {
      Libs::Graphics::DrainStats::g_enabled.store(previous, std::memory_order_relaxed);
    }
  } drain_stats_scope(drain_stats_mode);
  using namespace Libs::Graphics;
  const bool readback_queue_mode = argc == 2 &&
      (std::strcmp(argv[1], "--readback-queue-ageoff-gpu") == 0 ||
       std::strcmp(argv[1], "--readback-queue-prepared-rollover-gpu") == 0);
  if (readback_queue_mode) ConfigureReadbackQueueBootstrapTests();
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc == 2 && std::strcmp(argv[1], "--function-profile-smoke") == 0) {
    CheckFunctionProfileSmoke();
    return 0;
  }
  if (argc == 2 && (std::strcmp(argv[1], "--command-stream-idle-spin-cost") == 0 ||
                    std::strcmp(argv[1], "--command-stream-idle-spin-neighbors") == 0)) {
    Common::InitializeThreads();
    CheckCommandStreamIdleSpinCost(std::strcmp(argv[1], "--command-stream-idle-spin-cost") == 0);
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--native-thread-names-only") == 0) {
    CheckNativeThreadNamesWithoutProfiler();
    return 0;
  }
  Require("BindlessLifecycle", "arguments", argc == 1 ||
              (argc == 2 && (std::strcmp(argv[1], "--bindless-lifecycle-only") == 0 ||
       IsForeignReadbackReadyMode(argv[1]) ||
       IsRepeatedGpuDirtyStateMode(argv[1]) ||
                            std::strcmp(argv[1], "--cpu-write-fault-window-cost-gpu") == 0 ||
                            std::strcmp(argv[1], "--cpu-write-fault-window-neighbors-gpu") == 0 ||
                            std::strcmp(argv[1], "--cpu-write-fault-window-boundary-gpu") == 0 ||
                            std::strcmp(argv[1], "--cpu-write-fault-image-window-guards-gpu") == 0 ||
                            std::strcmp(argv[1], "--occlusion-native-cost-gpu") == 0 ||
                            std::strcmp(argv[1], "--wait-reg-mem-nonblocking-cost-gpu") == 0 ||
                            std::strcmp(argv[1], "--poll-exact-page-readback-cost-gpu") == 0 ||
                            (std::strcmp(argv[1], "--eager-readback-adjacent-pages-cost-gpu") == 0 || std::strcmp(argv[1], "--gpu-read-memory-exact-page-cost-gpu") == 0 || std::strcmp(argv[1], "--gpu-read-memory-foreign-fault-locality-gpu") == 0 || (std::strcmp(argv[1], "--gpu-read-memory-cross-owner-gpu") == 0 || std::strcmp(argv[1], "--gpu-read-memory-image-alias-guards-gpu") == 0)) ||
                            std::strcmp(argv[1], "--empty-pm4-tail-submit-cost-gpu") == 0 ||
                    (std::strcmp(argv[1], "--prepare-bda-upload-batch-cost-gpu") == 0 || std::strcmp(argv[1], "--prepare-bda-upload-batch-cost-legacy-gpu") == 0 || std::strcmp(argv[1], "--prepare-bda-upload-batch-cost-checked-gpu") == 0) ||
                            std::strcmp(argv[1], "--empty-pm4-tail-required-work-neighbors-gpu") == 0 ||
                    std::strcmp(argv[1], "--bda-upload-overlap-consumer-gpu") == 0 ||
                    std::strcmp(argv[1], "--prepare-bda-upload-untouched-compute-gpu") == 0 ||
                    std::strcmp(argv[1], "--prepare-bda-upload-async-neighbors-gpu") == 0 ||
                            std::strcmp(argv[1], "--empty-pm4-tail-diagnostic-work-gpu") == 0 ||
                            std::strcmp(argv[1], "--foreign-readback-queued-state-cost-gpu") == 0 ||
                            std::strcmp(argv[1], "--foreign-readback-queued-armed-neighbor-gpu") == 0 ||
                            std::strcmp(argv[1], "--wait-reg-mem-nonblocking-cost-async-gpu") == 0 ||
                            std::strcmp(argv[1], "--wait-reg-mem-poll-neighbors-gpu") == 0 ||
                            std::strcmp(argv[1], "--wait-reg-mem-queued-neighbor-gpu") == 0 ||
                            std::strcmp(argv[1], "--wait-reg-mem-notification-lifetime-gpu") == 0 ||
                            std::strcmp(argv[1], "--wait-reg-mem-ready-continuation-cost-gpu") == 0 ||
                            (std::strcmp(argv[1], "--retired-image-byte-budget-gpu") == 0 || (std::strcmp(argv[1], "--retired-image-byte-budget-neighbors-gpu") == 0 || std::strcmp(argv[1], "--retired-image-mixed-size-gpu") == 0)) ||
                            std::strcmp(argv[1], "--fused-tiled-upload-cost-gpu") == 0 ||
                    std::strcmp(argv[1], "--fused-tiled-upload-2byte-cost-gpu") == 0 ||
                    std::strcmp(argv[1], "--fused-tiled-upload-2byte-capability-gpu") == 0 ||
                    std::strcmp(argv[1], "--fused-tiled-upload-2byte-neighbors-gpu") == 0 ||
                    std::strcmp(argv[1], "--fused-tiled-upload-2byte-1080-pair-gpu") == 0 ||
                    std::strcmp(argv[1], "--fused-tiled-upload-1080-pair-gpu") == 0 ||
                    std::strcmp(argv[1], "--fused-tiled-upload-odd-offset-gpu") == 0 ||
                            std::strcmp(argv[1], "--fused-tiled-upload-gpu-microbench") == 0 ||
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
                    std::strcmp(argv[1], "--driver-cache-native-roundtrip") == 0 ||
                    std::strcmp(argv[1], "--generic-channel-repetition-cost") == 0 ||
                    std::strcmp(argv[1], "--generic-channel-repetition-neighbors") == 0 ||
                            std::strcmp(argv[1], "--generic-formatted-conversion-path-cost") == 0 ||
                            std::strcmp(argv[1], "--generic-gpu-microbench") == 0 ||
                            std::strcmp(argv[1], "--array-length-descriptor-cost") == 0 ||
                            std::strcmp(argv[1], "--generic-formatted-dispatch-decision-cost") == 0 ||
                            std::strcmp(argv[1], "--generic-buffer-numeric-readback") == 0 ||
                            std::strcmp(argv[1], "--generic-buffer-numeric-measurement") == 0 ||
                            std::strcmp(argv[1], "--formatted-buffer-static-numeric-measurement") == 0 ||
                            std::strcmp(argv[1], "--readback-queue-ageoff-gpu") == 0 ||
                            std::strcmp(argv[1], "--readback-queue-prepared-rollover-gpu") == 0 ||
                            std::strcmp(argv[1], "--armed-readback-window-cost-gpu") == 0 ||
                            std::strcmp(argv[1], "--armed-readback-typed-neighbors-gpu") == 0 ||
                            std::strcmp(argv[1], "--eop-write-ordering-gpu") == 0 ||
                            std::strcmp(argv[1], "--eop-literal-ordering-gpu") == 0 ||
                            std::strcmp(argv[1], "--eop-literal-neighbors-gpu") == 0 ||
                            std::strcmp(argv[1], "--eop-literal-publication-cost") == 0 ||
                            std::strcmp(argv[1], "--eop-literal-publication-span-neighbors") == 0 ||
                            std::strcmp(argv[1], "--fault-buffer-protocol-gpu") == 0 ||
                            std::strcmp(argv[1], "--fault-buffer-protocol-neighbors-gpu") == 0 ||
                            std::strcmp(argv[1], "--fault-bitmap-initialization-gpu") == 0 ||
                            std::strcmp(argv[1], "--fault-bitmap-initialization-neighbors-gpu") == 0 ||
                            std::strcmp(argv[1], "--fault-first-binding-deferred-gpu") == 0 ||
                            std::strcmp(argv[1], "--fault-first-binding-state-neighbors-gpu") == 0 ||
                            std::strcmp(argv[1], "--dispatch-snapshot-selector-only") == 0 ||
                            std::strcmp(argv[1], "--dispatch-snapshot-selector-frame-only") == 0 ||
                            std::strcmp(argv[1], "--dispatch-snapshot-selector-default-only") == 0 ||
                            std::strcmp(argv[1], "--dispatch-snapshot-job-selector-only") == 0 ||
                            std::strcmp(argv[1], "--dispatch-snapshot-job-selector-frame-only") == 0 ||
                            std::strcmp(argv[1], "--dispatch-snapshot-job-selector-default-only") == 0 ||
                            std::strcmp(argv[1], "--dispatch-snapshot-job-inventory-only") == 0 ||
                            std::strcmp(argv[1], "--dispatch-snapshot-job-inventory-frame-only") == 0 ||
                            std::strcmp(argv[1], "--dispatch-snapshot-job-inventory-default-only") == 0 ||
                            std::strcmp(argv[1], "--dispatch-snapshot-job-inventory-cap-only") == 0 ||
                            std::strcmp(argv[1], "--late-bda-image-fringe-gpu") == 0 ||
                            std::strcmp(argv[1], "--first-use-bda-pointer-chain-gpu") == 0 ||
                            std::strcmp(argv[1], "--first-use-bda-pointer-chain-neighbors-gpu") == 0 ||
                            std::strcmp(argv[1], "--first-use-dma-base-gpu") == 0 ||
                            std::strcmp(argv[1], "--first-use-dma-base-neighbors-gpu") == 0 ||
                            std::strcmp(argv[1], "--bc-alias-ownership-gpu") == 0 ||
                            std::strcmp(argv[1], "--bc-alias-ownership-neighbors-gpu") == 0 ||
                            std::strcmp(argv[1], "--copy-mip-destination-gpu") == 0 ||
                            std::strcmp(argv[1], "--copy-mip-destination-neighbors-gpu") == 0 ||
                            std::strcmp(argv[1], "--bc-alias-tail-padding-gpu") == 0 ||
                            std::strcmp(argv[1], "--dma-base-origins-only") == 0 ||
                            std::strcmp(argv[1], "--dma-base-runtime-bounds") == 0 ||
                            std::strcmp(argv[1], "--bc-partial-buffer-gpu") == 0 ||
                            std::strcmp(argv[1], "--bc-partial-buffer-neighbors-gpu") == 0 ||
                            std::strcmp(argv[1], "--bc-partial-buffer-flag-neighbors-gpu") == 0 ||
                            std::strcmp(argv[1], "--packed-buffer-offset-cost") == 0 ||
                            std::strcmp(argv[1], "--ngg-vertex-count-gpu") == 0 ||
                            std::strcmp(argv[1], "--packed-buffer-offset-gpu") == 0)),
          "usage: bindless_lifecycle_tests [--bindless-lifecycle-only | --bindless-preparation-neighbors | --bindless-feedback-neighbors | --bindless-image-access-age | --bindless-image-access-neighbors | --bindless-descriptor-retirement-readback | --bindless-descriptor-retirement-neighbors | --embedded-fetch-gpu-readback | --vertex-value-trace-gpu-readback | --vertex-nf-trace-gpu-readback | --generic-buffer-codegen-cost | --generic-buffer-numeric-readback | --generic-buffer-numeric-measurement | --formatted-buffer-static-numeric-measurement | --readback-queue-ageoff-gpu | --eop-write-ordering-gpu | --eop-literal-ordering-gpu | --eop-literal-neighbors-gpu | --eop-literal-publication-cost | --late-bda-image-fringe-gpu | --first-use-bda-pointer-chain-gpu | --first-use-bda-pointer-chain-neighbors-gpu | --fault-buffer-protocol-gpu | --fault-buffer-protocol-neighbors-gpu | --fault-bitmap-initialization-gpu | --fault-bitmap-initialization-neighbors-gpu | --fault-first-binding-deferred-gpu | --fault-first-binding-state-neighbors-gpu | --dispatch-snapshot-selector-only | --dispatch-snapshot-selector-frame-only | --dispatch-snapshot-selector-default-only | --packed-buffer-offset-cost | --packed-buffer-offset-gpu | --ngg-vertex-count-gpu | --dispatch-snapshot-job-selector-only | --dispatch-snapshot-job-selector-frame-only | --dispatch-snapshot-job-selector-default-only | --dispatch-snapshot-job-inventory-only | --dispatch-snapshot-job-inventory-frame-only | --dispatch-snapshot-job-inventory-default-only | --dispatch-snapshot-job-inventory-cap-only]");
  if (argc == 2 && (std::strcmp(argv[1], "--dispatch-snapshot-job-inventory-only") == 0 ||
                    std::strcmp(argv[1], "--dispatch-snapshot-job-inventory-frame-only") == 0 ||
                    std::strcmp(argv[1], "--dispatch-snapshot-job-inventory-default-only") == 0 ||
                    std::strcmp(argv[1], "--dispatch-snapshot-job-inventory-cap-only") == 0)) {
    CheckDispatchSnapshotJobSelector(
        std::strcmp(argv[1], "--dispatch-snapshot-job-inventory-frame-only") == 0,
        std::strcmp(argv[1], "--dispatch-snapshot-job-inventory-default-only") == 0,
        std::strcmp(argv[1], "--dispatch-snapshot-job-inventory-cap-only") == 0 ? 2u : 256u);
    return 0;
  }
  if (argc == 2 && (std::strcmp(argv[1], "--dispatch-snapshot-job-selector-only") == 0 ||
                    std::strcmp(argv[1], "--dispatch-snapshot-job-selector-frame-only") == 0 ||
                    std::strcmp(argv[1], "--dispatch-snapshot-job-selector-default-only") == 0)) {
    CheckDispatchSnapshotJobSelector(
        std::strcmp(argv[1], "--dispatch-snapshot-job-selector-frame-only") == 0,
        std::strcmp(argv[1], "--dispatch-snapshot-job-selector-default-only") == 0);
    return 0;
  }
  if (argc == 2 && (std::strcmp(argv[1], "--dispatch-snapshot-selector-only") == 0 ||
                    std::strcmp(argv[1], "--dispatch-snapshot-selector-frame-only") == 0 ||
                    std::strcmp(argv[1], "--dispatch-snapshot-selector-default-only") == 0)) {
    CheckDispatchSnapshotSelector(std::strcmp(argv[1], "--dispatch-snapshot-selector-frame-only") == 0,
                                  std::strcmp(argv[1], "--dispatch-snapshot-selector-default-only") == 0);
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--vertex-value-trace-gpu-readback") == 0)
    ConfigureVertexTraceGpuEnvironment();
  if (argc == 2 && (std::strcmp(argv[1], "--packed-buffer-offset-cost") == 0 ||
                    std::strcmp(argv[1], "--packed-buffer-offset-gpu") == 0))
    ConfigurePackedBufferOffsetTests();
  if (argc == 2 && std::strcmp(argv[1], "--ngg-vertex-count-gpu") == 0)
    ConfigureNggVertexCountTests();
  if (argc == 2 && std::strcmp(argv[1], "--late-bda-image-fringe-gpu") == 0)
    ConfigureLateBdaImageFringeTests();
  if (argc == 2 && (std::strcmp(argv[1], "--first-use-bda-pointer-chain-gpu") == 0 ||
                   std::strcmp(argv[1], "--first-use-bda-pointer-chain-neighbors-gpu") == 0))
    ConfigureFirstUseBdaPointerChainTests();
  if (argc == 2 && (std::strcmp(argv[1], "--first-use-dma-base-gpu") == 0 ||
                   std::strcmp(argv[1], "--first-use-dma-base-neighbors-gpu") == 0))
    ConfigureFirstUseDmaBaseTests();
  if (argc == 2 && (std::strcmp(argv[1], "--bc-alias-ownership-gpu") == 0 ||
                   std::strcmp(argv[1], "--bc-alias-ownership-neighbors-gpu") == 0 ||
                   std::strcmp(argv[1], "--copy-mip-destination-gpu") == 0 ||
                   std::strcmp(argv[1], "--copy-mip-destination-neighbors-gpu") == 0 ||
                   std::strcmp(argv[1], "--bc-alias-tail-padding-gpu") == 0))
    ConfigureBcAliasOwnershipTests();
  if (argc == 2 && (std::strcmp(argv[1], "--bc-partial-buffer-gpu") == 0 ||
                   std::strcmp(argv[1], "--bc-partial-buffer-neighbors-gpu") == 0 ||
                   std::strcmp(argv[1], "--bc-partial-buffer-flag-neighbors-gpu") == 0))
    ConfigureBcPartialBufferWriteTests(
        std::strcmp(argv[1], "--bc-partial-buffer-flag-neighbors-gpu") == 0);
  if (argc == 2 && (std::strcmp(argv[1], "--fault-first-binding-deferred-gpu") == 0 ||
                    std::strcmp(argv[1], "--fault-first-binding-state-neighbors-gpu") == 0))
    ConfigureFaultFirstBindingDeferredGpu();
  if (argc == 2 && std::strcmp(argv[1], "--vertex-nf-trace-gpu-readback") == 0)
    ConfigureVertexNonFiniteTraceGpuEnvironment();
  if (argc == 2 && std::strcmp(argv[1], "--armed-readback-window-cost-gpu") == 0)
    ConfigureArmedReadbackWindowCostTests();
  if (argc == 2 && std::strcmp(argv[1], "--wait-reg-mem-poll-neighbors-gpu") == 0)
    ConfigureArmedReadbackTypedNeighbors();
  if (argc == 2 && std::strcmp(argv[1], "--armed-readback-typed-neighbors-gpu") == 0)
    ConfigureArmedReadbackTypedNeighbors();
  if (argc == 2 && (std::strcmp(argv[1], "--generic-buffer-codegen-cost") == 0 ||
                    std::strcmp(argv[1], "--driver-cache-native-roundtrip") == 0 ||
                    std::strcmp(argv[1], "--generic-channel-repetition-cost") == 0 ||
                    std::strcmp(argv[1], "--generic-channel-repetition-neighbors") == 0 ||
                    std::strcmp(argv[1], "--generic-formatted-conversion-path-cost") == 0 ||
                    std::strcmp(argv[1], "--generic-gpu-microbench") == 0 ||
                    std::strcmp(argv[1], "--array-length-descriptor-cost") == 0 ||
                    std::strcmp(argv[1], "--generic-formatted-dispatch-decision-cost") == 0 ||
                    std::strcmp(argv[1], "--generic-buffer-numeric-readback") == 0 ||
                    std::strcmp(argv[1], "--generic-buffer-numeric-measurement") == 0))
    ConfigureGenericBufferTests();
  if (argc == 2 && std::strcmp(argv[1], "--formatted-buffer-static-numeric-measurement") == 0)
    ConfigureGenericBufferTests(true);
  if (argc == 2 && std::strcmp(argv[1], "--empty-pm4-tail-diagnostic-work-gpu") == 0)
    Libs::Graphics::ConfigureEmptyPm4TailDiagnosticWorkTests();
  if (argc == 2 && std::strcmp(argv[1], "--bda-upload-overlap-consumer-gpu") == 0)
    Libs::Graphics::ConfigurePrepareBdaUploadBatchCostTests();
  if (argc == 2 && std::strcmp(argv[1], "--prepare-bda-upload-untouched-compute-gpu") == 0)
    Libs::Graphics::ConfigurePrepareBdaUploadBatchCostTests();
  if (argc == 2 && std::strcmp(argv[1], "--prepare-bda-upload-async-neighbors-gpu") == 0)
    Libs::Graphics::ConfigurePrepareBdaUploadBatchAsyncNeighbors();
  Common::InitializeThreads();
  Common::Subsystems subsystems;
  subsystems.Initialize<Config::Lifecycle>();
  Config::ConfigOptions options;
  options.printf_direction = Config::LogDirection::Silent;
  options.gpu_frames_ahead = 0;
  options.label_flush_interval_us = 0;
  if (argc == 2 && std::strcmp(argv[1], "--prepare-bda-upload-batch-cost-legacy-gpu") == 0)
    options.bda_sync_mode = Config::BdaSyncMode::Legacy;
  if (argc == 2 && std::strcmp(argv[1], "--prepare-bda-upload-batch-cost-checked-gpu") == 0)
    options.bda_sync_mode = Config::BdaSyncMode::SelectiveChecked;
  Config::Load(options);
  subsystems.Initialize<Log::Lifecycle>();
  subsystems.Initialize<Libs::LibKernel::Memory::Lifecycle>();
  ShaderInit();
  VulkanHarness vulkan(argc == 2 &&
      (std::strcmp(argv[1], "--poll-exact-page-readback-cost-gpu") == 0 ||
       IsForeignReadbackReadyMode(argv[1]) ||
       IsRepeatedGpuDirtyStateMode(argv[1]) ||
       std::strcmp(argv[1], "--cpu-write-fault-window-cost-gpu") == 0 ||
       std::strcmp(argv[1], "--cpu-write-fault-window-neighbors-gpu") == 0 ||
       std::strcmp(argv[1], "--cpu-write-fault-window-boundary-gpu") == 0 ||
       std::strcmp(argv[1], "--cpu-write-fault-image-window-guards-gpu") == 0 ||
       (std::strcmp(argv[1], "--eager-readback-adjacent-pages-cost-gpu") == 0 || std::strcmp(argv[1], "--gpu-read-memory-exact-page-cost-gpu") == 0 || std::strcmp(argv[1], "--gpu-read-memory-foreign-fault-locality-gpu") == 0 || (std::strcmp(argv[1], "--gpu-read-memory-cross-owner-gpu") == 0 || std::strcmp(argv[1], "--gpu-read-memory-image-alias-guards-gpu") == 0)) ||
       std::strcmp(argv[1], "--empty-pm4-tail-submit-cost-gpu") == 0 ||
                    (std::strcmp(argv[1], "--prepare-bda-upload-batch-cost-gpu") == 0 || std::strcmp(argv[1], "--prepare-bda-upload-batch-cost-legacy-gpu") == 0 || std::strcmp(argv[1], "--prepare-bda-upload-batch-cost-checked-gpu") == 0) ||
       std::strcmp(argv[1], "--empty-pm4-tail-required-work-neighbors-gpu") == 0 ||
                    std::strcmp(argv[1], "--bda-upload-overlap-consumer-gpu") == 0 ||
                    std::strcmp(argv[1], "--prepare-bda-upload-untouched-compute-gpu") == 0 ||
                    std::strcmp(argv[1], "--prepare-bda-upload-async-neighbors-gpu") == 0 ||
       std::strcmp(argv[1], "--empty-pm4-tail-diagnostic-work-gpu") == 0 ||
       std::strcmp(argv[1], "--wait-reg-mem-notification-lifetime-gpu") == 0 ||
       std::strcmp(argv[1], "--wait-reg-mem-ready-continuation-cost-gpu") == 0 ||
       std::strcmp(argv[1], "--foreign-readback-queued-state-cost-gpu") == 0 ||
       std::strcmp(argv[1], "--foreign-readback-queued-armed-neighbor-gpu") == 0 ||
       std::strcmp(argv[1], "--wait-reg-mem-nonblocking-cost-gpu") == 0 ||
       std::strcmp(argv[1], "--wait-reg-mem-nonblocking-cost-async-gpu") == 0 ||
       std::strcmp(argv[1], "--wait-reg-mem-poll-neighbors-gpu") == 0 ||
       std::strcmp(argv[1], "--wait-reg-mem-queued-neighbor-gpu") == 0 ||
       (std::strcmp(argv[1], "--retired-image-byte-budget-gpu") == 0 || (std::strcmp(argv[1], "--retired-image-byte-budget-neighbors-gpu") == 0 || std::strcmp(argv[1], "--retired-image-mixed-size-gpu") == 0)) ||

       std::strcmp(argv[1], "--fused-tiled-upload-cost-gpu") == 0 ||
                    std::strcmp(argv[1], "--fused-tiled-upload-2byte-cost-gpu") == 0 ||
                    std::strcmp(argv[1], "--fused-tiled-upload-2byte-capability-gpu") == 0 ||
                    std::strcmp(argv[1], "--fused-tiled-upload-2byte-neighbors-gpu") == 0 ||
                    std::strcmp(argv[1], "--fused-tiled-upload-2byte-1080-pair-gpu") == 0 ||
                    std::strcmp(argv[1], "--fused-tiled-upload-1080-pair-gpu") == 0 ||
                    std::strcmp(argv[1], "--fused-tiled-upload-odd-offset-gpu") == 0 ||
       std::strcmp(argv[1], "--fused-tiled-upload-gpu-microbench") == 0 ||
       std::strcmp(argv[1], "--fault-bitmap-initialization-gpu") == 0 ||
       std::strcmp(argv[1], "--fault-bitmap-initialization-neighbors-gpu") == 0 ||
       std::strcmp(argv[1], "--fault-first-binding-deferred-gpu") == 0 ||
       std::strcmp(argv[1], "--fault-first-binding-state-neighbors-gpu") == 0 ||
       std::strcmp(argv[1], "--late-bda-image-fringe-gpu") == 0 ||
       std::strcmp(argv[1], "--first-use-bda-pointer-chain-gpu") == 0 ||
       std::strcmp(argv[1], "--first-use-bda-pointer-chain-neighbors-gpu") == 0 ||
       std::strcmp(argv[1], "--first-use-dma-base-gpu") == 0 ||
       std::strcmp(argv[1], "--first-use-dma-base-neighbors-gpu") == 0 ||
       std::strcmp(argv[1], "--bc-alias-ownership-gpu") == 0 ||
       std::strcmp(argv[1], "--bc-alias-ownership-neighbors-gpu") == 0 ||
       std::strcmp(argv[1], "--copy-mip-destination-gpu") == 0 ||
       std::strcmp(argv[1], "--copy-mip-destination-neighbors-gpu") == 0 ||
       std::strcmp(argv[1], "--bc-alias-tail-padding-gpu") == 0 ||
       std::strcmp(argv[1], "--bc-partial-buffer-gpu") == 0 ||
       std::strcmp(argv[1], "--bc-partial-buffer-neighbors-gpu") == 0 ||
       std::strcmp(argv[1], "--bc-partial-buffer-flag-neighbors-gpu") == 0), readback_queue_mode,
      argc == 2 && std::strcmp(argv[1], "--occlusion-native-cost-gpu") == 0);
  if (argc == 2 && (std::strcmp(argv[1], "--retired-image-byte-budget-gpu") == 0 || (std::strcmp(argv[1], "--retired-image-byte-budget-neighbors-gpu") == 0 || std::strcmp(argv[1], "--retired-image-mixed-size-gpu") == 0))) {
    auto& renderer=vulkan.RuntimeRenderer();renderer.InitializeGpu(nullptr);
    renderer.GetGpu().SendCommandSync([&]{
      if (std::strcmp(argv[1], "--retired-image-mixed-size-gpu") == 0)
        CheckRetiredImageMixedSize(vulkan);
      else if (std::strcmp(argv[1], "--retired-image-byte-budget-neighbors-gpu") == 0)
        CheckRetiredImageByteBudgetNeighbors(vulkan);
      else
        CheckRetiredImageByteBudget(vulkan);
    });
    renderer.ShutdownGpu();return 0;
  }
  if (argc == 2 && (std::strcmp(argv[1], "--fused-tiled-upload-cost-gpu") == 0 ||
                    std::strcmp(argv[1], "--fused-tiled-upload-2byte-cost-gpu") == 0 ||
                    std::strcmp(argv[1], "--fused-tiled-upload-2byte-capability-gpu") == 0 ||
                    std::strcmp(argv[1], "--fused-tiled-upload-2byte-neighbors-gpu") == 0 ||
                    std::strcmp(argv[1], "--fused-tiled-upload-2byte-1080-pair-gpu") == 0 ||
                    std::strcmp(argv[1], "--fused-tiled-upload-1080-pair-gpu") == 0 ||
                    std::strcmp(argv[1], "--fused-tiled-upload-odd-offset-gpu") == 0 ||
                    std::strcmp(argv[1], "--fused-tiled-upload-gpu-microbench") == 0)) {
    auto& renderer = vulkan.RuntimeRenderer();
    renderer.InitializeGpu(nullptr);
    renderer.GetGpu().SendCommandSync([&] {
      if (std::strcmp(argv[1], "--fused-tiled-upload-2byte-1080-pair-gpu") == 0)
        CheckFusedTiledUpload2Byte1080Bench(vulkan);
      else if (std::strcmp(argv[1], "--fused-tiled-upload-1080-pair-gpu") == 0)
        CheckFusedTiledUpload1080Bench(vulkan);
      else if (std::strcmp(argv[1], "--fused-tiled-upload-odd-offset-gpu") == 0)
        CheckFusedTiledUploadOddOffsetNeighbors(vulkan);
      else if (std::strcmp(argv[1], "--fused-tiled-upload-2byte-neighbors-gpu") == 0)
        CheckFusedTiledUpload2ByteNeighbors(vulkan);
      else if (std::strcmp(argv[1], "--fused-tiled-upload-2byte-capability-gpu") == 0)
        CheckFusedTiledUpload2ByteCapability(vulkan);
      else if (std::strcmp(argv[1], "--fused-tiled-upload-2byte-cost-gpu") == 0)
        CheckFusedTiledUpload2ByteCost(vulkan);
      else if (std::strcmp(argv[1], "--fused-tiled-upload-cost-gpu") == 0)
        CheckFusedTiledUploadCost(vulkan);
      else
        CheckFusedTiledUploadGpuMicrobench(vulkan);
    });
    renderer.ShutdownGpu();
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--wait-reg-mem-ready-continuation-cost-gpu") == 0) {
    auto& renderer = vulkan.RuntimeRenderer();
    renderer.InitializeGpu(nullptr);
    CheckWaitRegMemNativeQueuedNeighbor(vulkan, true);
    renderer.ShutdownGpu();
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--wait-reg-mem-notification-lifetime-gpu") == 0) {
    auto& renderer = vulkan.RuntimeRenderer();
    renderer.InitializeGpu(nullptr);
    std::shared_ptr<GuestGpu::ReadbackNotification> retained;
    renderer.GetGpu().SendCommandSync([&] {
      retained=GuestGpu::CaptureReadbackNotification();
      Require("WaitRegMemNotificationLifetime", "actual dispatcher token", retained!=nullptr,
          "capture outside the real dispatcher is not a lifetime control");
      // Exact finite native kernel, scalar, protected publication, PM4 values,
      // masks, guards and owned mapping cleanup precede the lifetime boundary.
      CheckWaitRegMemNativeNonblockingCost(vulkan);
    });
    retained->Notify();
    renderer.ShutdownGpu(); // joins and destroys the actual GuestGpu
    retained->Notify();    // retained callback token must be harmless after destruction
    retained.reset();
    std::printf("[wait-token] nativeControls=PASS notifyBeforeShutdown=PASS notifyAfterDestroyedGuestGpu=PASS tokenReleased=PASS\n");
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--wait-reg-mem-queued-neighbor-gpu") == 0) {
    auto& renderer = vulkan.RuntimeRenderer();
    renderer.InitializeGpu(nullptr);
    CheckWaitRegMemNativeQueuedNeighbor(vulkan);
    renderer.ShutdownGpu();
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--wait-reg-mem-poll-neighbors-gpu") == 0) {
    CheckWaitRegMemNativePollNeighbors(vulkan);
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--wait-reg-mem-nonblocking-cost-async-gpu") == 0) {
    auto& renderer = vulkan.RuntimeRenderer();
    renderer.InitializeGpu(nullptr);
    renderer.GetGpu().SendCommandSync([&] { CheckWaitRegMemNativeNonblockingCost(vulkan, true); });
    renderer.ShutdownGpu();
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--wait-reg-mem-nonblocking-cost-gpu") == 0) {
    auto& renderer = vulkan.RuntimeRenderer();
    renderer.InitializeGpu(nullptr);
    renderer.GetGpu().SendCommandSync([&] { CheckWaitRegMemNativeNonblockingCost(vulkan); });
    renderer.ShutdownGpu();
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--occlusion-native-cost-gpu") == 0) {
    CheckOcclusionNativeCost(vulkan);
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--poll-exact-page-readback-cost-gpu") == 0) {
    CheckPollExactPageReadbackCost(vulkan);
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--gpu-read-memory-image-alias-guards-gpu") == 0) {
    CheckGpuReadMemoryImageAliasGuards(vulkan);
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--cpu-write-fault-window-cost-gpu") == 0) {
    CheckCpuWriteFaultWindowCost(vulkan);
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--cpu-write-fault-image-window-guards-gpu") == 0) {
    CheckCpuWriteFaultImageWindowGuards(vulkan);
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--cpu-write-fault-window-neighbors-gpu") == 0) {
    CheckCpuWriteFaultWindowNeighbors(vulkan);
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--cpu-write-fault-window-boundary-gpu") == 0) {
    CheckCpuWriteFaultWindowBoundary(vulkan);
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--gpu-read-memory-exact-page-cost-gpu") == 0) {
    CheckGpuReadMemoryExactPageCost(vulkan);
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--gpu-read-memory-cross-owner-gpu") == 0) {
    CheckGpuReadMemoryCrossOwner(vulkan);
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--gpu-read-memory-foreign-fault-locality-gpu") == 0) {
    CheckGpuReadMemoryForeignFaultLocality(vulkan);
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--eager-readback-adjacent-pages-cost-gpu") == 0) {
    CheckEagerReadbackAdjacentPagesCost(vulkan);
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--empty-pm4-tail-diagnostic-work-gpu") == 0) {
    CheckEmptyPm4TailDiagnosticWork(vulkan);
    return 0;
  }
  if (argc == 2 && (std::strcmp(argv[1], "--prepare-bda-upload-batch-cost-gpu") == 0 || std::strcmp(argv[1], "--prepare-bda-upload-batch-cost-legacy-gpu") == 0 || std::strcmp(argv[1], "--prepare-bda-upload-batch-cost-checked-gpu") == 0)) {
    CheckPrepareBdaUploadBatchCost(vulkan);
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--prepare-bda-upload-untouched-compute-gpu") == 0) {
    CheckBdaUploadUntouchedComputeNeighbor(vulkan);
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--bda-upload-overlap-consumer-gpu") == 0) {
    CheckBdaUploadOverlapAndConsumerNeighbors(vulkan);
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--prepare-bda-upload-async-neighbors-gpu") == 0) {
    CheckPrepareBdaUploadBatchAsyncNeighbors(vulkan);
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--empty-pm4-tail-submit-cost-gpu") == 0) {
    CheckEmptyPm4TailSubmitCost(vulkan);
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--empty-pm4-tail-required-work-neighbors-gpu") == 0) {
    CheckEmptyPm4TailRequiredWorkNeighbors(vulkan);
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--foreign-readback-queued-state-cost-gpu") == 0) {
    CheckForeignReadbackQueuedStateCost(vulkan);
    return 0;
  }
  if (argc == 2 && IsRepeatedGpuDirtyStateMode(argv[1])) {
    if (std::strcmp(argv[1], "--repeated-gpu-dirty-state-cost-gpu") == 0)
      CheckRepeatedGpuDirtyStateCost(vulkan);
    else
      CheckRepeatedGpuDirtyStateNeighbors(vulkan);
    return 0;
  }
  if (argc == 2 && IsForeignReadbackReadyMode(argv[1])) {
    if (std::strcmp(argv[1], "--foreign-readback-ready-after-wait-cost-gpu") == 0)
      CheckForeignReadbackReadyAfterWaitCost(vulkan);
    else if (std::strcmp(argv[1], "--foreign-readback-ready-pending-image-neighbor-gpu") == 0)
      CheckForeignReadbackReadyAfterWaitPendingImageGuard(vulkan);
    else if (std::strcmp(argv[1], "--foreign-readback-ready-dirty-image-neighbor-gpu") == 0)
      CheckForeignReadbackReadyAfterWaitDirtyImageGuard(vulkan);
    else if (std::strcmp(argv[1], "--foreign-readback-ready-new-writer-neighbor-gpu") == 0)
      CheckForeignReadbackReadyNewWriterNeighbor(vulkan);
    else
      CheckForeignReadbackReadyAfterWaitNeighbors(vulkan,
          std::strcmp(argv[1], "--foreign-readback-ready-unpublished-neighbor-gpu") == 0 ? 1u : 0u);
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--foreign-readback-queued-armed-neighbor-gpu") == 0) {
    CheckForeignReadbackQueuedArmedNeighbor(vulkan);
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--dma-base-origins-only") == 0) {
    CheckDmaBaseOrigins();
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--dma-base-runtime-bounds") == 0) {
    CheckDmaBaseRuntimeBounds(vulkan);
    return 0;
  }
  const bool image_ownership_mode = argc == 2 &&
      (std::strcmp(argv[1], "--bc-alias-ownership-gpu") == 0 ||
       std::strcmp(argv[1], "--bc-alias-ownership-neighbors-gpu") == 0 ||
       std::strcmp(argv[1], "--bc-alias-tail-padding-gpu") == 0 ||
       std::strcmp(argv[1], "--copy-mip-destination-gpu") == 0 ||
       std::strcmp(argv[1], "--copy-mip-destination-neighbors-gpu") == 0 ||
       std::strcmp(argv[1], "--bc-partial-buffer-gpu") == 0 ||
       std::strcmp(argv[1], "--bc-partial-buffer-neighbors-gpu") == 0 ||
       std::strcmp(argv[1], "--bc-partial-buffer-flag-neighbors-gpu") == 0);
  if (image_ownership_mode) {
    // GPU-owned guest memory must be retired on the actual GPU thread, as in
    // production. Setup/cleanup uses that thread; readback oracles stay intact.
    auto& renderer = vulkan.RuntimeRenderer();
    renderer.InitializeGpu(nullptr);
    renderer.GetGpu().SendCommandSync([&] {
      if (std::strcmp(argv[1], "--bc-partial-buffer-gpu") == 0 ||
          std::strcmp(argv[1], "--bc-partial-buffer-neighbors-gpu") == 0 ||
          std::strcmp(argv[1], "--bc-partial-buffer-flag-neighbors-gpu") == 0)
        CheckBcPartialBufferWriteGpu(vulkan,
            std::strcmp(argv[1], "--bc-partial-buffer-gpu") != 0);
      else if (std::strcmp(argv[1], "--bc-alias-tail-padding-gpu") == 0)
        CheckBcAliasPartialTailPaddingGpu(vulkan);
      else if (std::strcmp(argv[1], "--copy-mip-destination-gpu") == 0 ||
               std::strcmp(argv[1], "--copy-mip-destination-neighbors-gpu") == 0)
        CheckCopyMipDestinationOwnershipGpu(vulkan,
            std::strcmp(argv[1], "--copy-mip-destination-neighbors-gpu") == 0);
      else
        CheckBcAliasOwnershipGpu(vulkan,
            std::strcmp(argv[1], "--bc-alias-ownership-neighbors-gpu") == 0);
    });
    renderer.ShutdownGpu();
    return 0;
  }
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
  else if (argc == 2 && std::strcmp(argv[1], "--generic-channel-repetition-cost") == 0)
    CheckGenericChannelRepetitionCost(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--generic-gpu-microbench") == 0)
    CheckGenericGpuMicrobench(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--array-length-descriptor-cost") == 0)
    CheckArrayLengthDescriptorCost(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--armed-readback-window-cost-gpu") == 0)
    CheckArmedReadbackWindowCost(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--armed-readback-typed-neighbors-gpu") == 0)
    CheckArmedReadbackTypedNeighbors(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--generic-channel-repetition-neighbors") == 0)
    CheckGenericChannelRepetitionNeighbors(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--driver-cache-native-roundtrip") == 0)
    CheckDriverCacheNativeRoundTrip(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--generic-buffer-codegen-cost") == 0)
    CheckGenericBufferCodegenCost(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--generic-formatted-conversion-path-cost") == 0)
    CheckGenericFormattedConversionPathCost(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--generic-formatted-dispatch-decision-cost") == 0)
    CheckGenericFormattedDispatchDecisionCost(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--ngg-vertex-count-gpu") == 0)
    CheckNggVertexCountGpu(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--late-bda-image-fringe-gpu") == 0)
    CheckLateBdaImageFringeGpu(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--first-use-bda-pointer-chain-gpu") == 0)
    CheckFirstUseBdaPointerChainGpu(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--first-use-bda-pointer-chain-neighbors-gpu") == 0)
    CheckFirstUseBdaPointerChainGpu(vulkan, true);
  else if (argc == 2 && std::strcmp(argv[1], "--first-use-dma-base-gpu") == 0)
    CheckFirstUseDmaBaseGpu(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--first-use-dma-base-neighbors-gpu") == 0)
    CheckFirstUseDmaBaseGpu(vulkan, true);
  else if (argc == 2 && std::strcmp(argv[1], "--packed-buffer-offset-cost") == 0)
    CheckPackedBufferOffsetCodegenCost(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--packed-buffer-offset-gpu") == 0)
    CheckPackedBufferOffsetGpuReadback(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--generic-buffer-numeric-readback") == 0)
    CheckGenericBufferNumeric(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--generic-buffer-numeric-measurement") == 0)
    return MeasureGenericBufferNumeric(vulkan) == 0u ? 0 : 2;
  else if (argc == 2 && std::strcmp(argv[1], "--formatted-buffer-static-numeric-measurement") == 0)
    return MeasureGenericBufferNumeric(vulkan, true) == 0u ? 0 : 2;
  else if (readback_queue_mode) {
    if (std::strcmp(argv[1], "--readback-queue-prepared-rollover-gpu") == 0)
      CheckReadbackQueuePreparedRollover(vulkan);
    else
      CheckReadbackQueueBootstrap(vulkan);
  }
  else if (argc == 2 && std::strcmp(argv[1], "--eop-write-ordering-gpu") == 0)
    CheckEndOfPipeWriteGpu(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--eop-literal-ordering-gpu") == 0)
    CheckEndOfPipeLiteralGpu(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--eop-literal-neighbors-gpu") == 0)
    CheckEndOfPipeLiteralNeighbors(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--eop-literal-publication-cost") == 0)
    CheckLiteralEopPublicationCost(vulkan);
  else if (argc == 2 && std::strcmp(argv[1], "--eop-literal-publication-span-neighbors") == 0)
    CheckLiteralEopPublicationSpanNeighbors(vulkan);
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
