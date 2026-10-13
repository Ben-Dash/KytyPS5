// Include after EmptyPm4TailRequiredWorkNeighbors.inc; real public PrepareBda and actual native SSBO reader.
#include <new>
namespace Libs::Graphics {
namespace {
void ConfigurePrepareBdaUploadBatchCostTests() {
  ConfigureEmptyPm4TailSubmitCostTests();
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  _putenv_s("KYTY_FAULT_AHEAD_PAGES","1");
#else
  setenv("KYTY_FAULT_AHEAD_PAGES","1",1);
#endif
}
std::vector<uint32_t> BdaUploadConsumerSpirv(uint32_t count) {
  using ShaderRecompiler::Spirv::Builder;Builder b(0x00010500u);
  b.RequireCapability(spv::CapabilityShader);b.AddMemoryModel(spv::AddressingModelLogical,spv::MemoryModelGLSL450);
  const auto v=b.Type(spv::OpTypeVoid),u=b.Type(spv::OpTypeInt,32u,0u),vec=b.Type(spv::OpTypeVector,u,3u);
  const auto zero=b.Constant(spv::OpConstant,u,0u),n=b.Constant(spv::OpConstant,u,count);
  const auto words=b.Type(spv::OpTypeRuntimeArray,u);b.AddAnnotation(spv::OpDecorate,words,spv::DecorationArrayStride,4u);
  const auto block=b.Type(spv::OpTypeStruct,words);b.AddAnnotation(spv::OpDecorate,block,spv::DecorationBlock);
  b.AddAnnotation(spv::OpMemberDecorate,block,0u,spv::DecorationOffset,0u);
  const auto ptr=b.Type(spv::OpTypePointer,spv::StorageClassStorageBuffer,block);
  const auto elem=b.Type(spv::OpTypePointer,spv::StorageClassStorageBuffer,u);
  std::array<uint32_t,3> buffers{};
  for(uint32_t i=0;i<3u;++i){buffers[i]=b.DefineGlobalVariable(ptr,spv::StorageClassStorageBuffer);
    b.AddAnnotation(spv::OpDecorate,buffers[i],spv::DecorationDescriptorSet,0u);
    b.AddAnnotation(spv::OpDecorate,buffers[i],spv::DecorationBinding,i);}
  const auto iptr=b.Type(spv::OpTypePointer,spv::StorageClassInput,vec);
  const auto gid=b.DefineGlobalVariable(iptr,spv::StorageClassInput);
  b.AddAnnotation(spv::OpDecorate,gid,spv::DecorationBuiltIn,spv::BuiltInGlobalInvocationId);
  const auto fn=b.Type(spv::OpTypeFunction,v),entry=b.AllocateId();
  b.AddEntryPoint(spv::ExecutionModelGLCompute,entry,"main",{buffers[0],buffers[1],buffers[2],gid});
  b.AddExecutionMode(entry,spv::ExecutionModeLocalSize,64u,1u,1u);
  b.AddFunction(spv::OpFunction,v,entry,spv::FunctionControlMaskNone,fn);b.AddFunction(spv::OpLabel,b.AllocateId());
  const auto xyz=b.AllocateId(),index=b.AllocateId();b.AddFunction(spv::OpLoad,vec,xyz,gid);
  b.AddFunction(spv::OpCompositeExtract,u,index,xyz,0u);
  const auto other=b.AllocateId();b.AddFunction(spv::OpIAdd,u,other,index,n);
  for(uint32_t i=0;i<2u;++i){const auto source=b.AllocateId(),value=b.AllocateId(),dest=b.AllocateId();
    b.AddFunction(spv::OpAccessChain,elem,source,buffers[i],zero,index);b.AddFunction(spv::OpLoad,u,value,source);
    b.AddFunction(spv::OpAccessChain,elem,dest,buffers[2],zero,i==0u?index:other);b.AddFunction(spv::OpStore,dest,value);}
  b.AddFunction(spv::OpReturn);b.AddFunction(spv::OpFunctionEnd);return b.Build();
}
class BdaUploadNativeConsumer {
public:
  BdaUploadNativeConsumer(vk::Device d,vk::Buffer first,vk::Buffer second,vk::Buffer output,uint32_t words):device(d) {
    const auto spirv=BdaUploadConsumerSpirv(words);
    spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_3);
    Require("PrepareBdaUploadBatchCost","consumer SPIR-V validation",validator.Validate(spirv),
            "bounded compute producer setup failed; not an EOP semantic failure");
    vk::ShaderModuleCreateInfo shader{};shader.codeSize=spirv.size()*4u;shader.pCode=spirv.data();
    RequireVulkanSuccess(device.createShaderModule(&shader,nullptr,&module),"EOP compute module");
    std::array<vk::DescriptorSetLayoutBinding,3> bindings{};
    for(uint32_t i=0;i<3u;++i){bindings[i].binding=i;bindings[i].descriptorCount=1u;
      bindings[i].descriptorType=vk::DescriptorType::eStorageBuffer;bindings[i].stageFlags=vk::ShaderStageFlagBits::eCompute;}
    vk::DescriptorSetLayoutCreateInfo layout{};layout.bindingCount=3u;layout.pBindings=bindings.data();
    RequireVulkanSuccess(device.createDescriptorSetLayout(&layout,nullptr,&set_layout),"BDA consumer set layout");
    vk::DescriptorPoolSize size{vk::DescriptorType::eStorageBuffer,3u};
    vk::DescriptorPoolCreateInfo pool_info{};pool_info.maxSets=1u;pool_info.poolSizeCount=1u;pool_info.pPoolSizes=&size;
    RequireVulkanSuccess(device.createDescriptorPool(&pool_info,nullptr,&pool),"BDA consumer pool");
    vk::DescriptorSetAllocateInfo allocation{};allocation.descriptorPool=pool;allocation.descriptorSetCount=1u;
    allocation.pSetLayouts=&set_layout;
    RequireVulkanSuccess(device.allocateDescriptorSets(&allocation,&set),"BDA consumer descriptor");
    std::array<vk::DescriptorBufferInfo,3> infos{{{first,0u,words*4u},{second,0u,words*4u},{output,0u,words*8u}}};
    std::array<vk::WriteDescriptorSet,3> writes{};
    for(uint32_t i=0;i<3u;++i){writes[i].dstSet=set;writes[i].dstBinding=i;writes[i].descriptorCount=1u;
      writes[i].descriptorType=vk::DescriptorType::eStorageBuffer;writes[i].pBufferInfo=&infos[i];}
    device.updateDescriptorSets(3u,writes.data(),0u,nullptr);
    groups=words/64u;EXIT_IF(words==0u||(words%64u)!=0u);
    vk::PipelineLayoutCreateInfo pipeline_layout_info{};pipeline_layout_info.setLayoutCount=1u;
    pipeline_layout_info.pSetLayouts=&set_layout;
    RequireVulkanSuccess(device.createPipelineLayout(&pipeline_layout_info,nullptr,&pipeline_layout),"EOP producer layout");
    vk::ComputePipelineCreateInfo create{};create.layout=pipeline_layout;
    create.stage.stage=vk::ShaderStageFlagBits::eCompute;create.stage.module=module;create.stage.pName="main";
    RequireVulkanSuccess(device.createComputePipelines(nullptr,1u,&create,nullptr,&pipeline),"EOP compute producer pipeline");
  }
  ~BdaUploadNativeConsumer() {
    device.destroyPipeline(pipeline,nullptr);device.destroyPipelineLayout(pipeline_layout,nullptr);
    device.destroyDescriptorPool(pool,nullptr);device.destroyDescriptorSetLayout(set_layout,nullptr);
    device.destroyShaderModule(module,nullptr);
  }
  void Record(vk::CommandBuffer command) {
    command.bindPipeline(vk::PipelineBindPoint::eCompute,pipeline);
    command.bindDescriptorSets(vk::PipelineBindPoint::eCompute,pipeline_layout,0u,1u,&set,0u,nullptr);
    command.dispatch(groups,1u,1u);
  }
private:
  uint32_t groups=0;
  vk::Device device;vk::ShaderModule module=nullptr;vk::DescriptorPool pool=nullptr;
  vk::DescriptorSetLayout set_layout=nullptr;vk::DescriptorSet set=nullptr;
  vk::PipelineLayout pipeline_layout=nullptr;vk::Pipeline pipeline=nullptr;
};
struct BdaUploadNativeBudget {
  std::array<VkBuffer,2> destinations{};uint32_t pre=0,post=0,calls=0,regions=0;uint64_t bytes=0;
  static inline thread_local BdaUploadNativeBudget* active=nullptr;
  static inline PFN_vkCmdPipelineBarrier original_barrier=nullptr;
  static inline PFN_vkCmdCopyBuffer original_copy=nullptr;
  bool Target(VkBuffer b) const{return b==destinations[0]||b==destinations[1];}
  static void VKAPI_PTR Barrier(VkCommandBuffer command,VkPipelineStageFlags src,VkPipelineStageFlags dst,
      VkDependencyFlags flags,uint32_t mc,const VkMemoryBarrier* memory,uint32_t bc,const VkBufferMemoryBarrier* buffers,
      uint32_t ic,const VkImageMemoryBarrier* images) {
    if(active!=nullptr){bool before=false,after=false;
      for(uint32_t i=0;i<bc;++i)if(active->Target(buffers[i].buffer)){
        before|=(buffers[i].dstAccessMask&VK_ACCESS_TRANSFER_WRITE_BIT)!=0;
        after|=(buffers[i].srcAccessMask&VK_ACCESS_TRANSFER_WRITE_BIT)!=0&&(buffers[i].dstAccessMask&VK_ACCESS_MEMORY_READ_BIT)!=0;}
      for(uint32_t i=0;i<mc;++i){before|=(memory[i].dstAccessMask&VK_ACCESS_TRANSFER_WRITE_BIT)!=0;
        after|=(memory[i].srcAccessMask&VK_ACCESS_TRANSFER_WRITE_BIT)!=0&&(memory[i].dstAccessMask&VK_ACCESS_MEMORY_READ_BIT)!=0;}
      if(before&&(src&VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)!=0&&(dst&VK_PIPELINE_STAGE_TRANSFER_BIT)!=0)++active->pre;
      if(after&&(src&VK_PIPELINE_STAGE_TRANSFER_BIT)!=0&&(dst&VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)!=0)++active->post;
    }
    original_barrier(command,src,dst,flags,mc,memory,bc,buffers,ic,images);
  }
  static void VKAPI_PTR Copy(VkCommandBuffer command,VkBuffer source,VkBuffer target,uint32_t count,const VkBufferCopy* ranges) {
    if(active!=nullptr&&active->Target(target)){++active->calls;active->regions+=count;for(uint32_t i=0;i<count;++i)active->bytes+=ranges[i].size;}
    original_copy(command,source,target,count,ranges);
  }
  struct Scope {
    explicit Scope(BdaUploadNativeBudget& budget){EXIT_IF(active!=nullptr||original_barrier!=nullptr||original_copy!=nullptr);
      original_barrier=VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier;original_copy=VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdCopyBuffer;
      EXIT_IF(original_barrier==nullptr||original_copy==nullptr);VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier=Barrier;
      VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdCopyBuffer=Copy;active=&budget;}
    ~Scope(){active=nullptr;VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier=original_barrier;
      VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdCopyBuffer=original_copy;original_barrier=nullptr;original_copy=nullptr;}
  };
};
void CheckPrepareBdaUploadBatchCost(VulkanHarness& vulkan) {
  constexpr const char* name="PrepareBdaUploadBatchCost";constexpr uint32_t owner_bytes=0x4000u,words=owner_bytes/4u;
  constexpr uint64_t a=FetchGpuGuest::Base+0x4000u,b=FetchGpuGuest::Base+0xc000u;
  constexpr uint32_t guard=0x5a5a5a5au;
  struct Result{bool setup=false,initial=false,dirty=false,clean=false,native=false,repeated=false,rewrite=false;
    BdaUploadNativeBudget budget{};uint64_t before=0,after=0;};
  std::array<Result,3> results{};constexpr std::array labels{"normal-ring","actual-ring-wrap","temporary-upload"};
  Require(name,"existing guest-fault route",Common::HostException::InstallHandler([](const Common::HostException::ExceptionInfo& i){
    if(i.type!=Common::HostException::ExceptionType::AccessViolation||
       (i.access_violation_type!=Common::HostException::AccessViolationType::Read&&i.access_violation_type!=Common::HostException::AccessViolationType::Write))return false;
    return LibKernel::Memory::HandleGpuFault(i.access_violation_type==Common::HostException::AccessViolationType::Write?PageFaultAccess::Write:PageFaultAccess::Read,i.access_violation_vaddr);
  }),"bootstrap failed; NOT RED");
  {
    FetchGpuGuest guest;auto& graphics=vulkan.RuntimeContext();auto& renderer=vulkan.RuntimeRenderer();
    renderer.InitializeGpu(nullptr);LibKernel::Memory::InstallGpuResources(&renderer);renderer.MapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    auto& gpu=renderer.GetGpu();auto& cache=renderer.GetBufferCache();auto& scheduler=renderer.GetCommandScheduler();
    std::unique_ptr<CommandProcessor> cp;Buffer* first=nullptr;Buffer* second=nullptr;
    std::vector<uint32_t> initial(words,guard),expected(words*2u,guard);
    FetchGpuBuffer output(graphics,owner_bytes*2u,vk::BufferUsageFlagBits::eStorageBuffer);output.Flush();
    gpu.SendCommandSync([&]{cp=std::make_unique<CommandProcessor>(renderer,0x31);cp->Reset();cp->BufferInit();
      std::memcpy(reinterpret_cast<void*>(a),initial.data(),owner_bytes);std::memcpy(reinterpret_cast<void*>(b),initial.data(),owner_bytes);
      first=cache.ObtainBuffer(a,owner_bytes,true,true).first;second=cache.ObtainBuffer(b,owner_bytes,true,true).first;
      // Public written bindings force genuine cache owners; actual finite GPU fills justify ownership.
      first->Fill(first->Offset(a),owner_bytes,guard);second->Fill(second->Offset(b),owner_bytes,guard);cache.OnCommandRecorded();
      renderer.AdvanceBdaEpoch();renderer.PrepareBda();scheduler.Finish();scheduler.DrainPriorityOperations();
      cache.ReadMemory(a,owner_bytes);cache.ReadMemory(b,owner_bytes);scheduler.Finish();scheduler.DrainPriorityOperations();});
    BdaUploadNativeConsumer reader(vulkan.Device(),first->Handle(),second->Handle(),output.buffer,words);
    const auto consume=[&](const std::vector<uint32_t>& golden){
      reader.Record(scheduler.Current().Handle()); // NO extra transfer->compute pre-barrier: actual upload visibility must suffice.
      const auto native=scheduler.Current().Handle();vk::MemoryBarrier host{};host.srcAccessMask=vk::AccessFlagBits::eShaderWrite;host.dstAccessMask=vk::AccessFlagBits::eHostRead;
      native.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,vk::PipelineStageFlagBits::eHost,{},1u,&host,0u,nullptr,0u,nullptr);
      scheduler.Finish();scheduler.DrainPriorityOperations();output.Invalidate();
      return std::memcmp(output.data,golden.data(),golden.size()*4u)==0;
    };
    for(uint32_t test=0;test<results.size();++test){auto& r=results[test];
      gpu.SendCommandSync([&]{
        scheduler.Finish();scheduler.DrainPriorityOperations();
        // Existing friend only replaces an idle host staging ring, preserving actual Map/Commit/Wait behavior.
        const uint64_t ring=test==0u?4u*1024u*1024u:test==1u?24u*1024u:8u*1024u;
        PerformanceMemoryTestAccess::RecreateUploadRing(cache,ring);
        r.setup=GuestGpu::IsGpuThread()&&!CommandRecordingDeferred()&&!DrainStats::Enabled()&&
          first!=second&&first->CpuAddress()==a&&second->CpuAddress()==b&&first->Size()==owner_bytes&&second->Size()==owner_bytes;
        r.initial=consume(expected);
        // Genuine CPU writes, real invalidation and new BDA epoch: no direct tracker-state fabrication.
        cache.InvalidateMemory(a,owner_bytes);cache.InvalidateMemory(b,owner_bytes);
        for(uint32_t i=32u;i<48u;++i){expected[i]=0x12340000u+test*0x100u+i;expected[words+i]=0x56780000u+test*0x100u+i;}
        std::memcpy(reinterpret_cast<void*>(a),expected.data(),owner_bytes);std::memcpy(reinterpret_cast<void*>(b),expected.data()+words,owner_bytes);
        r.dirty=cache.IsRegionCpuModified(a,owner_bytes)&&cache.IsRegionCpuModified(b,owner_bytes);
        renderer.AdvanceBdaEpoch();r.budget.destinations={static_cast<VkBuffer>(first->Handle()),static_cast<VkBuffer>(second->Handle())};
        r.before=scheduler.CurrentTick();{BdaUploadNativeBudget::Scope observe(r.budget);renderer.PrepareBda();}r.after=scheduler.CurrentTick();
        r.clean=!cache.IsRegionCpuModified(a,owner_bytes)&&!cache.IsRegionCpuModified(b,owner_bytes);r.native=consume(expected);
        BdaUploadNativeBudget repeat;repeat.destinations=r.budget.destinations;
        renderer.AdvanceBdaEpoch();{BdaUploadNativeBudget::Scope observe(repeat);renderer.PrepareBda();}
        r.repeated=repeat.calls==0u&&repeat.bytes==0u&&repeat.pre==0u&&repeat.post==0u&&consume(expected);
        // Real second CPU generation: old prepared bytes cannot be reused.
        cache.InvalidateMemory(a,owner_bytes);expected[40u]^=0x00abcdefu;
        std::memcpy(reinterpret_cast<void*>(a),expected.data(),owner_bytes);renderer.AdvanceBdaEpoch();
        BdaUploadNativeBudget rewrite;rewrite.destinations=r.budget.destinations;
        {BdaUploadNativeBudget::Scope observe(rewrite);renderer.PrepareBda();}
        r.rewrite=rewrite.calls==1u&&rewrite.bytes==owner_bytes&&rewrite.pre==1u&&rewrite.post==1u&&consume(expected);
      });
    }
    renderer.UnmapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);renderer.ShutdownGpu();LibKernel::Memory::InstallGpuResources(nullptr);cp.reset();
  }
  for(uint32_t test=0;test<results.size();++test){const auto& r=results[test];
    std::printf("[BDA-upload-batch] %s setup=%u initial=%u dirty=%u clean=%u nativeAllWordsGuards=%u repeated=%u rewrite=%u pre=%u post=%u copies=%u regions=%u bytes=%llu tick=%llu->%llu\n",
      labels[test],r.setup,r.initial,r.dirty,r.clean,r.native,r.repeated,r.rewrite,r.budget.pre,r.budget.post,r.budget.calls,r.budget.regions,
      static_cast<unsigned long long>(r.budget.bytes),static_cast<unsigned long long>(r.before),static_cast<unsigned long long>(r.after));
    Require(labels[test],"ALL real native SSBO words/guards, CPU generations, ring/temporary cleanup BEFORE cost",
      r.setup&&r.initial&&r.dirty&&r.clean&&r.native&&r.repeated&&r.rewrite&&r.budget.calls==2u&&r.budget.regions==2u&&r.budget.bytes==owner_bytes*2u&&
      r.after==r.before+(test==1u?1u:0u),"producer/native bytes/guards/generation/real ring-wrap lifetime failed; NOT valid cost RED");
  }
  for(uint32_t test=0;test<results.size();++test){const auto& r=results[test];const uint32_t pairs=test==1u?2u:1u;
    Require(labels[test],"independent upload dependency pairs batched only within same real tick",
      r.budget.pre==pairs&&r.budget.post==pairs,"per-owner transfer barriers remain redundant, or a mandatory ring boundary was dropped");}
}
} // namespace
} // namespace Libs::Graphics
