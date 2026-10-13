// Include after FusedTiledUploadCostTests.inc. Model only a missing host format
// capability; forward the actual query and preserve every other reported bit.
namespace Libs::Graphics {
namespace {
struct FusedUploadFormatlessCapabilityScope {
  static inline PFN_vkGetPhysicalDeviceFormatProperties2 original = nullptr;
  static inline uint32_t masked_queries = 0;
  static void VKAPI_PTR Query(VkPhysicalDevice device, VkFormat format,
                              VkFormatProperties2* properties) {
    original(device, format, properties);
    if (format != VK_FORMAT_R16_UINT || masked_queries != 0) return;
    for (auto* next = reinterpret_cast<VkBaseOutStructure*>(properties->pNext);
         next != nullptr; next = next->pNext) {
      if (next->sType != VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) continue;
      auto* full = reinterpret_cast<VkFormatProperties3*>(next);
      // The first extended R16 query belongs to the public upload decision.
      // Subsequent fixture setup queries see the unchanged real device limits.
      full->optimalTilingFeatures &= ~VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;
      ++masked_queries;
      break;
    }
  }
  FusedUploadFormatlessCapabilityScope() {
    EXIT_IF(original != nullptr);
    original = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceFormatProperties2;
    EXIT_IF(original == nullptr);
    masked_queries = 0;
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceFormatProperties2 = Query;
  }
  ~FusedUploadFormatlessCapabilityScope() {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceFormatProperties2 = original;
    original = nullptr;
  }
};
void CheckFusedTiledUpload2ByteCapability(VulkanHarness& vulkan) {
  FusedUploadObserved result;
  uint32_t masked = 0;
  {
    FusedUploadFormatlessCapabilityScope capabilities;
    result = RunFusedTiledUploadCase(vulkan, 2);
    masked = FusedUploadFormatlessCapabilityScope::masked_queries;
  }
  Require("FusedUpload2ByteCapability", "one actual upload capability query",
      masked == 1, "missing format query observation is not a fallback test");
  Require("FusedUpload2ByteCapability", "unsupported formatless writes keep exact copy route",
      result.intermediate_copies == 1,
      "missing host format capability bypassed the required legacy image upload");
  std::puts("FusedUpload2Byte missing-formatless-bit native exact bits/guards/source/cleanup/copy1 PASS");
}
} // namespace
} // namespace Libs::Graphics
