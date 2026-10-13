// Test-only proposal: actual production query/predication APIs, bounded8x8 raster.
// BaselineFlag0 deliberately remains conservative-visible; final oracle measures
// redundant native IA vertices after every semantic/raster control has passed.
#include "graphics/guest_gpu/command_processor/commandProcessor.h"
#include "graphics/guest_gpu/command_processor/pm4Dispatch.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/occlusionQueries.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvBuilder.h"

namespace Libs::Graphics {
namespace {
std::vector<uint32_t> OcclusionProbeSpirv(bool vertex) {
  using ShaderRecompiler::Spirv::Builder;
  Builder b(0x00010500u);
  b.RequireCapability(spv::CapabilityShader);
  b.AddMemoryModel(spv::AddressingModelLogical,spv::MemoryModelGLSL450);
  const auto v=b.Type(spv::OpTypeVoid),u=b.Type(spv::OpTypeInt,32u,0u);
  const auto f=b.Type(spv::OpTypeFloat,32u),vec=b.Type(spv::OpTypeVector,f,4u);
  const auto zero=b.Constant(spv::OpConstant,f,0u),one=b.Constant(spv::OpConstant,f,0x3f800000u);
  const auto half=b.Constant(spv::OpConstant,f,0x3f000000u);
  const auto minus=b.Constant(spv::OpConstant,f,0xbf800000u),three=b.Constant(spv::OpConstant,f,0x40400000u);
  const auto output=b.DefineGlobalVariable(b.Type(spv::OpTypePointer,spv::StorageClassOutput,vec),spv::StorageClassOutput);
  const auto fn=b.Type(spv::OpTypeFunction,v),entry=b.AllocateId();
  uint32_t index=0,boolean=0;
  if(vertex) {
    boolean=b.Type(spv::OpTypeBool);
    index=b.DefineGlobalVariable(b.Type(spv::OpTypePointer,spv::StorageClassInput,u),spv::StorageClassInput);
    b.AddAnnotation(spv::OpDecorate,index,spv::DecorationBuiltIn,spv::BuiltInVertexIndex);
    b.AddAnnotation(spv::OpDecorate,output,spv::DecorationBuiltIn,spv::BuiltInPosition);
    b.AddEntryPoint(spv::ExecutionModelVertex,entry,"main",{index,output});
  } else {
    b.AddAnnotation(spv::OpDecorate,output,spv::DecorationLocation,0u);
    b.AddEntryPoint(spv::ExecutionModelFragment,entry,"main",{output});
    b.AddExecutionMode(entry,spv::ExecutionModeOriginUpperLeft);
  }
  const auto uone=b.Constant(spv::OpConstant,u,1u),utwo=b.Constant(spv::OpConstant,u,2u);
  b.AddFunction(spv::OpFunction,v,entry,spv::FunctionControlMaskNone,fn);
  b.AddFunction(spv::OpLabel,b.AllocateId());
  const auto value=b.AllocateId();
  if(vertex) {
    const auto id=b.AllocateId(),is1=b.AllocateId(),is2=b.AllocateId(),x=b.AllocateId(),y=b.AllocateId();
    b.AddFunction(spv::OpLoad,u,id,index);
    b.AddFunction(spv::OpIEqual,boolean,is1,id,uone);b.AddFunction(spv::OpIEqual,boolean,is2,id,utwo);
    b.AddFunction(spv::OpSelect,f,x,is1,three,minus);b.AddFunction(spv::OpSelect,f,y,is2,three,minus);
    b.AddFunction(spv::OpCompositeConstruct,vec,value,x,y,half,one);
  } else b.AddFunction(spv::OpCompositeConstruct,vec,value,one,zero,zero,one);
  b.AddFunction(spv::OpStore,output,value);b.AddFunction(spv::OpReturn);b.AddFunction(spv::OpFunctionEnd);
  return b.Build();
}

class OcclusionProbeTarget {
public:
  OcclusionProbeTarget(GraphicContext& g,vk::Format format,vk::ImageAspectFlags aspect,
                       vk::ImageUsageFlags usage):graphics(g),aspect(aspect) {
    vk::ImageCreateInfo create{};create.imageType=vk::ImageType::e2D;create.format=format;
    create.extent={8u,8u,1u};create.mipLevels=create.arrayLayers=1u;create.samples=vk::SampleCountFlagBits::e1;
    create.tiling=vk::ImageTiling::eOptimal;create.usage=usage;
    Require("NativeOcclusion","actual tiny native image",graphics.CreateImage(create,backing),"native attachment allocation failed; not intended RED");
    vk::ImageViewCreateInfo info{};info.image=backing.image;info.viewType=vk::ImageViewType::e2D;info.format=format;
    info.subresourceRange={aspect,0u,1u,0u,1u};
    RequireVulkanSuccess(graphics.device.createImageView(&info,nullptr,&view),"occlusion8x8 view");
  }
  ~OcclusionProbeTarget() {graphics.device.destroyImageView(view,nullptr);graphics.DeleteImage(backing);}
  void Transition(vk::CommandBuffer cmd,vk::ImageLayout next,vk::AccessFlags access,vk::PipelineStageFlags stage) {
    vk::ImageMemoryBarrier barrier{};barrier.image=backing.image;barrier.oldLayout=layout;barrier.newLayout=next;
    barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange={aspect,0u,1u,0u,1u};
    barrier.srcAccessMask=layout==vk::ImageLayout::eUndefined?vk::AccessFlags{}:vk::AccessFlagBits::eMemoryRead|vk::AccessFlagBits::eMemoryWrite;
    barrier.dstAccessMask=access;
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,stage,{},0u,nullptr,0u,nullptr,1u,&barrier);
    layout=next;
  }
  GraphicContext& graphics;VulkanImage backing;vk::ImageView view=nullptr;
  vk::ImageAspectFlags aspect;vk::ImageLayout layout=vk::ImageLayout::eUndefined;
};

class OcclusionProbePipeline {
public:
  explicit OcclusionProbePipeline(vk::Device d):device(d) {
    const auto make=[&](bool vertex,vk::ShaderModule& module) {
      const auto words=OcclusionProbeSpirv(vertex);
      spvtools::SpirvTools validation(SPV_ENV_VULKAN_1_3);
      Require("NativeOcclusion","tiny shader validation",validation.Validate(words),"native shader setup failed; not intended RED");
      vk::ShaderModuleCreateInfo shader{};shader.codeSize=words.size()*4u;shader.pCode=words.data();
      RequireVulkanSuccess(device.createShaderModule(&shader,nullptr,&module),"occlusion tiny shader module");
    };
    make(true,vs);make(false,fs);
    vk::PipelineLayoutCreateInfo empty{};
    RequireVulkanSuccess(device.createPipelineLayout(&empty,nullptr,&layout),"occlusion descriptor-free pipeline layout");
    std::array<vk::PipelineShaderStageCreateInfo,2> stages{};
    stages[0].stage=vk::ShaderStageFlagBits::eVertex;stages[0].module=vs;stages[0].pName="main";
    stages[1].stage=vk::ShaderStageFlagBits::eFragment;stages[1].module=fs;stages[1].pName="main";
    vk::PipelineVertexInputStateCreateInfo vertex{};
    vk::PipelineInputAssemblyStateCreateInfo assembly{};assembly.topology=vk::PrimitiveTopology::eTriangleList;
    const vk::Viewport viewport{0,0,8,8,0,1};const vk::Rect2D scissor{{0,0},{8,8}};
    vk::PipelineViewportStateCreateInfo view{};view.viewportCount=view.scissorCount=1u;view.pViewports=&viewport;view.pScissors=&scissor;
    vk::PipelineRasterizationStateCreateInfo raster{};raster.polygonMode=vk::PolygonMode::eFill;raster.cullMode=vk::CullModeFlagBits::eNone;raster.lineWidth=1;
    vk::PipelineMultisampleStateCreateInfo samples{};samples.rasterizationSamples=vk::SampleCountFlagBits::e1;
    vk::PipelineDepthStencilStateCreateInfo depth{};depth.depthTestEnable=true;depth.depthWriteEnable=false;depth.depthCompareOp=vk::CompareOp::eLess;
    vk::PipelineColorBlendAttachmentState attachment{};attachment.colorWriteMask=vk::ColorComponentFlagBits::eR|vk::ColorComponentFlagBits::eG|vk::ColorComponentFlagBits::eB|vk::ColorComponentFlagBits::eA;
    vk::PipelineColorBlendStateCreateInfo blend{};blend.attachmentCount=1u;blend.pAttachments=&attachment;
    const vk::Format format=vk::Format::eR8G8B8A8Unorm;
    vk::PipelineRenderingCreateInfo rendering{};rendering.colorAttachmentCount=1u;rendering.pColorAttachmentFormats=&format;rendering.depthAttachmentFormat=vk::Format::eD32Sfloat;
    vk::GraphicsPipelineCreateInfo info{};info.pNext=&rendering;info.stageCount=2u;info.pStages=stages.data();info.layout=layout;
    info.pVertexInputState=&vertex;info.pInputAssemblyState=&assembly;info.pViewportState=&view;
    info.pRasterizationState=&raster;info.pMultisampleState=&samples;info.pDepthStencilState=&depth;info.pColorBlendState=&blend;
    RequireVulkanSuccess(device.createGraphicsPipelines(nullptr,1u,&info,nullptr,&pipeline),"occlusion finite native pipeline");
  }
  ~OcclusionProbePipeline() {device.destroyPipeline(pipeline,nullptr);device.destroyPipelineLayout(layout,nullptr);device.destroyShaderModule(vs,nullptr);device.destroyShaderModule(fs,nullptr);}
  void Draw(vk::CommandBuffer cmd) {cmd.bindPipeline(vk::PipelineBindPoint::eGraphics,pipeline);cmd.draw(3u,1u,0u,0u);}
  vk::Device device;vk::ShaderModule vs=nullptr,fs=nullptr;vk::PipelineLayout layout=nullptr;vk::Pipeline pipeline=nullptr;
};

void CheckOcclusionNativeCost(VulkanHarness& vulkan) {
  constexpr const char* name="NativeOcclusionCost";
  const char* mode=std::getenv("KYTY_OCCLUSION_QUERIES");
  const bool real=mode!=nullptr && std::strcmp(mode,"1")==0;
  Require(name,"fixture modes",mode!=nullptr && (std::strcmp(mode,"0")==0 || real),"run exact same fixture with explicit0 then1; unsupported setup, not RED");
  Require(name,"no artificial hidden counters",std::getenv("KYTY_DEBUG_OCCLUSION_HIDDEN_FILE")==nullptr,"synthetic-hidden fallback forbidden");
  auto& graphics=vulkan.RuntimeContext();auto& renderer=vulkan.RuntimeRenderer();auto& scheduler=renderer.GetCommandScheduler();
  auto& queries=renderer.GetOcclusionQueries();
  Require(name,"actual option and device",graphics.host_query_reset_enabled && queries.Enabled()==real,"test native capability metadata incorrect; not RED");
  Require(name,"independent GPU work oracle",graphics.physical_device.getFeatures().pipelineStatisticsQuery==VK_TRUE,"pipeline-statistics capability unavailable; not RED");
  GraphicsInitJmpTables();FetchGpuGuest guest;
  LibKernel::Memory::InstallGpuResources(&renderer);renderer.MapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
  CommandProcessor processor(renderer,0);processor.Reset();processor.BufferInit();
  OcclusionProbeTarget color(graphics,vk::Format::eR8G8B8A8Unorm,vk::ImageAspectFlagBits::eColor,
      vk::ImageUsageFlagBits::eColorAttachment|vk::ImageUsageFlagBits::eTransferSrc);
  OcclusionProbeTarget depth(graphics,vk::Format::eD32Sfloat,vk::ImageAspectFlagBits::eDepth,vk::ImageUsageFlagBits::eDepthStencilAttachment);
  OcclusionProbePipeline pipeline(vulkan.Device());
  FetchGpuBuffer pixels(graphics,256u,vk::BufferUsageFlagBits::eTransferDst);
  vk::QueryPoolCreateInfo stats{};stats.queryType=vk::QueryType::ePipelineStatistics;stats.queryCount=3u;
  stats.pipelineStatistics=vk::QueryPipelineStatisticFlagBits::eInputAssemblyVertices;
  vk::QueryPool statistics=nullptr;
  RequireVulkanSuccess(vulkan.Device().createQueryPool(&stats,nullptr,&statistics),"independent occlusion work statistics");
  const auto complete=[&] {
    const auto tick=scheduler.Submit();const auto semaphore=scheduler.GetMasterSemaphore().Handle();
    vk::SemaphoreWaitInfo wait{};wait.semaphoreCount=1u;wait.pSemaphores=&semaphore;wait.pValues=&tick;
    RequireVulkanSuccess(vulkan.Device().waitSemaphores(&wait,2000000000ull),"bounded2s occlusion fixture completion");
    scheduler.GetMasterSemaphore().Refresh();queries.PublishCompleted(false);scheduler.BeginCommand();
  };
  const auto dump=[&](uint64_t address) {
    const std::array<uint32_t,4> packet{KYTY_PM4(4,Pm4::IT_EVENT_WRITE,Pm4::R_ZERO),0x00000139u,
        static_cast<uint32_t>(address),static_cast<uint32_t>(address>>32u)};
    Pm4Execution execution;
    Require(name,"actual ZPASS packet",processor.Process(execution,packet)==Pm4ProcessResult::Complete,"query dump packet did not complete");
  };
  const auto raster=[&](bool visible,bool issue_draw) {
    auto& command=scheduler.Current();auto cmd=command.Handle();
    color.Transition(cmd,vk::ImageLayout::eColorAttachmentOptimal,vk::AccessFlagBits::eColorAttachmentWrite,vk::PipelineStageFlagBits::eColorAttachmentOutput);
    depth.Transition(cmd,vk::ImageLayout::eDepthStencilAttachmentOptimal,vk::AccessFlagBits::eDepthStencilAttachmentRead|vk::AccessFlagBits::eDepthStencilAttachmentWrite,
        vk::PipelineStageFlagBits::eEarlyFragmentTests|vk::PipelineStageFlagBits::eLateFragmentTests);
    RenderState rendering{};rendering.width=rendering.height=8u;rendering.num_color_attachments=1u;
    auto& c=rendering.color_attachments[0];c.image_view=color.view;c.image_layout=color.layout;c.is_clear=true;
    auto& d=rendering.depth_stencil_attachment;d.image_view=depth.view;d.image_layout=depth.layout;d.has_depth=d.depth_clear=true;
    d.clear_value[0]=std::bit_cast<uint32_t>(visible?0.75f:0.25f);
    command.BeginRendering(rendering);if(issue_draw)pipeline.Draw(cmd);command.EndRendering();scheduler.NoteCommandRecorded();
  };
  const auto copy_color=[&] {
    auto cmd=scheduler.Current().Handle();color.Transition(cmd,vk::ImageLayout::eTransferSrcOptimal,vk::AccessFlagBits::eTransferRead,vk::PipelineStageFlagBits::eTransfer);
    vk::BufferImageCopy copy{};copy.imageSubresource={vk::ImageAspectFlagBits::eColor,0u,0u,1u};copy.imageExtent={8u,8u,1u};
    cmd.copyImageToBuffer(color.backing.image,color.layout,pixels.buffer,1u,&copy);
    vk::BufferMemoryBarrier host{};host.buffer=pixels.buffer;host.size=pixels.size;host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    host.srcAccessMask=vk::AccessFlagBits::eTransferWrite;host.dstAccessMask=vk::AccessFlagBits::eHostRead;
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eHost,{},0u,nullptr,1u,&host,0u,nullptr);
  };
  std::array<uint64_t,3> actual_vertices{};
  for(uint32_t trial=0;trial<3u;trial++) {
    // hidden /visible /mixed two-render-instance query, reusing the native pool.
    const bool visible=trial!=0u;
    const uint64_t address=FetchGpuGuest::Base+0x8000u+trial*0x1000u;
    auto* counters=reinterpret_cast<uint64_t*>(address);
    constexpr uint64_t sentinel=0x13579bdf2468ace0ull;
    std::fill_n(counters-2,36u,sentinel);
    processor.SetPredication(0u,0u,0u,nullptr,0u);
    dump(address);
    if(trial==2u)raster(false,true);
    raster(visible,true);dump(address+8u);copy_color();
    if(real) {
      Require(name,"no publication before submission",queries.HasPendingDumps() && counters[0]==sentinel && counters[31]==sentinel,"CPU counters published before actual native completion");
      const auto before=scheduler.CurrentTick();processor.SetPredication(1u,1u,1u,counters,0u);
      Require(name,"unready no-wait stays conservative",!processor.ShouldSkipPredicatedPackets() && scheduler.CurrentTick()==before,"unready no-wait skipped required work or submitted");
    }
    complete();pixels.Invalidate();
    const auto* rgba=static_cast<const uint32_t*>(pixels.data);
    for(uint32_t texel=0;texel<64u;texel++)Require(name,"independent depth/raster golden",rgba[texel]==(visible?0xff0000ffu:0u),"visible raster missing or depth-hidden raster leaked");
    uint64_t delta=0;
    for(uint32_t db=0;db<16u;db++) {
      Require(name,"every DB ready",(counters[db*2u]&counters[db*2u+1u]&OcclusionCounters::ReadyBit)!=0,"query result publication is incomplete");
      delta+=counters[db*2u+1u]-counters[db*2u];
    }
    Require(name,"actual query semantics",real?((delta!=0)==visible):(delta==16u),"native sample visibility or documented conservative fallback changed");
    Require(name,"counter footprint canaries",counters[-2]==sentinel && counters[-1]==sentinel && counters[32]==sentinel && counters[33]==sentinel,"query publication crossed256byte pair footprint");
    Require(name,"native dumps drained",!queries.HasPendingDumps(),"completed native dump still pending");
    uint32_t tagged=0u,unconditional=0u;
    const uint64_t a=reinterpret_cast<uint64_t>(&tagged),b=reinterpret_cast<uint64_t>(&unconditional);
    const std::array<uint32_t,14> packets{0xc0022000u,0x00011100u,static_cast<uint32_t>(address),static_cast<uint32_t>(address>>32u),
        KYTY_PM4(5,Pm4::IT_WRITE_DATA,0)|1u,0u,static_cast<uint32_t>(a),static_cast<uint32_t>(a>>32u),11u,
        KYTY_PM4(5,Pm4::IT_WRITE_DATA,0),0u,static_cast<uint32_t>(b),static_cast<uint32_t>(b>>32u),22u};
    Pm4Execution execution;
    Require(name,"actual predication packet result",processor.Process(execution,packets)==Pm4ProcessResult::Complete &&
        tagged==((real && !visible)?0u:11u) && unconditional==22u && processor.ShouldSkipPredicatedPackets()==(real && !visible),"predicated packet polarity/required untagged work changed");
    const auto skip=processor.ShouldSkipPredicatedPackets();
    auto cmd=scheduler.Current().Handle();cmd.resetQueryPool(statistics,trial,1u);
    cmd.beginQuery(statistics,trial,{});raster(visible,!skip);cmd.endQuery(statistics,trial);copy_color();complete();pixels.Invalidate();
    RequireVulkanSuccess(vulkan.Device().getQueryPoolResults(statistics,trial,1u,sizeof(uint64_t),&actual_vertices[trial],sizeof(uint64_t),vk::QueryResultFlagBits::e64),"completed IA vertex result without WAIT");
    Require(name,"native work count control",actual_vertices[trial]==((real && !visible)?0u:3u),"native IA vertex count does not match actual query/predication");
    for(uint32_t texel=0;texel<64u;texel++)Require(name,"same full visible pixels after predication",static_cast<const uint32_t*>(pixels.data)[texel]==(visible?0xff0000ffu:0u),"predication removed visible required work or introduced hidden pixels");
    std::printf("[host] NativeOcclusion trial%u real%u delta%llu actualIAvertices%llu goldens/canaries/orderingPASS\n",trial,real?1u:0u,
        static_cast<unsigned long long>(delta),static_cast<unsigned long long>(actual_vertices[trial]));
  }
  // A pending native result cannot write back into memory logically unmapped by
  // the resource owner. Keep actual kernel backing intact for independent canaries.
  if(real) {
    const uint64_t address=FetchGpuGuest::Base+0xb000u;auto* counters=reinterpret_cast<uint64_t*>(address);
    constexpr uint64_t sentinel=0x257913acefb46800ull;std::fill_n(counters,32u,sentinel);
    dump(address);raster(false,true);dump(address+8u);queries.OnUnmap(address,256u);complete();
    Require(name,"pending unmap prevents publication",std::all_of(counters,counters+32u,[&](uint64_t value){return value==sentinel;}),"pending dump wrote an unmapped guest interval");
  }
  processor.SetPredication(0u,0u,0u,nullptr,0u);
  scheduler.Finish();vulkan.Device().destroyQueryPool(statistics,nullptr);
  renderer.UnmapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);LibKernel::Memory::InstallGpuResources(nullptr);
  std::printf("[host] NativeOcclusion allcontrolsPASS; hiddenIA=%llu visibleIA=%llu mixedIA=%llu; finalnativeworkbudget0\n",
      static_cast<unsigned long long>(actual_vertices[0]),static_cast<unsigned long long>(actual_vertices[1]),static_cast<unsigned long long>(actual_vertices[2]));
  Require(name,"avoidable depth-hidden native work cost",actual_vertices[0]==0u,
      "depth-hidden object produced3 actual IA vertices under conservative fallback; intended cost RED only after all semantic/raster controls");
}
} // namespace
} // namespace Libs::Graphics
