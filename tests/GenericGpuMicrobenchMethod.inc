// Include INSIDE GenericGpuPipeline public section before Run(). Existing APIs only.
void BenchmarkGenericLoads(uint32_t channels, uint32_t fixed_repeats) {
  constexpr const char* name="GenericGpuMicrobench";
  Require(name,"bounded workload setup",shader.loads==32 && (shader.wave==32 || shader.wave==64) &&
      channels>=1 && channels<=4 && fixed_repeats<=256 &&
      shader_input.size>=GenericPrefixBytes+512 && output.size>=shader.wave*512u+16,
      "requires32real reads, adequate buffers and at most256finite dispatches/batch");
  using F=Prospero::BufferFormat;
  constexpr std::array formats{F::k32UInt,F::k32_32UInt,F::k32_32_32UInt,F::k32_32_32_32UInt};
  constexpr std::array<std::array<uint32_t,4>,4> picks{{{0,0,0,0},{0,1,0,1},{0,1,2,0},{0,1,2,3}}};
  const auto descriptor=GenericInputDescriptor(formats[channels-1],16,DstSel(4,5,6,7),512);
  std::copy(descriptor.begin(),descriptor.end(),shader.userdata.begin());
  shader.userdata[8]=shader.userdata[9]=UINT32_MAX;
  shader.userdata[10]=shader.userdata[11]=0;
  const FetchGpuIR::SrtRuntime runtime{.user_data=shader.userdata,
      .shader_base=reinterpret_cast<uint64_t>(shader.code.data()),
      .read_memory=ReadFetchGpuGuest,.read_specialization_memory=ReadFetchGpuGuest};
  Require(name,"real materialization setup",
      FetchGpuIR::MaterializeResources(shader.plan,runtime,shader.resources,shader.specialization),
      "benchmark metadata did not materialize");
  const auto Word=[](uint32_t record,uint32_t component) {
    return 0x10000000u+record*0x100u+component*0x11u;
  };
  std::memset(shader_input.data,0x5a,shader_input.size);
  for(uint32_t record=0;record<32;++record)for(uint32_t component=0;component<4;++component)
    static_cast<uint32_t*>(shader_input.data)[GenericPrefixBytes/4u+record*4u+component]=Word(record,component);
  shader_input.Flush();
  std::fill_n(static_cast<uint32_t*>(output.data),output.size/4u,0xdeadbeefu);
  output.Flush();
  auto data=GenericPackShaderData(shader);
  if(shader_data_buffer) {
    std::memcpy(shader_data_buffer->data,data.data(),shader_data_buffer->size);
    shader_data_buffer->Flush();
  }
  const auto& layout=shader.compiled.program.bindings;
  for(const auto& binding:layout.descriptors) {
    if(binding.kind!=FetchGpuIR::DescriptorBindingKind::Buffers)continue;
    std::vector<vk::DescriptorBufferInfo> infos;
    for(const auto resource:binding.resources)
      infos.push_back(resource==shader.input_resource ?
          vk::DescriptorBufferInfo{shader_input.buffer,0,GenericPrefixBytes+512u} :
          vk::DescriptorBufferInfo{output.buffer,0,shader.wave*512u});
    vk::WriteDescriptorSet write{};
    write.dstSet=set;write.dstBinding=FetchGpuIR::NativeBinding(ShaderType::Compute,binding.kind);
    write.descriptorCount=static_cast<uint32_t>(infos.size());
    write.descriptorType=NativeDescriptorType(binding.kind);write.pBufferInfo=infos.data();
    vulkan.Device().updateDescriptorSets(1,&write,0,nullptr);
  }
  const auto CheckOutput=[&] {
    output.Invalidate();
    const auto* words=static_cast<const uint32_t*>(output.data);
    for(uint32_t lane=0;lane<shader.wave;++lane)for(uint32_t read=0;read<32;++read)
      for(uint32_t c=0;c<4;++c)
        Require(name,"all real read/output bytes",words[(lane*32u+read)*4u+c]==Word(read,picks[channels-1][c]),
                "benchmark outputs differ from independent channel/record golden");
    for(uint32_t i=shader.wave*128u;i<output.size/4u;++i)
      Require(name,"output tail guard",words[i]==0xdeadbeefu,"benchmark wrote outside live outputs");
  };
  const auto& graphics=vulkan.RuntimeContext();
  const auto families=graphics.physical_device.getQueueFamilyProperties();
  Require(name,"actual timestamp capability setup",graphics.queue_family<families.size() &&
      families[graphics.queue_family].timestampValidBits>0,
      "queue has no timestamps; unsupported is not a benchmark result");
  const auto valid_bits=families[graphics.queue_family].timestampValidBits;
  const auto mask=valid_bits>=64 ? UINT64_MAX : (uint64_t{1}<<valid_bits)-1u;
  const auto period=graphics.physical_device_properties.limits.timestampPeriod;
  Require(name,"timestamp period setup",period>0,"invalid GPU timestamp period");
  vk::QueryPoolCreateInfo query{};query.queryType=vk::QueryType::eTimestamp;query.queryCount=2;
  vk::QueryPool timestamps=nullptr;
  RequireVulkanSuccess(vulkan.Device().createQueryPool(&query,nullptr,&timestamps),"microbench timestamp pool");
  auto& scheduler=vulkan.RuntimeRenderer().GetCommandScheduler();
  const auto Sample=[&](uint32_t repeats) {
    const auto command=scheduler.Current().Handle();
    command.resetQueryPool(timestamps,0,2);
    vk::MemoryBarrier ready{};ready.srcAccessMask=vk::AccessFlagBits::eHostWrite;
    ready.dstAccessMask=vk::AccessFlagBits::eShaderRead|vk::AccessFlagBits::eShaderWrite;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eHost,vk::PipelineStageFlagBits::eComputeShader,
                            {},1,&ready,0,nullptr,0,nullptr);
    command.bindPipeline(vk::PipelineBindPoint::eCompute,pipeline);
    command.bindDescriptorSets(vk::PipelineBindPoint::eCompute,pipeline_layout,0,1,&set,0,nullptr);
    if(layout.UsesPushData()) {
      FetchGpuIR::PushData push;
      std::copy(data.begin(),data.end(),push.dwords.begin()+layout.push_data_start_dword);
      command.pushConstants(pipeline_layout,vk::ShaderStageFlagBits::eCompute,0,sizeof(push),push.dwords.data());
    }
    command.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe,timestamps,0);
    for(uint32_t repeat=0;repeat<repeats;++repeat) {
      if(repeat!=0) {
        // Same output range between dispatches: retain real WAW ordering, not a same-value race.
        vk::MemoryBarrier order{};order.srcAccessMask=order.dstAccessMask=vk::AccessFlagBits::eShaderWrite;
        command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                                vk::PipelineStageFlagBits::eComputeShader,{},1,&order,0,nullptr,0,nullptr);
      }
      command.dispatch(1,1,1);
    }
    command.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,timestamps,1);
    vk::MemoryBarrier done{};done.srcAccessMask=vk::AccessFlagBits::eShaderWrite;
    done.dstAccessMask=vk::AccessFlagBits::eHostRead;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,vk::PipelineStageFlagBits::eHost,
                            {},1,&done,0,nullptr,0,nullptr);
    scheduler.Finish();
    std::array<uint64_t,2> ticks{};
    RequireVulkanSuccess(vulkan.Device().getQueryPoolResults(timestamps,0,2,sizeof(ticks),ticks.data(),
        sizeof(uint64_t),vk::QueryResultFlagBits::e64),"completed microbench timestamps");
    const double elapsed_ns=static_cast<double>((ticks[1]-ticks[0])&mask)*period;
    Require(name,"finite nonzero GPU timing",elapsed_ns>0 && elapsed_ns<25000000.0,
            "single/sample workload exceeded25ms; stop instead of growing it");
    return elapsed_ns;
  };
  Sample(1);CheckOutput();Sample(1);
  const auto single_ns=Sample(1);
  const uint32_t repeats=fixed_repeats!=0 ? fixed_repeats :
      static_cast<uint32_t>(std::clamp(2000000.0/single_ns,1.0,256.0));
  Require(name,"bounded calibrated batch",single_ns*repeats<25000000.0,
          "requested repeats exceed measured25ms safety budget");
  std::vector<double> samples;
  for(uint32_t i=0;i<9;++i) {
    const auto elapsed=Sample(repeats);
    if(i>=2) {
      samples.push_back(elapsed/repeats);
      std::printf("[generic-gpu-sample] wave=%u channels=%u repeats=%u sample=%u batch_ns=%.0f ns_dispatch=%.1f\n",
          shader.wave,channels,repeats,i-2,elapsed,elapsed/repeats);
    }
  }
  CheckOutput();std::sort(samples.begin(),samples.end());
  std::printf("[generic-gpu-bench] wave=%u channels=%u reads=32 words_live=%u repeats=%u samples=7 median_ns_dispatch=%.1f min_ns=%.1f max_ns=%.1f SPVwords=%zu golden=PASS\n",
      shader.wave,channels,shader.wave*128u,repeats,samples[3],samples.front(),samples.back(),shader.compiled.spirv.size());
  vulkan.Device().destroyQueryPool(timestamps,nullptr);
}
