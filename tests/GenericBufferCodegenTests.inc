// Independent tiny compute numerical fixtures and a public compiler size budget.
// AMD formatted-buffer conversion/DST_SEL contract:
// https://docs.amd.com/v/u/en-US/rdna2-shader-instruction-set-architecture
// Native runtime SSBO range contract:
// https://docs.vulkan.org/guide/latest/buffer_array_length.html
#include <set>

namespace Libs::Graphics {
namespace {
constexpr uint32_t GenericInputBytes = 64u, GenericPrefixBytes = 16u;
constexpr uint32_t GenericWordsPerAdditionalReadBudget = 1536u;
// Four output channels *two normalizations, plus one duplicate body allowance.
// No guest FP arithmetic appears in this shader. Any equivalent conversion
// lowering is accepted; this is an upper cost budget, not an opcode recipe.
constexpr uint32_t GenericNormalizationDividesBudget = 16u;
constexpr std::array<uint32_t, 4> GenericInactive{0xacc00001u,0xacc00002u,0xacc00003u,0xacc00004u};

void ConfigureGenericBufferTests() {
  Require("GenericBuffer", "fixture environment setup",
      _putenv_s("KYTY_BUFFER_SPECIALIZE", "0") == 0 &&
      _putenv_s("KYTY_HALF_UNIFORM", "0") == 0 &&
      _putenv_s("KYTY_VTX_EXPLODE_CHECK", "0") == 0,
      "could not pin generic compiler profile before first getter; not a cost/numerical RED");
}

std::array<uint32_t,4> GenericInputDescriptor(Prospero::BufferFormat format, uint32_t stride,
                                           uint32_t swizzle, uint32_t range) {
  auto result = FetchGpuDescriptor(FetchGpuGuest::Input, stride, range / stride);
  result[3] = (result[3] & ~0x7ffffu) | swizzle | (static_cast<uint32_t>(format) << 12u);
  return result;
}

std::vector<uint32_t> GenericGuestCode(uint32_t loads) {
  const auto mov = [](uint32_t dst,uint32_t src) {
    return (0x3fu << 25u) | (dst << 17u) | (1u << 9u) | src;
  };
  std::vector<uint32_t> code{mov(20,10u),mov(21,11u)};
  Require("GenericBufferCompiler","bounded unrolled fixture",loads==1u || loads==8u || loads==32u,
      "load count must be1,8,32; not a cost/numerical RED");
  const uint32_t lane_shift = 4u + std::countr_zero(loads);
  code.push_back((0x1au << 25u) | (30u << 17u) | (0u << 9u) | (128u + lane_shift));
  for (uint32_t read = 0; read < loads; ++read) {
    for (uint32_t c = 0; c < 4u; ++c) {
      code.push_back(mov(10u+c,255u)); code.push_back(GenericInactive[c]);
    }
    code.push_back(0xbefe0408u); // S_MOV_B64 EXEC,s[8:9], real runtime mask
    code.push_back((0x38u << 26u) | (3u << 18u) | (1u << 13u) | (1u << 12u) | (read*16u));
    code.push_back((128u << 24u) | (10u << 8u) | 20u); // FORMAT_XYZW input s0:3
    code.push_back(0xbefe04c1u); // restore full guest EXEC for observable stores
    code.push_back(mov(31u,255u)); code.push_back(read*16u);
    code.push_back((0x25u << 25u) | (40u << 17u) | (30u << 9u) | (256u+31u));
    code.push_back((0x38u << 26u) | (0x1eu << 18u) | (1u << 12u));
    code.push_back((128u << 24u) | (1u << 16u) | (10u << 8u) | 40u);
  }
  code.push_back(0xbf810000u);
  return code;
}

struct GenericCompilation {
  ShaderComputeInputInfo compute{};
  std::array<uint32_t,12> userdata{};
  std::vector<uint32_t> code;
  FetchGpuIR::ResourcePlan plan;
  FetchGpuIR::ResourceSnapshot resources;
  FetchGpuIR::ResourceSpecialization specialization;
  ShaderRecompiler::CompileResult compiled;
  uint32_t input_resource = UINT32_MAX, loads = 0, wave = 0;
  GenericCompilation(uint32_t reads,uint32_t wave_size):code(GenericGuestCode(reads)),loads(reads),wave(wave_size) {
    constexpr const char* name = "GenericBufferCompiler";
    const auto input = GenericInputDescriptor(Prospero::BufferFormat::k32_32_32_32UInt,16u,
        DstSel(4,5,6,7),GenericInputBytes);
    const auto output = FetchGpuDescriptor(FetchGpuGuest::Output,0u,reads*wave_size*16u);
    std::copy(input.begin(),input.end(),userdata.begin());
    std::copy(output.begin(),output.end(),userdata.begin()+4u);
    userdata[8] = userdata[9] = UINT32_MAX;
    compute.threads_num[0] = wave_size;
    compute.threads_num[1] = compute.threads_num[2] = 1u;
    compute.dispatch_threads_num[0] = wave_size;
    compute.dispatch_threads_num[1] = compute.dispatch_threads_num[2] = 1u;
    compute.host_subgroup_size = 32u;
    compute.wave_size = wave_size;
    compute.thread_ids_num = 1;
    ShaderRecompiler::CompileOptions options;
    options.stage = ShaderType::Compute;
    options.wave_size = wave_size;
    options.user_data_base = 0u;
    options.user_data = userdata;
    options.input_info.compute = &compute;
    options.dump_ir = false;
    auto translated = ShaderRecompiler::TranslateProgram(code,options);
    Require(name,"actual translation setup",!translated.skip_dispatch,
        "synthetic ISA did not translate; not a cost/numerical RED");
    plan = FetchGpuIR::ExtractResourcePlan(translated.program);
    const FetchGpuIR::SrtRuntime runtime{.user_data=userdata,
        .shader_base=reinterpret_cast<uint64_t>(code.data()),
        .read_memory=ReadFetchGpuGuest,.read_specialization_memory=ReadFetchGpuGuest};
    Require(name,"actual materialization setup",FetchGpuIR::MaterializeResources(plan,runtime,resources,specialization),
        "real SRDs failed materialization; not a cost/numerical RED");
    compiled = ShaderRecompiler::CompileProgram(std::move(translated),options,specialization);
    spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_3);
    validator.SetMessageConsumer([](spv_message_level_t,const char*,const spv_position_t& pos,const char* message){
      std::fprintf(stderr,"GenericBuffer SPIR-V word%zu: %s\n",pos.index,message);
    });
    Require(name,"SPIR-V validation setup",validator.Validate(compiled.spirv),
        "module invalid before GPU; not a cost/numerical RED");
    uint32_t generic=0u;
    for(uint32_t i=0;i<compiled.program.info.buffers.size();++i) {
      if(compiled.program.info.buffers[i].descriptor_format!=FetchGpuIR::GenericBufferFormat)continue;
      input_resource=i; ++generic;
    }
    Require(name,"actual generic profile setup",generic==1u && !compiled.program.bindings.table,
        "must compile the real generic read-only formatted buffer, not a baked/BDA substitute");
    std::set<uint32_t> pcs;
    uint32_t stored=0u;
    for(const auto* block:compiled.program.blocks)for(const auto& inst:*block) {
      const auto access=FetchGpuIR::BufferAccessOf(inst.GetOpcode());
      if(access==FetchGpuIR::BufferAccess::None)continue;
      const auto flags=inst.Flags<FetchGpuIR::MemoryFlags>();
      const auto& memory=compiled.program.memory_info.at(flags.index);
      if(access==FetchGpuIR::BufferAccess::Read && memory.resource==input_resource)pcs.insert(flags.pc);
      if(access==FetchGpuIR::BufferAccess::Write)stored+=FetchGpuIR::BufferComponentCount(inst.GetOpcode());
    }
    Require(name,"all distinct reads/results live",pcs.size()==reads && stored==reads*4u,
        "cost fixture dropped or merged observable guest work; not a cost RED");
  }
};

struct GenericSpvCost {uint32_t instructions=0u,blocks=0u,selects=0u,divides=0u,functions=0u;};
GenericSpvCost GenericCensus(const std::vector<uint32_t>& words) {
  GenericSpvCost cost;
  for(size_t p=5u;p<words.size();) {
    const uint32_t count=words[p]>>16u,op=words[p]&0xffffu;
    Require("GenericBufferCost","valid module framing",count!=0u && count<=words.size()-p,
        "SPIR-V framing invalid; not a cost RED");
    ++cost.instructions;
    cost.blocks+=op==static_cast<uint32_t>(spv::OpLabel);
    cost.selects+=op==static_cast<uint32_t>(spv::OpSelect);
    cost.divides+=op==static_cast<uint32_t>(spv::OpFDiv);
    cost.functions+=op==static_cast<uint32_t>(spv::OpFunction);
    p+=count;
  }
  return cost;
}
void GenericCompilerProfile() {
  ShaderRecompiler::SetTableMode(false);
  ShaderRecompiler::SetHardwareStorageBufferBounds(false);
  ShaderRecompiler::SetNativeFloat16(false);
}
void CheckGenericBufferCodegenCost(VulkanHarness&) {
  GenericCompilerProfile();
  FetchGpuGuest guest;
  std::array<size_t,3> sizes{};
  std::array<GenericSpvCost,3> costs{};
  constexpr std::array<uint32_t,3> counts{1u,8u,32u};
  for(uint32_t k=0;k<counts.size();++k) {
    GenericCompilation shader(counts[k],32u);
    sizes[k]=shader.compiled.spirv.size();
    const auto cost=GenericCensus(shader.compiled.spirv);
    costs[k]=cost;
    std::printf("[cost] actual generic XYZW loads=%u words=%zu instructions=%u blocks=%u selects=%u FDiv=%u functions=%u all outputs live\n",
        counts[k],sizes[k],cost.instructions,cost.blocks,cost.selects,cost.divides,cost.functions);
  }
  for(uint32_t k=0;k<counts.size();++k)
    Require("GenericBufferCost","shared normalization body cost budget",
        costs[k].divides<=GenericNormalizationDividesBudget,
        "one generic slot repeats more than16 static normalization divisions; share conversion code while preserving all guest work");
  for(uint32_t k=1;k<counts.size();++k)
    Require("GenericBufferCost","shared conversion expansion budget",
        sizes[k]<=sizes[k-1u]+size_t{counts[k]-counts[k-1u]}*GenericWordsPerAdditionalReadBudget,
        "additional live XYZW reads exceed1536 SPIR-V words each; preserve reads/numerics while sharing invariant conversion body");
}

struct GenericNumericCase {
  const char* name;
  Prospero::BufferFormat format;
  std::array<uint32_t,4> source,expected;
  uint32_t selectors=DstSel(4,5,6,7),stride=4u,index=0u,offset=0u,range=GenericInputBytes,source_offset=0u;
};

std::vector<uint32_t> GenericPackShaderData(GenericCompilation& shader) {
  const auto& compiled=shader.compiled;
  const auto& layout=compiled.program.bindings;
  std::vector<uint32_t> data;
  for(const auto reg:layout.user_data_registers)
    data.push_back(shader.resources.user_data.at(reg-compiled.program.user_data_base));
  data.resize(layout.ShaderDataDwords());
  // The bound native buffer includes a real16-byte prefix before guest input.
  // Pack the shared per-resource offset, not a rewritten guest address.
  data[layout.memory_offset_dword+shader.input_resource/4u] |=
      GenericPrefixBytes<<((shader.input_resource%4u)*8u);
  for(uint32_t k=0;k<layout.buffer_strides.size();++k)
    data[layout.BufferStrideDword()+k/2u] |=
        (FetchGpuIR::CanonicalPackedStride(shader.resources.buffers.at(layout.buffer_strides[k]))&0x3fffu)
        <<((k%2u)*16u);
  for(uint32_t k=0;k<layout.buffer_formats.size();++k)
    data[layout.BufferFormatDword()+k] =
        FetchGpuIR::RuntimeBufferSlotWord(shader.resources.buffers.at(layout.buffer_formats[k]));
  return data;
}

class GenericGpuPipeline {
  VulkanHarness& vulkan;
  GenericCompilation& shader;
  FetchGpuBuffer& shader_input;
  FetchGpuBuffer& output;
  std::unique_ptr<FetchGpuBuffer> shader_data_buffer,flat_buffer;
  vk::DescriptorSetLayout descriptor_layout=nullptr;
  vk::DescriptorPool pool=nullptr;
  vk::DescriptorSet set=nullptr;
  vk::PipelineLayout pipeline_layout=nullptr;
  vk::ShaderModule module=nullptr;
  vk::Pipeline pipeline=nullptr;
  bool measuring=false;
  uint32_t measured_mismatches=0u;
public:
  GenericGpuPipeline(VulkanHarness& native,GenericCompilation& compiled_shader,
                     FetchGpuBuffer& input_buffer,FetchGpuBuffer& output_buffer):
      vulkan(native),shader(compiled_shader),shader_input(input_buffer),output(output_buffer) {
    constexpr const char* name="GenericBufferGpu";
    auto& graphics=vulkan.RuntimeContext();
    const auto device=vulkan.Device();
    const auto& compiled=shader.compiled;
    const auto& layout=compiled.program.bindings;
    const auto& resources=shader.resources;
    const auto& plan=shader.plan;
    auto shader_data=GenericPackShaderData(shader);

  std::vector<vk::DescriptorSetLayoutBinding> bindings;
  std::vector<vk::DescriptorPoolSize> pool_sizes;
  using Kind = FetchGpuIR::DescriptorBindingKind;
  for (const auto& binding : layout.descriptors) {
    Require(name, "supported actual bindings", binding.kind == Kind::Buffers ||
                binding.kind == Kind::ShaderData || binding.kind == Kind::FlattenedSrt,
            "unexpected actual binding needs explicit native fixture support; not a semantic RED");
    const auto count = NativeDescriptorCount(binding);
    const auto type = NativeDescriptorType(binding.kind);
    bindings.push_back({FetchGpuIR::NativeBinding(ShaderType::Compute, binding.kind), type,
                        count, vk::ShaderStageFlagBits::eCompute, nullptr});
    auto found = std::find_if(pool_sizes.begin(), pool_sizes.end(),
                              [&](const auto& entry) { return entry.type == type; });
    if (found == pool_sizes.end()) pool_sizes.push_back({type, count});
    else found->descriptorCount += count;
    if (binding.kind == Kind::ShaderData) {
      Require(name, "actual shader data", !shader_data.empty(), "required shader data is missing");
      shader_data_buffer = std::make_unique<FetchGpuBuffer>(graphics, shader_data.size() * 4u,
                                                           vk::BufferUsageFlagBits::eStorageBuffer);
      std::memcpy(shader_data_buffer->data, shader_data.data(), shader_data_buffer->size);
      shader_data_buffer->Flush();
    }
    if (binding.kind == Kind::FlattenedSrt) {
      Require(name, "actual flattened SRT", !resources.flattened_srt.empty(), "required SRT data is missing");
      flat_buffer = std::make_unique<FetchGpuBuffer>(graphics, resources.flattened_srt.size() * 4u,
                                                    vk::BufferUsageFlagBits::eStorageBuffer);
      std::memcpy(flat_buffer->data, resources.flattened_srt.data(), flat_buffer->size);
      flat_buffer->Flush();
    }
  }
  vk::DescriptorSetLayoutCreateInfo descriptor_info{};
  descriptor_info.bindingCount = static_cast<uint32_t>(bindings.size());
  descriptor_info.pBindings = bindings.data();
  RequireVulkanSuccess(device.createDescriptorSetLayout(&descriptor_info, nullptr, &descriptor_layout),
                       "generic-buffer reflected descriptor layout");
  vk::DescriptorPoolCreateInfo pool_info{};
  pool_info.maxSets = 1u;
  pool_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
  pool_info.pPoolSizes = pool_sizes.data();
  RequireVulkanSuccess(device.createDescriptorPool(&pool_info, nullptr, &pool),
                       "generic-buffer descriptor pool");
  vk::DescriptorSetAllocateInfo set_info{};
  set_info.descriptorPool = pool;
  set_info.descriptorSetCount = 1u;
  set_info.pSetLayouts = &descriptor_layout;
  RequireVulkanSuccess(device.allocateDescriptorSets(&set_info, &set), "generic-buffer descriptor set");
  for (const auto& binding : layout.descriptors) {
    std::vector<vk::DescriptorBufferInfo> infos;
    if (binding.kind == Kind::Buffers) {
      for (const auto resource : binding.resources) {
        // MaterializeResources stores each ordinary buffer at the same info
        // resource index; this fixture explicitly excludes indirect children.
        Require(name, "actual ordinary-buffer layout", resource < plan.info.buffers.size() &&
                    resource < resources.buffers.size() &&
                    compiled.program.info.buffers.at(resource).source == plan.info.buffers[resource].source &&
                    compiled.program.info.buffers.at(resource).indirect_root ==
                        FetchGpuIR::BufferResource::NoIndirectBuffer,
                "reflected ordinary buffer does not match its materialized plan index");
        const auto& descriptor = resources.buffers.at(resource);
        const uint64_t address = descriptor.dwords[0] |
            (static_cast<uint64_t>(descriptor.dwords[1] & 0xffffu) << 32u);
        if (address == FetchGpuGuest::Input) infos.push_back({shader_input.buffer, 0u, GenericPrefixBytes + GenericInputBytes});
        else {
          Require(name, "actual resource identity", address == FetchGpuGuest::Output,
                  "unexpected buffer SRD; must not fabricate a fallback resource");
          infos.push_back({output.buffer, 0u, output.size});
        }
      }
    } else {
      const auto* buffer = binding.kind == Kind::ShaderData ? shader_data_buffer.get() : flat_buffer.get();
      Require(name, "actual reflected data buffer", buffer != nullptr, "required data buffer missing");
      infos.push_back({buffer->buffer, 0u, buffer->size});
    }
    Require(name, "native descriptor count", infos.size() == NativeDescriptorCount(binding),
            "actual reflected binding count differs from fixture resources");
    vk::WriteDescriptorSet write{};
    write.dstSet = set;
    write.dstBinding = FetchGpuIR::NativeBinding(ShaderType::Compute, binding.kind);
    write.descriptorCount = static_cast<uint32_t>(infos.size());
    write.descriptorType = NativeDescriptorType(binding.kind);
    write.pBufferInfo = infos.data();
    device.updateDescriptorSets(1u, &write, 0u, nullptr);
  }
  vk::PushConstantRange push_range{vk::ShaderStageFlagBits::eCompute, 0u, FetchGpuIR::NativePushConstantSize};
  vk::PipelineLayoutCreateInfo pipeline_layout_info{};
  pipeline_layout_info.setLayoutCount = 1u;
  pipeline_layout_info.pSetLayouts = &descriptor_layout;
  pipeline_layout_info.pushConstantRangeCount = layout.UsesPushData() ? 1u : 0u;
  pipeline_layout_info.pPushConstantRanges = layout.UsesPushData() ? &push_range : nullptr;
  RequireVulkanSuccess(device.createPipelineLayout(&pipeline_layout_info, nullptr, &pipeline_layout),
                       "generic-buffer pipeline layout");
  vk::ShaderModuleCreateInfo module_info{};
  module_info.codeSize = compiled.spirv.size() * sizeof(uint32_t);
  module_info.pCode = compiled.spirv.data();
  RequireVulkanSuccess(device.createShaderModule(&module_info, nullptr, &module), "actual compute module");
  vk::PipelineShaderStageCreateInfo stage{};
  stage.stage = vk::ShaderStageFlagBits::eCompute;
  stage.module = module;
  stage.pName = "main";
  vk::ComputePipelineCreateInfo pipeline_info{};
  pipeline_info.stage = stage;
  pipeline_info.layout = pipeline_layout;
  RequireVulkanSuccess(device.createComputePipelines(nullptr,1u,&pipeline_info,nullptr,&pipeline),
      "real bounded generic-buffer compute pipeline");
  }
  ~GenericGpuPipeline() {
    const auto device=vulkan.Device();
    device.destroyPipeline(pipeline,nullptr);
    device.destroyShaderModule(module,nullptr);
    device.destroyPipelineLayout(pipeline_layout,nullptr);
    device.destroyDescriptorPool(pool,nullptr);
    device.destroyDescriptorSetLayout(descriptor_layout,nullptr);
  }
  void Run(const GenericNumericCase& test,uint32_t low_exec,uint32_t high_exec) {
    constexpr const char* name="GenericBufferGpu";
    const auto descriptor=GenericInputDescriptor(test.format,test.stride,test.selectors,test.range);
    std::copy(descriptor.begin(),descriptor.end(),shader.userdata.begin());
    shader.userdata[8]=low_exec; shader.userdata[9]=high_exec;
    shader.userdata[10]=test.index; shader.userdata[11]=test.offset;
    const FetchGpuIR::SrtRuntime runtime{.user_data=shader.userdata,
        .shader_base=reinterpret_cast<uint64_t>(shader.code.data()),
        .read_memory=ReadFetchGpuGuest,.read_specialization_memory=ReadFetchGpuGuest};
    Require(name,"real per-case materialization setup",
        FetchGpuIR::MaterializeResources(shader.plan,runtime,shader.resources,shader.specialization),
        "actual changed descriptor did not materialize; not a numerical RED");
    auto normalized=shader.specialization;
    FetchGpuIR::NormalizeBufferSpecialization(normalized,ShaderType::Compute,shader.plan.info,
        shader.plan.shader_data_base_dwords);
    Require(name,"same generic permutation setup",
        normalized.buffers.at(shader.input_resource).descriptor_format==FetchGpuIR::GenericBufferFormat &&
        normalized.buffers.at(shader.input_resource).packed_stride==
            shader.compiled.program.info.buffers.at(shader.input_resource).packed_stride,
        "case requires a different compiled permutation; not a numerical RED");
    std::memset(shader_input.data,0x5a,GenericPrefixBytes);
    std::memset(static_cast<uint8_t*>(shader_input.data)+GenericPrefixBytes,0,GenericInputBytes);
    Require(name,"bounded independent source",test.source_offset<=GenericInputBytes-sizeof(test.source),
        "fixture source exceeds its real tiny guest range");
    std::memcpy(static_cast<uint8_t*>(shader_input.data)+GenericPrefixBytes+test.source_offset,
        test.source.data(),sizeof(test.source));
    std::memset(reinterpret_cast<void*>(FetchGpuGuest::Input),0,GenericInputBytes);
    std::memcpy(reinterpret_cast<void*>(FetchGpuGuest::Input+test.source_offset),
        test.source.data(),sizeof(test.source));
    shader_input.Flush();
    std::fill_n(static_cast<uint32_t*>(output.data),output.size/4u,0xdeadbeefu);
    output.Flush();
    auto data=GenericPackShaderData(shader);
    if(shader_data_buffer) {
      std::memcpy(shader_data_buffer->data,data.data(),shader_data_buffer->size);
      shader_data_buffer->Flush();
    }
    const auto& layout=shader.compiled.program.bindings;
    // Change only the real input descriptor's bound range between completed
    // dispatches. ArrayLength and all per-word/whole-element checks remain real.
    for(const auto& binding:layout.descriptors) {
      if(binding.kind!=FetchGpuIR::DescriptorBindingKind::Buffers)continue;
      std::vector<vk::DescriptorBufferInfo> infos;
      for(const auto resource:binding.resources)
        infos.push_back(resource==shader.input_resource ?
            vk::DescriptorBufferInfo{shader_input.buffer,0u,GenericPrefixBytes+test.range} :
            vk::DescriptorBufferInfo{output.buffer,0u,shader.wave*16u});
      vk::WriteDescriptorSet write{};
      write.dstSet=set;
      write.dstBinding=FetchGpuIR::NativeBinding(ShaderType::Compute,binding.kind);
      write.descriptorCount=static_cast<uint32_t>(infos.size());
      write.descriptorType=NativeDescriptorType(binding.kind);
      write.pBufferInfo=infos.data();
      vulkan.Device().updateDescriptorSets(1u,&write,0u,nullptr);
    }
    auto& scheduler=vulkan.RuntimeRenderer().GetCommandScheduler();
    auto command=scheduler.Current().Handle();
    vk::MemoryBarrier ready{};
    ready.srcAccessMask=vk::AccessFlagBits::eHostWrite;
    ready.dstAccessMask=vk::AccessFlagBits::eShaderRead|vk::AccessFlagBits::eShaderWrite;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eHost,vk::PipelineStageFlagBits::eComputeShader,
        {},1u,&ready,0u,nullptr,0u,nullptr);
    command.bindPipeline(vk::PipelineBindPoint::eCompute,pipeline);
    command.bindDescriptorSets(vk::PipelineBindPoint::eCompute,pipeline_layout,0u,1u,&set,0u,nullptr);
    if(layout.UsesPushData()) {
      FetchGpuIR::PushData push;
      std::copy(data.begin(),data.end(),push.dwords.begin()+layout.push_data_start_dword);
      command.pushConstants(pipeline_layout,vk::ShaderStageFlagBits::eCompute,0u,sizeof(push),push.dwords.data());
    }
    command.dispatch(1u,1u,1u);
    vk::MemoryBarrier done{};
    done.srcAccessMask=vk::AccessFlagBits::eShaderWrite;
    done.dstAccessMask=vk::AccessFlagBits::eHostRead;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,vk::PipelineStageFlagBits::eHost,
        {},1u,&done,0u,nullptr,0u,nullptr);
    scheduler.Finish();
    output.Invalidate();
    const auto* words=static_cast<const uint32_t*>(output.data);
    for(uint32_t lane=0;lane<shader.wave;++lane) {
      const auto mask=lane<32u?low_exec:high_exec;
      const auto& expected=((mask>>(lane%32u))&1u)!=0u?test.expected:GenericInactive;
      if(!std::equal(expected.begin(),expected.end(),words+lane*4u)) {
        const auto* actual=words+lane*4u;
        const auto& real_descriptor=shader.resources.buffers.at(shader.input_resource).dwords;
        std::printf("[gpu-detail] %s wave=%u lane=%u EXEC=%08x/%08x active=%u actual=%08x,%08x,%08x,%08x expected=%08x,%08x,%08x,%08x\n",
            test.name,shader.wave,lane,low_exec,high_exec,(mask>>(lane%32u))&1u,
            actual[0],actual[1],actual[2],actual[3],expected[0],expected[1],expected[2],expected[3]);
        std::printf("[gpu-detail] materializedInputSRD=%08x,%08x,%08x,%08x format=%u stride=%u index=%u offset=%08x prefix=%u boundRange=%u sourceAt=%u source=%08x,%08x,%08x,%08x\n",
            real_descriptor[0],real_descriptor[1],real_descriptor[2],real_descriptor[3],
            static_cast<uint32_t>(test.format),test.stride,test.index,test.offset,GenericPrefixBytes,
            GenericPrefixBytes+test.range,test.source_offset,test.source[0],test.source[1],test.source[2],test.source[3]);
        std::printf("[gpu-detail] actualShaderData(%zu):",data.size());
        for(const auto word:data)std::printf(" %08x",word);
        std::printf("; inputResource=%u formatDword=%u offsetsDword=%u\n",
            shader.input_resource,layout.BufferFormatDword(),layout.memory_offset_dword);
      }
      if(measuring) {
        const auto* actual=words+lane*4u;
        const bool same=std::equal(expected.begin(),expected.end(),actual);
        measured_mismatches+=!same;
        std::printf("[gpu-measurement] case=%s wave=%u lane=%u EXEC=%08x/%08x active=%u actual=%08x,%08x,%08x,%08x expected=%08x,%08x,%08x,%08x equal=%u\n",
            test.name,shader.wave,lane,low_exec,high_exec,(mask>>(lane%32u))&1u,
            actual[0],actual[1],actual[2],actual[3],expected[0],expected[1],expected[2],expected[3],same);
      } else {
      Require(test.name,"real generic format/bounds/half-EXEC result",
          std::equal(expected.begin(),expected.end(),words+lane*4u),
          "actual output differs from independent byte/format/bounds golden or inactive VGPRs");
      }
    }
    for(uint32_t i=shader.wave*4u;i<output.size/4u;++i)
      Require(test.name,"output boundary sentinel",words[i]==0xdeadbeefu,"dispatch wrote beyond its live lane outputs");
    if(!measuring) {
    std::printf("[gpu] %s wave%u lowEXEC=%08x highEXEC=%08x actual=%08x,%08x,%08x,%08x PASS\n",
        test.name,shader.wave,low_exec,high_exec,words[0],words[1],words[2],words[3]);
    } else {
      std::printf("[gpu-measurement] case=%s wave=%u EXEC=%08x/%08x cumulativeLaneMismatches=%u (measurement only)\n",
          test.name,shader.wave,low_exec,high_exec,measured_mismatches);
    }
  }
  uint32_t RunMeasurement(const GenericNumericCase& test,uint32_t low_exec,uint32_t high_exec) {
    const auto previous=measured_mismatches;
    measuring=true;
    Run(test,low_exec,high_exec);
    measuring=false;
    return measured_mismatches-previous;
  }
};
void CheckGenericBufferNumeric(VulkanHarness& vulkan) {
  constexpr const char* name="GenericBufferGpu";
  auto& graphics=vulkan.RuntimeContext();
  vk::PhysicalDeviceSubgroupProperties subgroup{};
  vk::PhysicalDeviceProperties2 properties{};
  properties.pNext=&subgroup;
  graphics.physical_device.getProperties2(&properties);
  Require(name,"actual compute subgroup capability",
      subgroup.subgroupSize==32u &&
      static_cast<bool>(subgroup.supportedStages&vk::ShaderStageFlagBits::eCompute) &&
      static_cast<bool>(subgroup.supportedOperations&vk::SubgroupFeatureFlagBits::eBallot),
      "numeric fixture requires real32-lane compute ballot support; unsupported is not a pass");
  GenericCompilerProfile();
  FetchGpuGuest guest;
  using F=Prospero::BufferFormat;
  // These are literal guest bytes and mathematical conversion results, not
  // values decoded by the production Format helpers used by the implementation.
  const std::vector<GenericNumericCase> cases{
      {"UInt8-max",F::k8UInt,{0xffu,0u,0u,0u},{0xffu,0xffu,0xffu,0xffu}},
      {"SInt8-min",F::k8SInt,{0x80u,0u,0u,0u},{0xffffff80u,0xffffff80u,0xffffff80u,0xffffff80u}},
      {"Uscaled8",F::k8UScaled,{0xffu,0u,0u,0u},{0x437f0000u,0x437f0000u,0x437f0000u,0x437f0000u}},
      {"Sscaled8",F::k8SScaled,{0x80u,0u,0u,0u},{0xc3000000u,0xc3000000u,0xc3000000u,0xc3000000u}},
      {"UNorm8-one",F::k8UNorm,{0xffu,0u,0u,0u},{0x3f800000u,0x3f800000u,0x3f800000u,0x3f800000u}},
      {"SNorm8-min-clamp",F::k8SNorm,{0x80u,0u,0u,0u},{0xbf800000u,0xbf800000u,0xbf800000u,0xbf800000u}},
      {"UInt16-max",F::k16UInt,{0xffffu,0u,0u,0u},{0xffffu,0xffffu,0xffffu,0xffffu}},
      {"SInt16-min",F::k16SInt,{0x8000u,0u,0u,0u},{0xffff8000u,0xffff8000u,0xffff8000u,0xffff8000u}},
      {"Uscaled16",F::k16UScaled,{0x101u,0u,0u,0u},{0x43808000u,0x43808000u,0x43808000u,0x43808000u}},
      {"Sscaled16",F::k16SScaled,{0xfeffu,0u,0u,0u},{0xc3808000u,0xc3808000u,0xc3808000u,0xc3808000u}},
      {"UNorm16-one",F::k16UNorm,{0xffffu,0u,0u,0u},{0x3f800000u,0x3f800000u,0x3f800000u,0x3f800000u}},
      {"SNorm16-min-clamp",F::k16SNorm,{0x8000u,0u,0u,0u},{0xbf800000u,0xbf800000u,0xbf800000u,0xbf800000u}},
      {"FP16-one",F::k16Float,{0x3c00u,0u,0u,0u},{0x3f800000u,0x3f800000u,0x3f800000u,0x3f800000u}},
      {"UInt32-raw",F::k32UInt,{0xdeadbeefu,0u,0u,0u},{0xdeadbeefu,0xdeadbeefu,0xdeadbeefu,0xdeadbeefu}},
      {"SInt32-min",F::k32SInt,{0x80000000u,0u,0u,0u},{0x80000000u,0x80000000u,0x80000000u,0x80000000u}},
      {"FP32-negative-zero",F::k32Float,{0x80000000u,0u,0u,0u},{0x80000000u,0x80000000u,0x80000000u,0x80000000u}},
      {"UInt8-WZY-one",F::k8_8_8_8UInt,{0x11223344u,0u,0u,0u},{0x11u,0x22u,0x33u,1u},DstSel(7,6,5,1)},
      {"UInt16-four-components",F::k16_16_16_16UInt,{0xffff0001u,0x80007fffu,0u,0u},{1u,0xffffu,0x7fffu,0x8000u}},
      {"FP32-four-components",F::k32_32_32_32Float,{0x3f800000u,0xbf000000u,0u,0x80000000u},
          {0x3f800000u,0xbf000000u,0u,0x80000000u}},
      {"float-constants-repeat",F::k32Float,{0xbf000000u,0u,0u,0u},{0u,0x3f800000u,0xbf000000u,0xbf000000u},DstSel(0,1,4,4)},
      {"runtime-stride12-plus-vector-offset",F::k32_32_32_32UInt,
          {0xabcdef01u,0x10203040u,0x50607080u,0x90a0b0c0u},{0xabcdef01u,0x10203040u,0x50607080u,0x90a0b0c0u},
          DstSel(4,5,6,7),12u,1u,4u,60u,16u},
      {"unaligned-element-aligns-DWORD",F::k8_8_8_8UInt,{0x11223344u,0u,0u,0u},{0x44u,0x33u,0x22u,0x11u},
          DstSel(4,5,6,7),4u,0u,3u},
      {"whole-element-exact-end",F::k32_32_32_32UInt,
          {1u,2u,3u,4u},{1u,2u,3u,4u},DstSel(4,5,6,7),4u,0u,48u,64u,48u},
      {"whole-element-partial-OOB-keeps-constants",F::k32_32_32_32Float,
          {0x3f800000u,0x40000000u,0x40400000u,0x40800000u},{0u,0u,0x3f800000u,0u},
          DstSel(4,0,1,7),4u,0u,52u},
      {"prefix-overflow-cannot-alias-zero",F::k32_32_32_32UInt,
          {1u,2u,3u,4u},{0u,0u,0u,0u},DstSel(4,5,6,7),4u,0u,0xfffffff0u}
  };
  FetchGpuBuffer input(graphics,GenericPrefixBytes+GenericInputBytes,vk::BufferUsageFlagBits::eStorageBuffer);
  HW::Context registers{};
  HW::UserConfig user_config{};
  HW::Shader shaders{};
  vulkan.RuntimeRenderer().GetCommandScheduler().Begin(registers,user_config,shaders);
  for(const uint32_t wave:{32u,64u}) {
    GenericCompilation shader(1u,wave);
    FetchGpuBuffer output(graphics,wave*16u+16u,vk::BufferUsageFlagBits::eStorageBuffer);
    GenericGpuPipeline pipeline(vulkan,shader,input,output);
    for(const auto& test:cases) {
      pipeline.Run(test,UINT32_MAX,UINT32_MAX);
      // Odd low-half and even high-half lanes are active: both emulated halves
      // retain their distinct original VGPRs when inactive, then publish stores.
      pipeline.Run(test,0xaaaaaaaau,0x55555555u);
    }
  }
}
// This collector does not certify the strict numerical suite. It retains all
// original goldens, reports every lane, and returns a nonzero mismatch count.
uint32_t MeasureGenericBufferNumeric(VulkanHarness& vulkan) {
  uint32_t mismatches=0u;
  constexpr const char* name="GenericBufferGpu";
  auto& graphics=vulkan.RuntimeContext();
  vk::PhysicalDeviceSubgroupProperties subgroup{};
  vk::PhysicalDeviceProperties2 properties{};
  properties.pNext=&subgroup;
  graphics.physical_device.getProperties2(&properties);
  Require(name,"actual compute subgroup capability",
      subgroup.subgroupSize==32u &&
      static_cast<bool>(subgroup.supportedStages&vk::ShaderStageFlagBits::eCompute) &&
      static_cast<bool>(subgroup.supportedOperations&vk::SubgroupFeatureFlagBits::eBallot),
      "numeric fixture requires real32-lane compute ballot support; unsupported is not a pass");
  GenericCompilerProfile();
  FetchGpuGuest guest;
  using F=Prospero::BufferFormat;
  // These are literal guest bytes and mathematical conversion results, not
  // values decoded by the production Format helpers used by the implementation.
  const std::vector<GenericNumericCase> cases{
      {"UInt8-max",F::k8UInt,{0xffu,0u,0u,0u},{0xffu,0xffu,0xffu,0xffu}},
      {"SInt8-min",F::k8SInt,{0x80u,0u,0u,0u},{0xffffff80u,0xffffff80u,0xffffff80u,0xffffff80u}},
      {"Uscaled8",F::k8UScaled,{0xffu,0u,0u,0u},{0x437f0000u,0x437f0000u,0x437f0000u,0x437f0000u}},
      {"Sscaled8",F::k8SScaled,{0x80u,0u,0u,0u},{0xc3000000u,0xc3000000u,0xc3000000u,0xc3000000u}},
      {"UNorm8-one",F::k8UNorm,{0xffu,0u,0u,0u},{0x3f800000u,0x3f800000u,0x3f800000u,0x3f800000u}},
      {"SNorm8-min-clamp",F::k8SNorm,{0x80u,0u,0u,0u},{0xbf800000u,0xbf800000u,0xbf800000u,0xbf800000u}},
      {"UInt16-max",F::k16UInt,{0xffffu,0u,0u,0u},{0xffffu,0xffffu,0xffffu,0xffffu}},
      {"SInt16-min",F::k16SInt,{0x8000u,0u,0u,0u},{0xffff8000u,0xffff8000u,0xffff8000u,0xffff8000u}},
      {"Uscaled16",F::k16UScaled,{0x101u,0u,0u,0u},{0x43808000u,0x43808000u,0x43808000u,0x43808000u}},
      {"Sscaled16",F::k16SScaled,{0xfeffu,0u,0u,0u},{0xc3808000u,0xc3808000u,0xc3808000u,0xc3808000u}},
      {"UNorm16-one",F::k16UNorm,{0xffffu,0u,0u,0u},{0x3f800000u,0x3f800000u,0x3f800000u,0x3f800000u}},
      {"SNorm16-min-clamp",F::k16SNorm,{0x8000u,0u,0u,0u},{0xbf800000u,0xbf800000u,0xbf800000u,0xbf800000u}},
      {"FP16-one",F::k16Float,{0x3c00u,0u,0u,0u},{0x3f800000u,0x3f800000u,0x3f800000u,0x3f800000u}},
      {"UInt32-raw",F::k32UInt,{0xdeadbeefu,0u,0u,0u},{0xdeadbeefu,0xdeadbeefu,0xdeadbeefu,0xdeadbeefu}},
      {"SInt32-min",F::k32SInt,{0x80000000u,0u,0u,0u},{0x80000000u,0x80000000u,0x80000000u,0x80000000u}},
      {"FP32-negative-zero",F::k32Float,{0x80000000u,0u,0u,0u},{0x80000000u,0x80000000u,0x80000000u,0x80000000u}},
      {"UInt8-WZY-one",F::k8_8_8_8UInt,{0x11223344u,0u,0u,0u},{0x11u,0x22u,0x33u,1u},DstSel(7,6,5,1)},
      {"UInt16-four-components",F::k16_16_16_16UInt,{0xffff0001u,0x80007fffu,0u,0u},{1u,0xffffu,0x7fffu,0x8000u}},
      {"FP32-four-components",F::k32_32_32_32Float,{0x3f800000u,0xbf000000u,0u,0x80000000u},
          {0x3f800000u,0xbf000000u,0u,0x80000000u}},
      {"float-constants-repeat",F::k32Float,{0xbf000000u,0u,0u,0u},{0u,0x3f800000u,0xbf000000u,0xbf000000u},DstSel(0,1,4,4)},
      {"runtime-stride12-plus-vector-offset",F::k32_32_32_32UInt,
          {0xabcdef01u,0x10203040u,0x50607080u,0x90a0b0c0u},{0xabcdef01u,0x10203040u,0x50607080u,0x90a0b0c0u},
          DstSel(4,5,6,7),12u,1u,4u,60u,16u},
      {"unaligned-element-aligns-DWORD",F::k8_8_8_8UInt,{0x11223344u,0u,0u,0u},{0x44u,0x33u,0x22u,0x11u},
          DstSel(4,5,6,7),4u,0u,3u},
      {"whole-element-exact-end",F::k32_32_32_32UInt,
          {1u,2u,3u,4u},{1u,2u,3u,4u},DstSel(4,5,6,7),4u,0u,48u,64u,48u},
      {"whole-element-partial-OOB-keeps-constants",F::k32_32_32_32Float,
          {0x3f800000u,0x40000000u,0x40400000u,0x40800000u},{0u,0u,0x3f800000u,0u},
          DstSel(4,0,1,7),4u,0u,52u},
      {"prefix-overflow-cannot-alias-zero",F::k32_32_32_32UInt,
          {1u,2u,3u,4u},{0u,0u,0u,0u},DstSel(4,5,6,7),4u,0u,0xfffffff0u}
  };
  FetchGpuBuffer input(graphics,GenericPrefixBytes+GenericInputBytes,vk::BufferUsageFlagBits::eStorageBuffer);
  HW::Context registers{};
  HW::UserConfig user_config{};
  HW::Shader shaders{};
  vulkan.RuntimeRenderer().GetCommandScheduler().Begin(registers,user_config,shaders);
  for(const uint32_t wave:{32u,64u}) {
    GenericCompilation shader(1u,wave);
    FetchGpuBuffer output(graphics,wave*16u+16u,vk::BufferUsageFlagBits::eStorageBuffer);
    GenericGpuPipeline pipeline(vulkan,shader,input,output);
    for(const auto& test:cases) {
      mismatches+=pipeline.RunMeasurement(test,UINT32_MAX,UINT32_MAX);
      // Odd low-half and even high-half lanes are active: both emulated halves
      // retain their distinct original VGPRs when inactive, then publish stores.
      mismatches+=pipeline.RunMeasurement(test,0xaaaaaaaau,0x55555555u);
    }
  }
  std::printf("[gpu-measurement] totalLaneMismatches=%u; all100 case/mask/wave requests observed; guest normalized precision remains unresolved\n",mismatches);
  return mismatches;
}
} // namespace
} // namespace Libs::Graphics
