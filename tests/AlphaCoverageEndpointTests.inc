// Include INSIDE VulkanHarness before CheckNativeIndirectDispatch.
// Independent endpoint oracle through PM4 -> actual RenderExecutor -> depth GPUreadback.
// No intermediate-alpha/dither oracle is fabricated. Raw2222/round0 is the AMD
// documented nondithered configuration. Explicit EXEC_ON_NOOP isolates pipeline
// A2C from the separate PS-retention path. No artificial color attachment.
  void CheckAlphaCoverageEndpoints() {
    constexpr const char* name = "AlphaCoverageEndpoints";
    constexpr uintptr_t base = 0x0000000207800000ull;
    constexpr uint64_t bytes = 0x100000u, vertex_address = base + 0x10000u;
    constexpr uint32_t side = 16u, guard = 0x7a19e51bu;
    const std::array<float, 12> vertices{-1.f,-1.f,.25f,1.f, 3.f,-1.f,.25f,1.f,
                                        -1.f,3.f,.25f,1.f};
    std::vector<u32> vertex_code{EncodeVop1(0x01u,20u,Vgpr(5u)),
        EncodeMubuf0(0x0eu,0u,true,false),EncodeMubuf1(0u,2u,20u),
        EncodeExp0(0x0cu,0xfu),EncodeExp1(0u,1u,2u,3u)};
    AppendEnd(&vertex_code);
    std::array<std::vector<u32>,2> pixels;
    for (u32 alpha=0u;alpha<2u;++alpha) {
      auto& code=pixels[alpha];AppendVMovU32(&code,0u,0u);
      AppendVMovLiteral(&code,1u,std::bit_cast<u32>(float(alpha)));
      code.insert(code.end(),{EncodeExp0(0u,0xfu),EncodeExp1(0u,0u,0u,1u)});
      AppendEnd(&code);
      ShaderMapUserData(reinterpret_cast<uint64_t>(code.data()),
          {.type=Prospero::ShaderBinaryType::kPs,.code_size_bytes=u32(code.size()*4u)});
    }
    static ShaderUserData native_user_data{};
    ShaderMapUserData(reinterpret_cast<uint64_t>(vertex_code.data()),
        {.type=Prospero::ShaderBinaryType::kGs,.user_data=&native_user_data,
         .code_size_bytes=u32(vertex_code.size()*4u)});
    EnsureRuntimeContext();
    int64_t direct=-1;
    Require(name,"tracked guest allocation",LibKernel::Memory::KernelAllocateDirectMemory(
        0,LibKernel::Memory::KernelGetDirectMemorySize(),bytes,0x10000u,0,&direct)==0,
        "setup allocation failure; not semantic RED");
    void* mapped=reinterpret_cast<void*>(base);
    Require(name,"tracked guest mapping",LibKernel::Memory::KernelMapDirectMemory(
        &mapped,bytes,0x3u,0x10u,direct,0x10000u)==0 && mapped==reinterpret_cast<void*>(base),
        "setup mapping failure; not semantic RED");
    std::memset(mapped,0,bytes);
    std::memcpy(reinterpret_cast<void*>(vertex_address),vertices.data(),sizeof(vertices));
    struct Result {u32 samples,alpha;bool enabled,override_off,force_ps,correct,guards;
                   std::array<u32,4> actual;};
    std::vector<Result> results;
    {
      RenderContext context(m_runtime_context);context.InitializeGpu(nullptr);
      LibKernel::Memory::InstallGpuResources(&context);
      context.GetGpu().SendCommandSync([&] {
        GraphicsInitJmpTables();CommandProcessor cp(context,0);cp.BufferInit();
        context.MapMemory(base,bytes);
        auto& regs=cp.GetCtx();auto& ucfg=cp.GetUcfg();auto& shaders=cp.GetShCtx();
        auto& scheduler=context.GetCommandScheduler();auto& cache=context.GetTextureCache();
        auto& executor=context.GetRenderExecutor();
        regs.SetViewportTransformControl(0x300u);
        regs.SetViewportScaleOffset(0u,side/2.f,side/2.f,side/2.f,side/2.f,1.f,0.f);
        regs.SetViewportZMax(0u,1.f);regs.SetScreenScissor(0,0,side,side);
        regs.SetWindowScissor(0,0,side,side,false);regs.SetGenericScissor(0,0,side,side,false);
        regs.SetViewportScissor(0u,0,0,side,side,false);regs.SetModeControl({});
        regs.SetRenderTargetMask(0u);regs.SetShaderMask(0xfu);
        regs.SetTargetOutputMode(0u,9u); // noninteger alpha-capable 32_ABGR, full uncompressed EXP
        regs.SetPsInControl(0x8000u);regs.SetPsInputEna(0u);regs.SetPsInputAddr(0u);
        regs.SetDepthControl({.z_enable=true,.z_write_enable=true,
                            .zfunc=static_cast<uint8_t>(vk::CompareOp::eAlways)});
        ucfg.SetPrimitiveType(Prospero::PrimitiveType::kTriList);
        shaders.SetEsShaderBase(reinterpret_cast<uint64_t>(vertex_code.data()));
        shaders.SetGsShaderResource2({.user_sgpr=4u});
        ShaderBufferResource vbo{};vbo.UpdateAddress48(vertex_address);
        vbo.fields[1]|=16u<<16u;vbo.fields[2]=3u;
        vbo.fields[3]=DstSel(4,5,6,7)|(u32(Prospero::BufferFormat::k32_32_32_32Float)<<12u);
        for(u32 i=0u;i<4u;++i)shaders.SetGsUserSgpr(i,vbo.fields[i],HW::UserSgprType::Vsharp);
        const auto set_context=[&](u32 offset,u32 value) {
          const std::array<u32,3> packet{KYTY_PM4(3,Pm4::IT_SET_CONTEXT_REG,Pm4::R_ZERO),offset,value};
          Pm4Execution execution;
          Require(name,"real PM4 context decode",cp.Process(execution,packet)==Pm4ProcessResult::Complete,
                  "PM4 setup did not complete; not semantic RED");
        };
        // Independent MS depth observer already used by the older native depth tests.
        vk::ShaderModule module=CreateShaderModule(name,
            std::vector<u32>(std::begin(GPU_TEST_MS_DEPTH_SPV),std::end(GPU_TEST_MS_DEPTH_SPV)));
        const std::array<vk::DescriptorSetLayoutBinding,2> bindings{{
          {0u,vk::DescriptorType::eSampledImage,1u,vk::ShaderStageFlagBits::eCompute,nullptr},
          {1u,vk::DescriptorType::eStorageBuffer,1u,vk::ShaderStageFlagBits::eCompute,nullptr}}};
        vk::DescriptorSetLayoutCreateInfo layout_info{};
        layout_info.flags=vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
        layout_info.bindingCount=2u;layout_info.pBindings=bindings.data();
        vk::DescriptorSetLayout set_layout=nullptr;
        RequireVk(name,"MS observer descriptor layout",m_device.createDescriptorSetLayout(
            &layout_info,nullptr,&set_layout),"vkCreateDescriptorSetLayout");
        const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute,0u,sizeof(u32)};
        vk::PipelineLayoutCreateInfo pipeline_layout_info{};
        pipeline_layout_info.setLayoutCount=1u;pipeline_layout_info.pSetLayouts=&set_layout;
        pipeline_layout_info.pushConstantRangeCount=1u;pipeline_layout_info.pPushConstantRanges=&push;
        vk::PipelineLayout observer_layout=nullptr;
        RequireVk(name,"MS observer pipeline layout",m_device.createPipelineLayout(
            &pipeline_layout_info,nullptr,&observer_layout),"vkCreatePipelineLayout");
        vk::ComputePipelineCreateInfo pipeline_info{};
        pipeline_info.stage.stage=vk::ShaderStageFlagBits::eCompute;
        pipeline_info.stage.module=module;
        pipeline_info.stage.pName="main";
        pipeline_info.layout=observer_layout;vk::Pipeline observer=nullptr;
        RequireVk(name,"MS observer pipeline",m_device.createComputePipelines(
            nullptr,1u,&pipeline_info,nullptr,&observer),"vkCreateComputePipelines");
        for (u32 samples:{1u,4u}) {
          const u32 encoded=samples==1u?0u:2u;
          Require(name,"actual depth sample capability",bool(
              context.GetGraphics().GetPhysicalDeviceProperties().limits.framebufferDepthSampleCounts &
              vulkan_sample_count(samples)),"device cannot represent fixture; not semantic RED");
          HW::DepthRenderTarget target{};target.z_info.format=Prospero::DepthFormat::kZ32F;
          target.z_info.num_samples=encoded;
          target.z_info.texture_compatibility=Prospero::TextureCompatiblePlaneCompression::kEnable;
          target.z_read_base_addr=target.z_write_base_addr=base+(samples==1u?0x40000u:0x60000u);
          target.size.valid=true;target.size.x_max=target.size.y_max=side-1u;
          regs.SetDepthRenderTarget(target);
          set_context(Pm4::DB_EQAA,(encoded<<Pm4::DB_EQAA_MAX_ANCHOR_SAMPLES_SHIFT)|
              (encoded<<Pm4::DB_EQAA_ALPHA_TO_MASK_NUM_SAMPLES_SHIFT));
          RenderDepthInfo depth{};
          RenderExecutorTestAccess::ResolveRenderDepthTarget(executor,scheduler.Current(),depth);
          Require(name,"real discovered depth-only shape",depth.image_id &&
              depth.desc.info.extent==vk::Extent3D{side,side,1u} &&
              depth.desc.info.samples==samples && depth.desc.info.pixel_format==vk::Format::eD32Sfloat,
              "depth setup invalid; not semantic RED");
          auto readback=CreateHostBuffer(name,(side*side+8u)*sizeof(u32),
              vk::BufferUsageFlagBits::eTransferDst|vk::BufferUsageFlagBits::eStorageBuffer,
              std::vector<u32>(side*side+8u,guard));
          for(bool force_ps:{false,true})for(bool enabled:{false,true})
            for(bool override_off:{false,true})for(u32 alpha:{0u,1u}) {
              const u32 raw=0x0000aa00u|(enabled?1u:0u);
              set_context(Pm4::DB_ALPHA_TO_MASK,raw);
              const u32 db=(override_off?(1u<<Pm4::DB_SHADER_CONTROL_ALPHA_TO_MASK_DISABLE_SHIFT):0u)|
                           (force_ps?(1u<<Pm4::DB_SHADER_CONTROL_EXEC_ON_NOOP_SHIFT):0u);
              set_context(Pm4::DB_SHADER_CONTROL,db);
              Require(name,"decoded A2C fixture controls",regs.GetAlphaToMaskRaw()==raw &&
                  regs.GetShaderRegisters().db_shader_control.alpha_to_mask_disable==override_off &&
                  regs.GetShaderRegisters().db_shader_control.shader_execute_on_noop==force_ps &&
                  regs.GetEqaaControl().alpha_to_mask_num_samples==encoded &&
                  regs.GetRenderTargetMask()==0u,"fixture register decoder mismatch; not semantic RED");
              shaders.SetPsShaderBase(reinterpret_cast<uint64_t>(pixels[alpha].data()));
              vk::ClearValue clear{};clear.depthStencil=vk::ClearDepthStencilValue{.75f,0u};
              TextureCacheTestAccess::ClearImage(cache,scheduler.Current(),depth.image_id,
                  {vk::ImageAspectFlagBits::eDepth,0u,1u,0u,1u},clear);
              cp.DrawIndexAuto({.vertex_count=3u,.instance_count=1u});
              scheduler.EndRendering();
              auto& image=cache.GetImage(depth.image_id);
              if(samples==1u) {
                const vk::BufferImageCopy copy{0u,0u,0u,
                    {vk::ImageAspectFlagBits::eDepth,0u,0u,1u},{},{side,side,1u}};
                image.Download(std::span{&copy,1u},readback.buffer,0u,readback.size,readback.size);
              } else {
                image.Transit(vk::ImageLayout::eShaderReadOnlyOptimal,vk::AccessFlagBits2::eShaderRead,
                              {},scheduler.Current().Handle());
                ImageViewInfo view{};view.format=image.backing.format;view.type=vk::ImageViewType::e2D;
                view.aspect=vk::ImageAspectFlagBits::eDepth;view.usage=vk::ImageUsageFlagBits::eSampled;
                const vk::DescriptorImageInfo image_info{nullptr,image.FindView(view),
                                                        vk::ImageLayout::eShaderReadOnlyOptimal};
                const vk::DescriptorBufferInfo output_info{readback.buffer,0u,readback.size};
                std::array<vk::WriteDescriptorSet,2> writes{};
                writes[0].dstBinding=0u;writes[0].descriptorCount=1u;
                writes[0].descriptorType=vk::DescriptorType::eSampledImage;writes[0].pImageInfo=&image_info;
                writes[1].dstBinding=1u;writes[1].descriptorCount=1u;
                writes[1].descriptorType=vk::DescriptorType::eStorageBuffer;writes[1].pBufferInfo=&output_info;
                auto command=scheduler.Current().Handle();
                command.bindPipeline(vk::PipelineBindPoint::eCompute,observer);
                command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,observer_layout,0u,writes);
                command.pushConstants(observer_layout,vk::ShaderStageFlagBits::eCompute,0u,sizeof(samples),&samples);
                command.dispatch(1u,1u,1u);
              }
              vk::MemoryBarrier2 host{};host.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands;
              host.srcAccessMask=vk::AccessFlagBits2::eMemoryWrite;
              host.dstStageMask=vk::PipelineStageFlagBits2::eHost;host.dstAccessMask=vk::AccessFlagBits2::eHostRead;
              vk::DependencyInfo dependency{};dependency.memoryBarrierCount=1u;dependency.pMemoryBarriers=&host;
              scheduler.Current().Handle().pipelineBarrier2(dependency);
              const auto tick=scheduler.CurrentTick();scheduler.Finish();scheduler.WaitPriorityOperations(tick);
              const auto words=ReadBuffer(name,readback,side*side+8u);
              const auto expected=std::bit_cast<u32>(enabled&&!override_off&&alpha==0u?.75f:.25f);
              const u32 observed=samples==1u?side*side:samples;
              const bool correct=std::ranges::all_of(std::span{words}.first(observed),
                  [=](u32 value){return value==expected;});
              const bool canaries=std::ranges::all_of(std::span{words}.subspan(side*side,8u),
                  [=](u32 value){return value==guard;});
              results.push_back({samples,alpha,enabled,override_off,force_ps,correct,canaries,
                                {words[0],words[1],words[2],words[3]}});
              RenderExecutorTestAccess::ResetBindings(executor);
          }
          DestroyBuffer(&readback);
        }
        scheduler.Finish();scheduler.DrainPriorityOperations();
        m_device.destroyPipeline(observer,nullptr);m_device.destroyPipelineLayout(observer_layout,nullptr);
        m_device.destroyDescriptorSetLayout(set_layout,nullptr);m_device.destroyShaderModule(module,nullptr);
        RenderExecutorTestAccess::ResetBindings(executor);context.UnmapMemory(base,bytes);
      });
      LibKernel::Memory::InstallGpuResources(nullptr);context.ShutdownGpu();
    }
    Require(name,"task guest cleanup",LibKernel::Memory::KernelMunmap(base,bytes)==0 &&
        LibKernel::Memory::KernelReleaseDirectMemory(direct,bytes)==0,"mapping cleanup failed");
    bool controls=true,coverage=true,retention=true,canaries=true;
    for(const auto& r:results) {
      std::printf("[a2c-endpoint] samples=%u alpha=%u enable=%u override=%u forcePS=%u correct=%u guard=%u depth=%08x,%08x,%08x,%08x\n",
          r.samples,r.alpha,r.enabled,r.override_off,r.force_ps,r.correct,r.guards,
          r.actual[0],r.actual[1],r.actual[2],r.actual[3]);
      canaries&=r.guards;
      if(!r.enabled||r.override_off||r.alpha==1u)controls&=r.correct;
      else if(r.force_ps)coverage&=r.correct;
      else retention&=r.correct;
    }
    Require(name,"positive fixed-function/override/alpha1 controls",controls && canaries,
            "producer/readback/override control failed; not intended A2C RED");
    Require(name,"actual alpha0 coverage with PS explicitly retained",coverage,
            "alpha0 failed to remove depth coverage at1x/4x");
    Require(name,"depth-only alpha coverage retains required pixel shader",retention,
            "alpha coverage pixel shader omitted when color writes disabled");
  }
