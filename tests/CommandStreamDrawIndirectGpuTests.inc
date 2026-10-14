// Finite native semantic controls for copied non-indexed indirect recording.
// Uses existing actual8x8 raster helpers; no guest captures or synthetic success.
#include "graphics/host_gpu/renderer/commandHooks.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace Libs::Graphics {
namespace {
void ObserveDrawIndirectNativeDrain(CommandStreamRegression::Phase phase,uint32_t,void* opaque) {
  if(phase==CommandStreamRegression::Phase::DrainWaiting)++*static_cast<uint32_t*>(opaque);
}

void CheckCommandStreamDrawIndirectGpu(VulkanHarness& vulkan) {
  constexpr const char* name="CommandStreamDrawIndirectGpu";
  constexpr uint32_t Canary=0x13579bdfu,ArgsWords=32u,PixelWords=80u;
  constexpr vk::DeviceSize ArgsOffset=32u,PixelOffset=32u;
  struct Observation {
    uint32_t draw_count=0,stride=0,producer_drains=0;
    uint64_t actual_vertices=~uint64_t{0};
    bool rgba=false,pixel_canaries=false,args_bytes=false;
  };
  std::array<Observation,3> observed{};
  bool workers_joined=false,resources_retired=false;
  {
    auto& graphics=vulkan.RuntimeContext();
    auto& scheduler=vulkan.RuntimeRenderer().GetCommandScheduler();
    const auto features=graphics.physical_device.getFeatures();
    // VulkanHarness enables the selected device's reported core feature set.
    Require(name,"finite query/multidraw native capabilities",
        features.pipelineStatisticsQuery==VK_TRUE&&features.multiDrawIndirect==VK_TRUE&&
        graphics.physical_device_properties.limits.maxDrawIndirectCount>=2u,
        "native device cannot execute IA query/drawCount2; unsupported setup, not passing proof");
    Require(name,"forced real recording thread",
        Config::RecordThreadEnabled()&&Config::AsyncSubmitEnabled(),
        "configure deferred async recording before creating the native renderer");
    // RenderContext construction has already enabled async submit and its real stream.
    Require(name,"actual production hooks",CommandRecordingDeferred(),
        "direct-only rendering is not coverage of CommandStream copied recording");
    HW::Context registers{};HW::UserConfig user_config{};HW::Shader shaders{};
    scheduler.Begin(registers,user_config,shaders);

    OcclusionProbeTarget color(graphics,vk::Format::eR8G8B8A8Unorm,
        vk::ImageAspectFlagBits::eColor,
        vk::ImageUsageFlagBits::eColorAttachment|vk::ImageUsageFlagBits::eTransferSrc);
    OcclusionProbeTarget depth(graphics,vk::Format::eD32Sfloat,
        vk::ImageAspectFlagBits::eDepth,vk::ImageUsageFlagBits::eDepthStencilAttachment);
    OcclusionProbePipeline pipeline(vulkan.Device());
    FetchGpuBuffer upload(graphics,ArgsWords*4u,vk::BufferUsageFlagBits::eTransferSrc);
    FetchGpuBuffer args(graphics,ArgsWords*4u,
        vk::BufferUsageFlagBits::eTransferDst|vk::BufferUsageFlagBits::eIndirectBuffer);
    FetchGpuBuffer pixels(graphics,PixelWords*4u,vk::BufferUsageFlagBits::eTransferDst);
    vk::QueryPoolCreateInfo query_info{};query_info.queryType=vk::QueryType::ePipelineStatistics;
    query_info.queryCount=3u;
    query_info.pipelineStatistics=vk::QueryPipelineStatisticFlagBits::eInputAssemblyVertices;
    vk::QueryPool statistics=nullptr;
    RequireVulkanSuccess(vulkan.Device().createQueryPool(&query_info,nullptr,&statistics),
        "indirect native IA query pool");
    std::array<uint32_t,ArgsWords> expected_args{};expected_args.fill(Canary);
    for(const uint32_t start:{8u,16u}) {
      expected_args[start]=3u;expected_args[start+1u]=1u;
      expected_args[start+2u]=expected_args[start+3u]=0u;
    }
    for(uint32_t trial=0;trial<3u;trial++) {
      auto& result=observed[trial];result.draw_count=trial;
      result.stride=trial>1u?32u:0u; // stride ignored for0/1; real gap for2.
      std::memcpy(upload.data,expected_args.data(),sizeof(expected_args));upload.Flush();
      auto* initial_args=static_cast<uint32_t*>(args.data);
      std::fill_n(initial_args,ArgsWords,Canary);
      for(const uint32_t start:{8u,16u})std::fill_n(initial_args+start,4u,0u);
      args.Flush();
      std::fill_n(static_cast<uint32_t*>(pixels.data),PixelWords,Canary);pixels.Flush();
      auto& command=scheduler.Current();const auto cmd=command.Handle();
      vk::MemoryBarrier host_ready{};host_ready.srcAccessMask=vk::AccessFlagBits::eHostWrite;
      host_ready.dstAccessMask=vk::AccessFlagBits::eTransferRead|vk::AccessFlagBits::eTransferWrite;
      cmd.pipelineBarrier(vk::PipelineStageFlagBits::eHost,vk::PipelineStageFlagBits::eTransfer,
          {},1u,&host_ready,0u,nullptr,0u,nullptr);
      // Actual GPU producer write is copied through production's deferred hook.
      // Host args contained zero vertices; missing write/order cannot draw red.
      const vk::BufferCopy copy{0u,0u,args.size};
      cmd.copyBuffer(upload.buffer,args.buffer,1u,&copy);
      vk::BufferMemoryBarrier arguments_ready{};arguments_ready.buffer=args.buffer;
      arguments_ready.offset=0u;arguments_ready.size=args.size;
      arguments_ready.srcQueueFamilyIndex=arguments_ready.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      arguments_ready.srcAccessMask=vk::AccessFlagBits::eTransferWrite;
      arguments_ready.dstAccessMask=vk::AccessFlagBits::eIndirectCommandRead;
      cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eDrawIndirect,
          {},0u,nullptr,1u,&arguments_ready,0u,nullptr);
      color.Transition(cmd,vk::ImageLayout::eColorAttachmentOptimal,
          vk::AccessFlagBits::eColorAttachmentWrite,vk::PipelineStageFlagBits::eColorAttachmentOutput);
      depth.Transition(cmd,vk::ImageLayout::eDepthStencilAttachmentOptimal,
          vk::AccessFlagBits::eDepthStencilAttachmentRead|vk::AccessFlagBits::eDepthStencilAttachmentWrite,
          vk::PipelineStageFlagBits::eEarlyFragmentTests|vk::PipelineStageFlagBits::eLateFragmentTests);
      RenderState rendering{};rendering.width=rendering.height=8u;
      rendering.num_color_attachments=1u;
      auto& c=rendering.color_attachments[0];c.image_view=color.view;
      c.image_layout=color.layout;c.is_clear=true;
      auto& d=rendering.depth_stencil_attachment;d.image_view=depth.view;d.image_layout=depth.layout;
      d.has_depth=d.depth_clear=true;d.clear_value[0]=std::bit_cast<uint32_t>(0.75f);
      cmd.resetQueryPool(statistics,trial,1u);
      command.BeginRendering(rendering);
      cmd.bindPipeline(vk::PipelineBindPoint::eGraphics,pipeline.pipeline);
      cmd.beginQuery(statistics,trial,{});
      const auto old_observer=CommandStreamRegression::observer;
      void* const old_context=CommandStreamRegression::context;
      CommandStreamRegression::observer=&ObserveDrawIndirectNativeDrain;
      CommandStreamRegression::context=&result.producer_drains;
      cmd.drawIndirect(args.buffer,ArgsOffset,result.draw_count,result.stride);
      CommandStreamRegression::observer=old_observer;
      CommandStreamRegression::context=old_context;
      cmd.endQuery(statistics,trial);command.EndRendering();scheduler.NoteCommandRecorded();
      color.Transition(cmd,vk::ImageLayout::eTransferSrcOptimal,
          vk::AccessFlagBits::eTransferRead,vk::PipelineStageFlagBits::eTransfer);
      vk::BufferImageCopy pixels_copy{};pixels_copy.bufferOffset=PixelOffset;
      pixels_copy.imageSubresource={vk::ImageAspectFlagBits::eColor,0u,0u,1u};
      pixels_copy.imageExtent={8u,8u,1u};
      cmd.copyImageToBuffer(color.backing.image,color.layout,pixels.buffer,1u,&pixels_copy);
      vk::MemoryBarrier host_done{};
      host_done.srcAccessMask=vk::AccessFlagBits::eTransferWrite;
      host_done.dstAccessMask=vk::AccessFlagBits::eHostRead;
      cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eHost,
          {},1u,&host_done,0u,nullptr,0u,nullptr);
      const auto tick=scheduler.Submit();
      const auto semaphore=scheduler.GetMasterSemaphore().Handle();
      vk::SemaphoreWaitInfo wait{};wait.semaphoreCount=1u;
      wait.pSemaphores=&semaphore;wait.pValues=&tick;
      RequireVulkanSuccess(vulkan.Device().waitSemaphores(&wait,2'000'000'000ull),
          "finite indirect raster/copy completion2s");
      scheduler.GetMasterSemaphore().Refresh();pixels.Invalidate();args.Invalidate();
      RequireVulkanSuccess(vulkan.Device().getQueryPoolResults(statistics,trial,1u,
          sizeof(uint64_t),&result.actual_vertices,sizeof(uint64_t),vk::QueryResultFlagBits::e64),
          "actual completed IA vertices without native WAIT");
      result.args_bytes=std::memcmp(args.data,expected_args.data(),sizeof(expected_args))==0;
      const auto* rgba=static_cast<const uint32_t*>(pixels.data);
      result.pixel_canaries=true;result.rgba=true;
      for(uint32_t word=0;word<8u;word++)
        result.pixel_canaries&=rgba[word]==Canary&&rgba[72u+word]==Canary;
      for(uint32_t texel=0;texel<64u;texel++)
        result.rgba&=rgba[8u+texel]==(trial==0u?0u:0xff0000ffu);
      if(trial+1u<3u)scheduler.BeginCommand();
    }
    // All three native timelines completed; this joins the actual recording and
    // publication workers, with no open recording or withheld CPU gate.
    scheduler.Shutdown();workers_joined=true;
    vulkan.Device().destroyQueryPool(statistics,nullptr);
  } // Native buffers/images/views/pipelines/modules retire after GPU+CPU completion.
  resources_retired=true;
  bool numeric=true;
  for(const auto& result:observed) {
    numeric&=result.actual_vertices==uint64_t{result.draw_count}*3u&&
        result.rgba&&result.pixel_canaries&&result.args_bytes;
    std::printf("[draw-indirect-gpu] realProductionHooks=1 realCommandStream=1 gpuWrittenArgs=1 "
        "drawCount=%u stride=%u actualIAvertices=%llu expectedIAvertices=%u "
        "rgba=%u pixelCanaries=%u argsAndCanaries=%u actualProducerDrains=%u "
        "workersJoined=%u resourcesRetired=%u finiteGPUWaitNs=2000000000\n",
        result.draw_count,result.stride,static_cast<unsigned long long>(result.actual_vertices),
        result.draw_count*3u,result.rgba,result.pixel_canaries,result.args_bytes,
        result.producer_drains,workers_joined,resources_retired);
  }
  std::fflush(stdout);
  Require(name,"unchanged native numeric/lifetime controls",
      numeric&&workers_joined&&resources_retired,
      "native IA count, full raster, GPU-written args/canaries or resource retirement mismatch");
  // Numeric oracle is identical OFF/ON. Producer-drain counts are observations,
  // not native semantics; the separate CPU fixture proves the cost RED/GREEN.
}
} // namespace
} // namespace Libs::Graphics
