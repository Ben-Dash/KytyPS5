// Global scope before VulkanHarness. Test-only direct native forwarding observer.
class CsPartialFlushAliasBarrierAudit {
public:
  CsPartialFlushAliasBarrierAudit() {
    Require("ComputeChainVisibility", "direct native observer bootstrap",
      up1==nullptr&&!CommandRecordingDeferred()&&VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier!=nullptr,
      "nested, missing or deferred native dispatcher; not a shader result failure");
    up1=VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier;
    up2=VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier2;
    up2khr=VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier2KHR;
    local=this;VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier=Observe1;
    if(up2!=nullptr)VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier2=Observe2;
    if(up2khr!=nullptr)VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier2KHR=Observe2KHR;
  }
  ~CsPartialFlushAliasBarrierAudit() {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier=up1;
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier2=up2;
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier2KHR=up2khr;
    local=nullptr;up1=nullptr;up2=nullptr;up2khr=nullptr;
  }
  CsPartialFlushAliasBarrierAudit(const CsPartialFlushAliasBarrierAudit&)=delete;
  CsPartialFlushAliasBarrierAudit& operator=(const CsPartialFlushAliasBarrierAudit&)=delete;
  uint64_t shader_access=0,full_write_visibility=0;
  struct Observed {
    VkPipelineStageFlags2 source=0,destination=0;
    VkAccessFlags2 source_access=0,destination_access=0;
  };
  bool capture_partial=false;
  uint32_t partial_count=0;
  std::array<Observed,4> partial{};
private:
  static inline PFN_vkCmdPipelineBarrier up1=nullptr;
  static inline PFN_vkCmdPipelineBarrier2 up2=nullptr;
  static inline PFN_vkCmdPipelineBarrier2KHR up2khr=nullptr;
  static inline thread_local CsPartialFlushAliasBarrierAudit* local=nullptr;
  static void VKAPI_PTR Observe1(VkCommandBuffer command,VkPipelineStageFlags src,VkPipelineStageFlags dst,
      VkDependencyFlags flags,uint32_t count,const VkMemoryBarrier* memory,uint32_t buffers,
      const VkBufferMemoryBarrier* buffer_barriers,uint32_t images,const VkImageMemoryBarrier* image_barriers) {
    if(local!=nullptr&&(src&VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)!=0&&(dst&VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)!=0)
      for(uint32_t i=0;i<count;++i)
        if((memory[i].srcAccessMask&(VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT))==
            (VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT)&&
           (memory[i].dstAccessMask&(VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT))==
            (VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT))++local->shader_access;
    up1(command,src,dst,flags,count,memory,buffers,buffer_barriers,images,image_barriers);
  }
  static void Note2(const VkDependencyInfo* info) {
    if(local==nullptr)return;
    for(uint32_t i=0;i<info->memoryBarrierCount;++i) {
      const auto& b=info->pMemoryBarriers[i];
      if(local->capture_partial) {
        if(local->partial_count<local->partial.size())
          local->partial[local->partial_count]={b.srcStageMask,b.dstStageMask,b.srcAccessMask,b.dstAccessMask};
        ++local->partial_count;
      }
      if((b.srcStageMask&VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT)!=0&&
         (b.dstStageMask&VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT)!=0&&
         (b.srcAccessMask&VK_ACCESS_2_MEMORY_WRITE_BIT)!=0&&
         (b.dstAccessMask&(VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT))==
            (VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT))++local->full_write_visibility;
    }
  }
  static void VKAPI_PTR Observe2(VkCommandBuffer command,const VkDependencyInfo* info) {
    Note2(info);up2(command,info);
  }
  static void VKAPI_PTR Observe2KHR(VkCommandBuffer command,const VkDependencyInfo* info) {
    Note2(info);up2khr(command,info);
  }
};
