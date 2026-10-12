// Synthetic public-CP EOP neighbors. Native reads consume the actual cache owner.
#include "graphics/shader/recompiler/backend/spirv/SpirvBuilder.h"
#include "kernel/eventQueue.h"
#include "libs/errno.h"

namespace Libs::Graphics {
namespace {
std::vector<uint32_t> EopComputeProducerSpirv(uint32_t marker) {
  using ShaderRecompiler::Spirv::Builder;
  Builder b(0x00010500u);
  b.RequireCapability(spv::CapabilityShader);
  b.AddMemoryModel(spv::AddressingModelLogical,spv::MemoryModelGLSL450);
  const auto v=b.Type(spv::OpTypeVoid),u=b.Type(spv::OpTypeInt,32u,0u);
  const auto zero=b.Constant(spv::OpConstant,u,0u),value=b.Constant(spv::OpConstant,u,marker);
  const auto words=b.Type(spv::OpTypeRuntimeArray,u);
  b.AddAnnotation(spv::OpDecorate,words,spv::DecorationArrayStride,4u);
  const auto block=b.Type(spv::OpTypeStruct,words);
  b.AddAnnotation(spv::OpDecorate,block,spv::DecorationBlock);
  b.AddAnnotation(spv::OpMemberDecorate,block,0u,spv::DecorationOffset,0u);
  const auto pointer=b.Type(spv::OpTypePointer,spv::StorageClassStorageBuffer,block);
  const auto element=b.Type(spv::OpTypePointer,spv::StorageClassStorageBuffer,u);
  const auto output=b.DefineGlobalVariable(pointer,spv::StorageClassStorageBuffer);
  b.AddAnnotation(spv::OpDecorate,output,spv::DecorationDescriptorSet,0u);
  b.AddAnnotation(spv::OpDecorate,output,spv::DecorationBinding,0u);
  const auto fn=b.Type(spv::OpTypeFunction,v),entry=b.AllocateId();
  b.AddEntryPoint(spv::ExecutionModelGLCompute,entry,"main",{output});
  b.AddExecutionMode(entry,spv::ExecutionModeLocalSize,1u,1u,1u);
  b.AddFunction(spv::OpFunction,v,entry,spv::FunctionControlMaskNone,fn);
  b.AddFunction(spv::OpLabel,b.AllocateId());
  const auto address=b.AllocateId();
  b.AddFunction(spv::OpAccessChain,element,address,output,zero,zero);
  b.AddFunction(spv::OpStore,address,value);
  b.AddFunction(spv::OpReturn);
  b.AddFunction(spv::OpFunctionEnd);
  return b.Build();
}
class EopComputeProducer {
public:
  EopComputeProducer(vk::Device d,vk::Buffer output,uint32_t marker):device(d) {
    const auto spirv=EopComputeProducerSpirv(marker);
    spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_3);
    Require("EopLiteralNeighbors","producer SPIR-V validation",validator.Validate(spirv),
            "bounded compute producer setup failed; not an EOP semantic failure");
    vk::ShaderModuleCreateInfo shader{};shader.codeSize=spirv.size()*4u;shader.pCode=spirv.data();
    RequireVulkanSuccess(device.createShaderModule(&shader,nullptr,&module),"EOP compute module");
    vk::DescriptorSetLayoutBinding binding{};binding.binding=0u;binding.descriptorCount=1u;
    binding.descriptorType=vk::DescriptorType::eStorageBuffer;binding.stageFlags=vk::ShaderStageFlagBits::eCompute;
    vk::DescriptorSetLayoutCreateInfo layout{};layout.bindingCount=1u;layout.pBindings=&binding;
    RequireVulkanSuccess(device.createDescriptorSetLayout(&layout,nullptr,&set_layout),"EOP producer set layout");
    vk::DescriptorPoolSize size{vk::DescriptorType::eStorageBuffer,1u};
    vk::DescriptorPoolCreateInfo pool_info{};pool_info.maxSets=1u;pool_info.poolSizeCount=1u;pool_info.pPoolSizes=&size;
    RequireVulkanSuccess(device.createDescriptorPool(&pool_info,nullptr,&pool),"EOP producer pool");
    vk::DescriptorSetAllocateInfo allocation{};allocation.descriptorPool=pool;allocation.descriptorSetCount=1u;
    allocation.pSetLayouts=&set_layout;
    RequireVulkanSuccess(device.allocateDescriptorSets(&allocation,&set),"EOP producer descriptor");
    vk::DescriptorBufferInfo info{output,0u,4u};
    vk::WriteDescriptorSet write{};write.dstSet=set;write.dstBinding=0u;write.descriptorCount=1u;
    write.descriptorType=vk::DescriptorType::eStorageBuffer;write.pBufferInfo=&info;
    device.updateDescriptorSets(1u,&write,0u,nullptr);
    vk::PipelineLayoutCreateInfo pipeline_layout_info{};pipeline_layout_info.setLayoutCount=1u;
    pipeline_layout_info.pSetLayouts=&set_layout;
    RequireVulkanSuccess(device.createPipelineLayout(&pipeline_layout_info,nullptr,&pipeline_layout),"EOP producer layout");
    vk::ComputePipelineCreateInfo create{};create.layout=pipeline_layout;
    create.stage.stage=vk::ShaderStageFlagBits::eCompute;create.stage.module=module;create.stage.pName="main";
    RequireVulkanSuccess(device.createComputePipelines(nullptr,1u,&create,nullptr,&pipeline),"EOP compute producer pipeline");
  }
  ~EopComputeProducer() {
    device.destroyPipeline(pipeline,nullptr);device.destroyPipelineLayout(pipeline_layout,nullptr);
    device.destroyDescriptorPool(pool,nullptr);device.destroyDescriptorSetLayout(set_layout,nullptr);
    device.destroyShaderModule(module,nullptr);
  }
  void Record(vk::CommandBuffer command) {
    command.bindPipeline(vk::PipelineBindPoint::eCompute,pipeline);
    command.bindDescriptorSets(vk::PipelineBindPoint::eCompute,pipeline_layout,0u,1u,&set,0u,nullptr);
    command.dispatch(1u,1u,1u);
  }
private:
  vk::Device device;vk::ShaderModule module=nullptr;vk::DescriptorPool pool=nullptr;
  vk::DescriptorSetLayout set_layout=nullptr;vk::DescriptorSet set=nullptr;
  vk::PipelineLayout pipeline_layout=nullptr;vk::Pipeline pipeline=nullptr;
};
struct EopIrqObserver {
  uint64_t address=0u,expected=0u,observed=0u;bool called=false,readable=false;
};
void ObserveEopIrq(LibKernel::EventQueue::KernelEqueueEvent* event,void* data) {
  auto& observer=*static_cast<EopIrqObserver*>(event->event.udata);
  observer.called=true;
  observer.readable=LibKernel::Memory::TryReadBacking(observer.address,&observer.observed,8u);
  event->triggered=true;
  event->event.fflags++;
  event->event.data=reinterpret_cast<intptr_t>(data);
}
void ResetEopIrq(LibKernel::EventQueue::KernelEqueueEvent* event) {
  event->triggered=false;event->event.fflags=0u;event->event.data=0;
}
void CopyEopNativeLabel(CommandScheduler& scheduler,Buffer& source,uint64_t offset,
                        FetchGpuBuffer& readback,uint64_t destination,uint64_t bytes) {
  auto command=scheduler.Current().Handle();
  vk::MemoryBarrier before{};before.srcAccessMask=vk::AccessFlagBits::eMemoryWrite;
  before.dstAccessMask=vk::AccessFlagBits::eTransferRead|vk::AccessFlagBits::eTransferWrite;
  command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,vk::PipelineStageFlagBits::eTransfer,
      {},1u,&before,0u,nullptr,0u,nullptr);
  vk::BufferCopy copy{offset,destination,bytes};
  command.copyBuffer(source.Handle(),readback.buffer,1u,&copy);
  vk::MemoryBarrier after{};after.srcAccessMask=vk::AccessFlagBits::eTransferWrite;
  after.dstAccessMask=vk::AccessFlagBits::eHostRead;
  command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eHost,
      {},1u,&after,0u,nullptr,0u,nullptr);
}
struct EopLiteralNeighborResult {
  bool label_setup=false,irq_setup=false,pending=false,complete=false,backing_read=false;
  bool older_kept_new_dirty=false,clean=false,irq_called=false,irq_readable=false;
  uint64_t before=0u,first_read=0u,last_read=0u,after=0u,irq_backing=0u;
  int irq_status=0,irq_count=0;uintptr_t irq_ident=0u;
};
void CheckEndOfPipeLiteralNeighbors(VulkanHarness& vulkan) {
  constexpr const char* name="EopLiteralNeighbors";
  constexpr uint64_t sentinel=0x8899aabbccddeef0ull,literal=0x1122334455667788ull;
  constexpr uint64_t newer=0xa5b6c7d8e9fa0b1cull;
  constexpr uint32_t marker=0x12345678u;
  std::array<EopLiteralNeighborResult,3> results{};
  {
  FetchGpuGuest guest;
  auto& renderer=vulkan.RuntimeRenderer();auto& scheduler=renderer.GetCommandScheduler();
  auto& cache=renderer.GetBufferCache();
  LibKernel::Memory::InstallGpuResources(&renderer);
  renderer.MapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
  CommandProcessor cp(renderer,0x31);cp.Reset();cp.BufferInit();
  FetchGpuBuffer producer(vulkan.RuntimeContext(),4u,vk::BufferUsageFlagBits::eStorageBuffer|
      vk::BufferUsageFlagBits::eTransferDst);
  FetchGpuBuffer readback(vulkan.RuntimeContext(),16u,vk::BufferUsageFlagBits::eTransferDst);
  EopComputeProducer compute(vulkan.Device(),producer.buffer,marker);
  for(uint32_t test=0u;test<3u;++test) {
    const uint64_t address=FetchGpuGuest::Base+(test+1u)*0x4000u;
    const uint64_t width=test==2u?4u:8u;
    auto* label=reinterpret_cast<uint64_t*>(address);
    std::memcpy(label,&sentinel,8u);
    // Read-only ObtainBuffer may choose the transient stream ring. Register and upload
    // the actual cached owner explicitly before testing an existing clean/dirty label.
    const auto registered_id=cache.FindBuffer(address,width);
    cache.SynchronizeBuffersInRange(address,width);
    auto [registered,registered_offset]=cache.ObtainBuffer(address,width,test==1u,true,registered_id);
    if(test==1u) {
      // A real prior GPU write owns this label before the public EOP call.
      auto command=scheduler.Current().Handle();
      vk::MemoryBarrier before{};before.srcAccessMask=vk::AccessFlagBits::eMemoryWrite;
      before.dstAccessMask=vk::AccessFlagBits::eTransferWrite;
      command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,vk::PipelineStageFlagBits::eTransfer,
          {},1u,&before,0u,nullptr,0u,nullptr);
      command.updateBuffer(registered->Handle(),registered_offset,8u,&sentinel);
      cache.OnCommandRecorded();
    }
    const bool label_setup=cache.IsRegionRegistered(address,width)&&
        cache.IsRegionGpuModified(address,width)==(test==1u);
    std::memset(producer.data,0,producer.size);producer.Flush();
    std::memset(readback.data,0,readback.size);readback.Flush();
    auto command=scheduler.Current().Handle();
    vk::MemoryBarrier ready{};ready.srcAccessMask=vk::AccessFlagBits::eHostWrite;
    ready.dstAccessMask=vk::AccessFlagBits::eShaderWrite|vk::AccessFlagBits::eTransferWrite;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eHost,vk::PipelineStageFlagBits::eAllCommands,
        {},1u,&ready,0u,nullptr,0u,nullptr);
    if(test==2u)compute.Record(command);else command.fillBuffer(producer.buffer,0u,4u,marker);
    const auto tick=scheduler.CurrentTick();
    EopIrqObserver observer{address,literal};
    LibKernel::EventQueue::KernelEqueue queue=LibKernel::EventQueue::KERNEL_EQUEUE_INVALID;
    bool irq_setup=true;
    if(test==0u) {
      irq_setup=LibKernel::EventQueue::KernelCreateEqueue(&queue,"eop-publication") == 0;
      LibKernel::EventQueue::KernelEqueueEvent event{};
      event.event.ident=0x31u;event.event.filter=LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS;
      event.event.udata=&observer;event.filter.trigger_func=ObserveEopIrq;event.filter.reset_func=ResetEopIrq;
      irq_setup=irq_setup&&LibKernel::EventQueue::KernelAddEvent(queue,event)==0;
      renderer.AddInterruptEq(queue,0x31);
    }
    if(test==2u)cp.WriteAtEndOfPipe32(0u,0u,0x2fu,0u,6u,2u,label,
        static_cast<uint32_t>(literal),0u);
    else cp.WriteAtEndOfPipe64(0u,0u,0x28u,0u,0u,2u,label,literal,test==0u?2u:0u);
    auto [first,first_offset]=cache.ObtainBuffer(address,width,false,true);
    CopyEopNativeLabel(scheduler,*first,first_offset,readback,0u,width);
    bool older_kept_new_dirty=true;
    if(test==1u) {
      scheduler.DeferPriorityOperation([&] {older_kept_new_dirty=cache.IsRegionGpuModified(address,width);});
      cp.WriteAtEndOfPipe64(0u,0u,0x28u,0u,0u,2u,label,newer,0u);
      auto [last,last_offset]=cache.ObtainBuffer(address,width,false,true);
      CopyEopNativeLabel(scheduler,*last,last_offset,readback,8u,width);
    }
    uint64_t before=0u;
    const bool peek=LibKernel::Memory::TryReadBacking(address,&before,width);
    const bool pending=peek&&before==(width==4u?static_cast<uint32_t>(sentinel):sentinel)&&
        scheduler.CurrentTick()==tick&&!scheduler.IsFree(tick)&&!observer.called;
    scheduler.Finish();scheduler.WaitPriorityOperations(tick);
    producer.Invalidate();readback.Invalidate();
    uint64_t after=0u,first_read=0u,last_read=0u;
    const bool backing_read=LibKernel::Memory::TryReadBacking(address,&after,width);
    std::memcpy(&first_read,readback.data,width);
    if(test==1u)std::memcpy(&last_read,static_cast<uint8_t*>(readback.data)+8u,width);
    const bool complete=scheduler.IsFree(tick)&&*static_cast<const uint32_t*>(producer.data)==marker;
    LibKernel::EventQueue::KernelEvent event{};int count=0;LibKernel::KernelUseconds timeout=0;
    int irq_status=0;
    if(test==0u) {
      irq_status=LibKernel::EventQueue::KernelWaitEqueue(queue,&event,1,&count,&timeout);
      renderer.DeleteInterruptEq(queue,0x31);LibKernel::EventQueue::KernelDeleteEqueue(queue);
    }
    std::printf("[gpu] EopLiteralNeighbor%u registered=%u pending=%u producer=%u before=%016llx firstNative=%016llx lastNative=%016llx backing=%016llx newerDirty=%u irqCalled=%u irqBacking=%016llx\n",
        test,label_setup,pending,complete,static_cast<unsigned long long>(before),
        static_cast<unsigned long long>(first_read),static_cast<unsigned long long>(last_read),
        static_cast<unsigned long long>(after),older_kept_new_dirty,observer.called,
        static_cast<unsigned long long>(observer.observed));
    auto& result=results[test];
    result.label_setup=label_setup;result.irq_setup=irq_setup;result.pending=pending;
    result.complete=complete;result.backing_read=backing_read;
    result.older_kept_new_dirty=older_kept_new_dirty;
    result.clean=!cache.HasGpuDirtyBytes(address,width);
    result.irq_called=observer.called;result.irq_readable=observer.readable;
    result.before=before;result.first_read=first_read;result.last_read=last_read;
    result.after=after;result.irq_backing=observer.observed;result.irq_status=irq_status;
    result.irq_count=count;result.irq_ident=event.ident;
  }
  scheduler.Finish();scheduler.WaitPriorityOperations(scheduler.CurrentTick()-1u);
  renderer.UnmapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
  LibKernel::Memory::InstallGpuResources(nullptr);
  } // Dispose native producer, readback, descriptors and guest mapping before assertions.
  for(uint32_t test=0u;test<results.size();++test) {
    const auto& result=results[test];
    const uint64_t expected_first=test==2u?static_cast<uint32_t>(literal):literal;
    const uint64_t expected_final=test==1u?newer:expected_first;
    Require(name,"actual registered/dirty/pending/completed setup",
        result.label_setup&&result.irq_setup&&result.pending&&result.complete,
        "native producer/setup failed; not an EOP semantic result");
    Require(name,"subsequent native GPU label read",result.first_read==expected_first&&
        (test!=1u||result.last_read==newer),"real cached native label did not feed the following GPU read");
    Require(name,"ordered latest backing ownership",result.backing_read&&result.after==expected_final&&
        result.older_kept_new_dirty&&result.clean,
        "old publication cleared new ownership or final backing differs from the latest GPU write");
    Require(name,"IRQ after actual publication",test!=0u||
        (result.irq_called&&result.irq_readable&&result.irq_backing==literal&&result.irq_status==0&&
         result.irq_count==1&&result.irq_ident==0x31u),
        "graphics interrupt ran before its actual guest backing publication");
  }
}
} // namespace
} // namespace Libs::Graphics
