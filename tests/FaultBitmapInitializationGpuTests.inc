// Test-only allocation identities/raw resources; no initializer or fake resource state.
namespace Libs::Graphics {
struct FaultBitmapInitializationTestAccess {
  static VmaAllocationInfo Allocation(const Buffer& buffer) {
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(buffer.m_graphics->allocator,buffer.m_allocation,&info);
    return info;
  }
  static Buffer& RawBitmap(FaultManager& manager) {return manager.m_fault_buffer;}
};
struct BufferCacheTestAccess {
  static FaultManager& Faults(BufferCache& cache) {return cache.m_fault_manager;}
};
namespace {
std::vector<uint32_t> FaultBitmapProducerSpirv(uint32_t marker,uint32_t word) {
  using ShaderRecompiler::Spirv::Builder;
  Builder b(0x00010500u);
  b.RequireCapability(spv::CapabilityShader);
  b.AddMemoryModel(spv::AddressingModelLogical,spv::MemoryModelGLSL450);
  const auto v=b.Type(spv::OpTypeVoid),u=b.Type(spv::OpTypeInt,32u,0u);
  const auto zero=b.Constant(spv::OpConstant,u,0u),value=b.Constant(spv::OpConstant,u,marker);
  const auto index=b.Constant(spv::OpConstant,u,word);
  const auto scope=b.Constant(spv::OpConstant,u,1u);
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
  b.AddFunction(spv::OpAccessChain,element,address,output,zero,index);
  b.AddFunction(spv::OpAtomicOr,u,b.AllocateId(),address,scope,zero,value);
  b.AddFunction(spv::OpReturn);
  b.AddFunction(spv::OpFunctionEnd);
  return b.Build();
}
class FaultBitmapProducer {
public:
  FaultBitmapProducer(vk::Device d,vk::Buffer output,uint64_t bytes,uint32_t word,uint32_t marker):device(d) {
    const auto spirv=FaultBitmapProducerSpirv(marker,word);
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
    vk::DescriptorBufferInfo info{output,0u,bytes};
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
  ~FaultBitmapProducer() {
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

void CheckFaultBitmapInitializationGpu(VulkanHarness& vulkan) {
  constexpr const char* name="FaultBitmapInitializationGpu";
  constexpr uint64_t guest_base=0x0000000201500000ull,guest_bytes=0x20000u;
  constexpr uint64_t poison_address=guest_base,current_address=guest_base+0x10000u;
  constexpr uint32_t poison_page=static_cast<uint32_t>(poison_address>>BufferCache::CACHING_PAGEBITS);
  constexpr uint32_t current_page=static_cast<uint32_t>(current_address>>BufferCache::CACHING_PAGEBITS);
  constexpr uint64_t poison_offset=uint64_t{poison_page/32u}*4u;
  constexpr uint32_t poison_value=1u<<(poison_page%32u),current_value=1u<<(current_page%32u);
  bool pinned_block=false,seeded=false,reused=false,clean_poison=false,present_current=false;
  uint64_t requested_bytes=0u;
  {
    auto& graphics=vulkan.RuntimeContext();
    auto& prior=vulkan.RuntimeRenderer();auto& prior_scheduler=prior.GetCommandScheduler();
    HW::Context prior_registers{};HW::UserConfig prior_config{};HW::Shader prior_shaders{};
    prior_scheduler.Begin(prior_registers,prior_config,prior_shaders);
    auto& original_manager=BufferCacheTestAccess::Faults(prior.GetBufferCache());
    requested_bytes=FaultBitmapInitializationTestAccess::RawBitmap(original_manager).Size();
    Buffer anchor(graphics,prior_scheduler,MemoryUsage::DeviceLocal,0u,AllFlags,4096u);
    const auto anchor_info=FaultBitmapInitializationTestAccess::Allocation(anchor);
    VmaAllocationInfo previous{};
    FetchGpuBuffer probe(graphics,4u,vk::BufferUsageFlagBits::eTransferDst);
    {
      Buffer predecessor(graphics,prior_scheduler,MemoryUsage::DeviceLocal,0u,AllFlags,requested_bytes);
      previous=FaultBitmapInitializationTestAccess::Allocation(predecessor);
      pinned_block=previous.deviceMemory==anchor_info.deviceMemory;
      predecessor.Fill(0u,requested_bytes,0u);
      prior_scheduler.Current().Handle().updateBuffer(predecessor.Handle(),poison_offset,4u,&poison_value);
      CopyEopNativeLabel(prior_scheduler,predecessor,poison_offset,probe,0u,4u);
      prior_scheduler.Finish();prior_scheduler.WaitPriorityOperations(prior_scheduler.CurrentTick()-1u);
      probe.Invalidate();seeded=*static_cast<const uint32_t*>(probe.data)==poison_value;
    } // Free the completed predecessor; anchor keeps its real VMA block alive.
    {
      RenderContext tested(graphics);
      auto& cache=tested.GetBufferCache();
      auto& manager=BufferCacheTestAccess::Faults(cache);
      auto& raw=FaultBitmapInitializationTestAccess::RawBitmap(manager);
      const auto fresh=FaultBitmapInitializationTestAccess::Allocation(raw);
      reused=pinned_block&&fresh.deviceMemory==previous.deviceMemory&&fresh.offset==previous.offset&&
          fresh.size>=requested_bytes&&raw.Size()==requested_bytes;
      std::printf("[host] FaultInit predecessorMemory=%p offset=%llu size=%llu freshMemory=%p offset=%llu size=%llu pinned=%u seeded=%u exactReuse=%u\n",
          static_cast<void*>(previous.deviceMemory),static_cast<unsigned long long>(previous.offset),
          static_cast<unsigned long long>(previous.size),static_cast<void*>(fresh.deviceMemory),
          static_cast<unsigned long long>(fresh.offset),static_cast<unsigned long long>(fresh.size),
          pinned_block,seeded,reused);
      // Never dispatch a parser over uncontrolled uninitialized memory on a setup mismatch.
      if(reused&&seeded) {
        int64_t direct=-1;
        Require(name,"real mapped guest allocation",LibKernel::Memory::KernelAllocateDirectMemory(0,
            LibKernel::Memory::KernelGetDirectMemorySize(),guest_bytes,0x10000u,0,&direct)==0,
            "owned guest page allocation failed; setup failure, not initialization RED");
        void* mapped=reinterpret_cast<void*>(guest_base);
        Require(name,"real mapped guest region",LibKernel::Memory::KernelMapDirectMemory(&mapped,
            guest_bytes,0x3,0x10,direct,0x10000u)==0&&mapped==reinterpret_cast<void*>(guest_base),
            "owned guest pages failed to map; setup failure, not initialization RED");
        std::memset(mapped,0,guest_bytes);
        LibKernel::Memory::InstallGpuResources(&tested);tested.MapMemory(guest_base,guest_bytes);
        HW::Context registers{};HW::UserConfig config{};HW::Shader shaders{};
        auto& scheduler=tested.GetCommandScheduler();scheduler.Begin(registers,config,shaders);
        // This is the real accessor used by the descriptor binder. Reuse observation above
        // deliberately used raw metadata so a future lazy first-use initializer is not triggered early.
        auto* bound=cache.GetFaultBuffer();
        FaultBitmapProducer producer(vulkan.Device(),bound->Handle(),bound->Size(),
            current_page/32u,current_value);
        auto command=scheduler.Current().Handle();
        vk::MemoryBarrier ready{};ready.srcAccessMask=vk::AccessFlagBits::eMemoryWrite;
        ready.dstAccessMask=vk::AccessFlagBits::eShaderRead|vk::AccessFlagBits::eShaderWrite;
        command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
            vk::PipelineStageFlagBits::eComputeShader,{},1u,&ready,0u,nullptr,0u,nullptr);
        producer.Record(command);
        // Actual parser/host callback: a first-Process clear would lose this legitimate bit.
        cache.ProcessFaultBuffer();
        scheduler.Finish();scheduler.WaitPriorityOperations(scheduler.CurrentTick()-1u);
        scheduler.PopPendingOperations();
        clean_poison=!cache.IsRegionRegistered(poison_address,1u);
        present_current=cache.IsRegionRegistered(current_address,1u);
        std::printf("[gpu] FaultInit poisonRegistered=%u currentRegistered=%u\n",!clean_poison,present_current);
        tested.UnmapMemory(guest_base,guest_bytes);
        LibKernel::Memory::InstallGpuResources(nullptr);
        Require(name,"owned guest cleanup",LibKernel::Memory::KernelMunmap(guest_base,guest_bytes)==0&&
            LibKernel::Memory::KernelReleaseDirectMemory(direct,guest_bytes)==0,
            "owned guest mapping cleanup failed");
      }
    } // The actual second renderer and all callbacks are gone before assertions.
    prior_scheduler.Finish();prior_scheduler.WaitPriorityOperations(prior_scheduler.CurrentTick()-1u);
  }
  Require(name,"verified completed VMA predecessor reuse",pinned_block&&seeded&&reused,
      "actual live VMA allocation did not reuse the controlled predecessor; setup failure, not a valid RED");
  Require(name,"fresh bitmap excludes predecessor fault",clean_poison,
      "first real fault-buffer use exported a poisoned predecessor bit as a current fault");
  Require(name,"first-use preserves real new fault",present_current,
      "initialization/parser dropped the legitimate producer bit; clearing at first Process is incorrect");
}
} // namespace
} // namespace Libs::Graphics
