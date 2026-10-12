// Actual production fault-parser SPIR-V; bounded synthetic bitmap and conservation oracle.
#include "gpu_tiler_shaders/fault_buffer_process_spv.h"
#include "spirv-tools/libspirv.hpp"

namespace Libs::Graphics {
namespace {
void CheckFaultBufferProtocolGpu(VulkanHarness& vulkan) {
  constexpr const char* name="FaultBufferProtocolGpu";
  constexpr uint32_t bitmap_words=128u,records=1024u,capacity=records-1u;
  constexpr uint32_t requested=2049u,page_shift=14u,max_passes=4u,guard_words=16u;
  constexpr uint64_t guard=0x9a8b7c6d5e4f3021ull;
  struct Observation {uint32_t count=0u,pending=0u,exported=0u;bool bounded=true,valid=true,
    guards=true,conserved=true;};
  std::array<Observation,max_passes> observed{};
  uint32_t passes=0u,total=0u;
  {
    FetchGpuBuffer bitmap(vulkan.RuntimeContext(),bitmap_words*4u+guard_words*8u,
        vk::BufferUsageFlagBits::eStorageBuffer);
    FetchGpuBuffer output(vulkan.RuntimeContext(),records*8u+guard_words*8u,
        vk::BufferUsageFlagBits::eStorageBuffer);
    auto* input=static_cast<uint32_t*>(bitmap.data);
    for(uint32_t page=1u;page<=requested;++page)input[page/32u]|=1u<<(page%32u);
    auto* input_guard=reinterpret_cast<uint64_t*>(input+bitmap_words);
    auto* exported=static_cast<uint64_t*>(output.data);
    for(uint32_t i=0u;i<guard_words;++i)input_guard[i]=guard;
    bitmap.Flush();
    spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_3);
    Require(name,"actual production SPIR-V validation",
        validator.Validate(FAULT_BUFFER_PROCESS_SPV,std::size(FAULT_BUFFER_PROCESS_SPV)),
        "generated fault parser module invalid; setup failure, not the quota RED");
    vk::Device device=vulkan.Device();
    vk::ShaderModuleCreateInfo module_info{};module_info.codeSize=sizeof(FAULT_BUFFER_PROCESS_SPV);
    module_info.pCode=FAULT_BUFFER_PROCESS_SPV;
    vk::ShaderModule module=nullptr;
    RequireVulkanSuccess(device.createShaderModule(&module_info,nullptr,&module),"fault protocol module");
    std::array<vk::DescriptorSetLayoutBinding,2> bindings{};
    for(uint32_t i=0u;i<bindings.size();++i) {
      bindings[i].binding=i;bindings[i].descriptorCount=1u;
      bindings[i].descriptorType=vk::DescriptorType::eStorageBuffer;
      bindings[i].stageFlags=vk::ShaderStageFlagBits::eCompute;
    }
    vk::DescriptorSetLayoutCreateInfo set_info{};set_info.bindingCount=bindings.size();set_info.pBindings=bindings.data();
    vk::DescriptorSetLayout set_layout=nullptr;
    RequireVulkanSuccess(device.createDescriptorSetLayout(&set_info,nullptr,&set_layout),"fault protocol set layout");
    vk::DescriptorPoolSize pool_size{vk::DescriptorType::eStorageBuffer,2u};
    vk::DescriptorPoolCreateInfo pool_info{};pool_info.maxSets=1u;pool_info.poolSizeCount=1u;pool_info.pPoolSizes=&pool_size;
    vk::DescriptorPool pool=nullptr;
    RequireVulkanSuccess(device.createDescriptorPool(&pool_info,nullptr,&pool),"fault protocol descriptor pool");
    vk::DescriptorSetAllocateInfo allocation{};allocation.descriptorPool=pool;allocation.descriptorSetCount=1u;
    allocation.pSetLayouts=&set_layout;
    vk::DescriptorSet set=nullptr;
    RequireVulkanSuccess(device.allocateDescriptorSets(&allocation,&set),"fault protocol descriptors");
    std::array<vk::DescriptorBufferInfo,2> buffers{{{bitmap.buffer,0u,bitmap_words*4u},
                                                  {output.buffer,0u,records*8u}}};
    std::array<vk::WriteDescriptorSet,2> writes{};
    for(uint32_t i=0u;i<writes.size();++i) {
      writes[i].dstSet=set;writes[i].dstBinding=i;writes[i].descriptorCount=1u;
      writes[i].descriptorType=vk::DescriptorType::eStorageBuffer;writes[i].pBufferInfo=&buffers[i];
    }
    device.updateDescriptorSets(writes.size(),writes.data(),0u,nullptr);
    vk::PipelineLayoutCreateInfo layout_info{};layout_info.setLayoutCount=1u;layout_info.pSetLayouts=&set_layout;
    vk::PipelineLayout layout=nullptr;
    RequireVulkanSuccess(device.createPipelineLayout(&layout_info,nullptr,&layout),"fault protocol pipeline layout");
    vk::ComputePipelineCreateInfo create{};create.layout=layout;
    create.stage.stage=vk::ShaderStageFlagBits::eCompute;create.stage.module=module;create.stage.pName="main";
    vk::Pipeline pipeline=nullptr;
    RequireVulkanSuccess(device.createComputePipelines(nullptr,1u,&create,nullptr,&pipeline),"fault protocol compute pipeline");
    auto& scheduler=vulkan.RuntimeRenderer().GetCommandScheduler();
    HW::Context registers{};HW::UserConfig config{};HW::Shader shaders{};
    scheduler.Begin(registers,config,shaders);
    std::array<bool,bitmap_words*32u> seen{};
    for(uint32_t pass=0u;pass<max_passes;++pass) {
      auto& result=observed[pass];passes=pass+1u;
      std::fill(exported,exported+records+guard_words,guard);exported[0]=0u;output.Flush();
      auto command=scheduler.Current().Handle();
      vk::MemoryBarrier ready{};ready.srcAccessMask=vk::AccessFlagBits::eHostWrite;
      ready.dstAccessMask=vk::AccessFlagBits::eShaderRead|vk::AccessFlagBits::eShaderWrite;
      command.pipelineBarrier(vk::PipelineStageFlagBits::eHost,vk::PipelineStageFlagBits::eComputeShader,
          {},1u,&ready,0u,nullptr,0u,nullptr);
      command.bindPipeline(vk::PipelineBindPoint::eCompute,pipeline);
      command.bindDescriptorSets(vk::PipelineBindPoint::eCompute,layout,0u,1u,&set,0u,nullptr);
      // Exactly128 invocations address exactly128 allocated input words; max32 bits/word.
      command.dispatch(bitmap_words/64u,1u,1u);
      vk::MemoryBarrier done{};done.srcAccessMask=vk::AccessFlagBits::eShaderWrite;
      done.dstAccessMask=vk::AccessFlagBits::eHostRead;
      command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,vk::PipelineStageFlagBits::eHost,
          {},1u,&done,0u,nullptr,0u,nullptr);
      scheduler.Finish();scheduler.WaitPriorityOperations(scheduler.CurrentTick()-1u);
      bitmap.Invalidate();output.Invalidate();
      result.count=static_cast<uint32_t>(exported[0]);result.bounded=result.count<=capacity;
      // Diagnostic inspection is confined to the allocated record area even on an invalid count.
      // The unchanged protocol oracle below still FAILS every oversized count.
      for(uint32_t i=1u;i<=std::min(result.count,capacity);++i) {
        const auto value=exported[i],page=value>>page_shift;
        const bool valid=(value&((1ull<<page_shift)-1u))==0u&&page>=1u&&page<=requested;
        result.valid&=valid;
        if(valid) {result.valid&=!seen[page];seen[page]=true;result.exported++;}
      }
      std::array<bool,bitmap_words*32u> pending{};
      for(uint32_t word=0u;word<bitmap_words;++word)
        for(uint32_t bit=0u;bit<32u;++bit)if((input[word]&(1u<<bit))!=0u) {
          const uint32_t page=word*32u+bit;pending[page]=true;result.pending++;
        }
      for(uint32_t page=0u;page<seen.size();++page) {
        const bool original=page>=1u&&page<=requested;
        result.conserved&=(seen[page]||pending[page])==original&&!(seen[page]&&pending[page]);
      }
      for(uint32_t i=0u;i<guard_words;++i)
        result.guards&=input_guard[i]==guard&&exported[records+i]==guard;
      total+=result.exported;
      std::printf("[gpu] FaultProtocol pass=%u count=%u exported=%u total=%u pending=%u bounded=%u uniqueValid=%u conserved=%u guards=%u\n",
          pass,result.count,result.exported,total,result.pending,result.bounded,result.valid,result.conserved,result.guards);
      if(result.pending==0u||!result.bounded||!result.valid||!result.conserved||!result.guards)break;
    }
    scheduler.Finish();scheduler.WaitPriorityOperations(scheduler.CurrentTick()-1u);
    device.destroyPipeline(pipeline,nullptr);device.destroyPipelineLayout(layout,nullptr);
    device.destroyDescriptorPool(pool,nullptr);device.destroyDescriptorSetLayout(set_layout,nullptr);
    device.destroyShaderModule(module,nullptr);
  } // All native objects drained/disposed before any protocol assertion.
  for(uint32_t pass=0u;pass<passes;++pass) {
    const auto& result=observed[pass];
    Require(name,"bounded output record count",result.bounded,
        "actual fault parser counter exceeded the1023 available records");
    Require(name,"output guard",result.guards,"parser wrote outside its bitmap or output record descriptor");
    Require(name,"unique original fault records",result.valid,"parser exported duplicate or unrequested page bits");
    Require(name,"lossless exported/pending partition",result.conserved,
        "original fault bits disappeared instead of remaining pending after quota exhaustion");
  }
  Require(name,"bounded repeated drain",total==requested&&observed[passes-1u].pending==0u,
      "four bounded dispatches did not export each of2049 original fault bits exactly once");
}
} // namespace
} // namespace Libs::Graphics
