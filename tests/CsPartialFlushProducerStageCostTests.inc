// INSIDE ShaderRecompilerComputeTests VulkanHarness. Globally include CsPartialFlushBarrierAudit.inc.
// No production API addition required for RED; actual existing finite dispatch/TriggerEvent/FillBuffer paths.
// Real shader and ownership controls plus cleanup precede the final source-stage cost assertion.
void CheckCsPartialFlushProducerStageCost() {
  constexpr const char* name="CsPartialFlushProducerStageCost";
  constexpr uint64_t base=0x0000000208e00000ull,bytes=0x40000u;
  constexpr uint64_t first_address=base+0x10000u,second_address=base+0x20000u,transfer_address=base+0x30000u;
  constexpr uint32_t lanes=64u,sentinel=0xdeadbeefu;
  EnsureRuntimeContext();
  RenderContext context(m_runtime_context);context.InitializeGpu(nullptr);
  int64_t direct=-1;
  Require(name,"owned direct allocation",LibKernel::Memory::KernelAllocateDirectMemory(0,
      LibKernel::Memory::KernelGetDirectMemorySize(),bytes,0x10000u,0,&direct)==0,
      "owned allocation failed; not native visibility proof");
  void* mapped=reinterpret_cast<void*>(base);
  Require(name,"owned direct mapping",LibKernel::Memory::KernelMapDirectMemory(&mapped,bytes,
      0x3,0x10,direct,0x10000u)==0&&mapped==reinterpret_cast<void*>(base),
      "owned mapping failed; not native visibility proof");
  std::fill_n(static_cast<u32*>(mapped),bytes/4u,sentinel);
  for(u32 i=0;i<lanes;++i) {
    reinterpret_cast<u32*>(first_address)[i]=0xfffffff0u+i;
    reinterpret_cast<u32*>(second_address)[i]=0x12345000u+i;
    reinterpret_cast<u32*>(transfer_address)[i]=0x87654000u+i;
  }
  LibKernel::Memory::InstallGpuResources(&context);context.MapMemory(base,bytes);
  std::vector<u32> code;
  code.push_back(EncodeMubuf0(0x0cu,0,true,false));code.push_back(EncodeMubuf1(1u,0u,0u));
  code.push_back(EncodeVop2(0x25u,1u,InlineU32(1u),1u));
  code.push_back(EncodeMubuf0(0x1cu,0,true,false));code.push_back(EncodeMubuf1(1u,0u,0u));
  AppendEnd(&code);
  ShaderMapUserData(reinterpret_cast<uint64_t>(code.data()),
    {.type=Prospero::ShaderBinaryType::kCs,.code_size_bytes=static_cast<u32>(code.size()*4u)});
  bool setup=false,independent_pending=false,independent_policy=false;
  bool acquire_policy=true,partial_policy=false,break_policy=false,values=false,guards=false;
  uint64_t independent_access=0,standard_access=0,custom_access=0,partial_full=0,break_access=0;
  std::array<CsPartialFlushBarrierAudit::Observed,2> observed{};
  uint32_t observed_count=0;
  bool transfer_native=false;
  uint64_t other_full_events=0;
  constexpr u32 transfer_fill=0x55667788u;
  const bool enabled=ComputeChainEnabled();
  context.GetGpu().SendCommandSync([&] {
    auto& scheduler=context.GetCommandScheduler();auto& cache=context.GetBufferCache();
    HW::Context registers{};HW::UserConfig config{};HW::Shader shaders{};
    scheduler.Begin(registers,config,shaders);
    CommandProcessor boundary(context,0x31u);
    const auto SetAddress=[&](uint64_t address) {
      shaders.SetCsShader({.data_addr=reinterpret_cast<uint64_t>(code.data()),.num_thread_x=lanes,
        .num_thread_y=1,.num_thread_z=1,.wave_size=64,.user_sgpr=4,.tgid_x_en=false});
      ShaderBufferResource desc{};desc.UpdateAddress48(address);desc.fields[1]|=4u<<16u;
      desc.fields[2]=lanes;desc.fields[3]=DstSel(4,5,6,7)|
        (static_cast<u32>(Prospero::BufferFormat::k32UInt)<<12u);
      for(u32 i=0;i<4;++i)shaders.SetCsUserSgpr(i,desc.fields[i],HW::UserSgprType::Unknown);
    };
    const auto Warm=[&] {
      ShaderComputeInputInfo info{};
      const auto program=context.GetPipelineCache().GetComputeProgram(shaders.GetCs(),registers.GetShaderRegisters(),info);
      Require(name,"actual ordinary LDS-free load/add/store pipeline",bool(program)&&
          info.stage.program!=nullptr&&info.stage.resources!=nullptr&&info.stage.resources->uniform_fill.kind==ShaderRecompiler::IR::UniformFillKind::None&&
          ShaderRecompiler::IR::FindBinding(info.stage.program->bindings,ShaderRecompiler::IR::DescriptorBindingKind::SharedMemory)==nullptr&&
          context.GetPipelineCache().GetComputePipeline(info,program).pipeline!=nullptr,
          "real pipeline not eligible or unexpectedly a fill; no configuration proof");
    };
    const auto Dispatch=[&](uint64_t address) {
      SetAddress(address);
      Require(name,"actual finite dispatch recorded",context.GetRenderExecutor().DispatchDirect(
          0,scheduler.Current(),1,1,1,0x41u),"actual renderer deferred the finite warm pipeline");
    };
    const char* age=KYTY_GETENV("KYTY_RECORDING_AGE_US");
    setup=GuestGpu::IsGpuThread()&&!CommandRecordingDeferred()&&age!=nullptr&&std::strcmp(age,"0")==0;
    SetAddress(first_address);Warm();SetAddress(second_address);Warm();SetAddress(transfer_address);Warm();
    // Prime actual allocation/fault-buffer/bindings before measuring continuation.
    Dispatch(first_address);boundary.BreakComputeChain();Dispatch(second_address);
    boundary.BreakComputeChain();Dispatch(transfer_address);boundary.BreakComputeChain();
    scheduler.Finish();scheduler.WaitPriorityOperations(scheduler.CurrentTick()-1u);
    cache.ReadMemory(first_address,lanes*4u);cache.ReadMemory(second_address,lanes*4u);
    scheduler.Finish();scheduler.DrainPriorityOperations();
    {
      CsPartialFlushBarrierAudit audit;
      Dispatch(first_address);
      const bool first_pending=scheduler.Current().ComputeChainPending();
      const auto before_second=audit.shader_access;
      Dispatch(second_address);independent_access=audit.shader_access-before_second;
      independent_pending=first_pending&&scheduler.Current().ComputeChainPending();
      independent_policy=enabled?(independent_pending&&independent_access==0):
          (!first_pending&&!scheduler.Current().ComputeChainPending()&&independent_access>=1);
      boundary.BreakComputeChain();
      // Dependent read-modify-write pairs are valid only with a real guest sync boundary.
      for(uint32_t packet=0;packet<2u;++packet) {
        Dispatch(first_address);
        const bool pending=scheduler.Current().ComputeChainPending();
        const auto before=audit.shader_access;
        const std::array<uint32_t,6> standard{0x00400000u,1u,0u,0u,0u,10u};
        const std::array<uint32_t,7> custom{0u,0u,0u,0u,0u,0u,0u};
        const auto consumed=packet==0u?CpOpAcquireMem(boundary,0xc0055800u,standard.data(),0,0):
            CpOpAcquireMem(boundary,0xc0061050u,custom.data(),0,0);
        const auto accesses=audit.shader_access-before;
        if(packet==0u)standard_access=accesses;else custom_access=accesses;
        acquire_policy=acquire_policy&&consumed==(packet==0u?6u:7u)&&
            !scheduler.Current().ComputeChainPending()&&
            (enabled?(pending&&accesses==1u):(!pending&&accesses==0u));
        Dispatch(first_address);boundary.BreakComputeChain();
      }
      Dispatch(first_address);const auto before_partial=audit.full_write_visibility;
      audit.capture_partial=true;
      boundary.TriggerEvent(0x07u,4u,0u);
      audit.capture_partial=false;
      partial_full=audit.full_write_visibility-before_partial;
      partial_policy=!scheduler.Current().ComputeChainPending();
      Dispatch(first_address);boundary.BreakComputeChain();
      Dispatch(first_address);const auto before_break=audit.shader_access;
      const bool pending=scheduler.Current().ComputeChainPending();boundary.BreakComputeChain();
      break_access=audit.shader_access-before_break;
      break_policy=!scheduler.Current().ComputeChainPending()&&
          (enabled?(pending&&break_access==1u):(!pending&&break_access==0u));
      // Real native transfer neighbor: the unpublished primer keeps this destination GPU-owned.
      transfer_native=cache.IsRegionGpuModified(transfer_address,lanes*4u);
      cache.FillBuffer(transfer_address,lanes*4u,transfer_fill,false);
      audit.capture_partial=true;
      boundary.TriggerEvent(0x07u,4u,0u);
      audit.capture_partial=false;
      Dispatch(transfer_address);boundary.BreakComputeChain();
      const auto before_other=audit.full_write_visibility;
      boundary.TriggerEvent(0x0fu,4u,0u); // VS partial, independently retained conservative policy.
      boundary.TriggerEvent(0x10u,4u,0u); // PS partial, independently retained conservative policy.
      boundary.TriggerEvent(0x16u,0u,0u); // Real cache writeback/invalidate, full visibility preserved.
      other_full_events=audit.full_write_visibility-before_other;
      observed=audit.partial;observed_count=audit.partial_count;
      scheduler.Finish();scheduler.WaitPriorityOperations(scheduler.CurrentTick()-1u);
      cache.ReadMemory(first_address,lanes*4u);cache.ReadMemory(second_address,lanes*4u);
      cache.ReadMemory(transfer_address,lanes*4u);
      scheduler.Finish();scheduler.DrainPriorityOperations();
    } // Restore every native dispatcher after submitted work and publication are retired.
    values=true;guards=true;
    for(const auto address:{first_address,second_address,transfer_address}) {
      std::array<u32,lanes+8u> actual{};
      Require(name,"real published range including guards",LibKernel::Memory::TryReadBacking(
          address-16u,actual.data(),sizeof(actual)),"native output not readable after retirement");
      for(u32 i=0;i<actual.size();++i) {
        if(i>=4u&&i<4u+lanes) {
          const u32 seed=address==first_address?0xfffffff0u:0x12345000u;
          // Bootstrap1 + disjoint1 + two acquirepairs4 + partialpair2 + lastbreak1.
          const u32 increments=address==first_address?9u:2u;
          const u32 expected=address==transfer_address?transfer_fill+1u:u32(seed+(i-4u)+increments);
          values=values&&actual[i]==expected;
        }else guards=guards&&actual[i]==sentinel;
      }
    }
    context.UnmapMemory(base,bytes);
  });
  context.ShutdownGpu();LibKernel::Memory::InstallGpuResources(nullptr);
  Require(name,"owned mapping cleanup",LibKernel::Memory::KernelMunmap(base,bytes)==0,"mapping cleanup failed");
  Require(name,"owned allocation cleanup",LibKernel::Memory::KernelReleaseDirectMemory(direct,bytes)==0,"allocation cleanup failed");
  std::printf("[compute-chain] enabled=%u setup=%u independentPending=%u independentAccess=%llu acquire=%llu/%llu partialFull=%llu breakAccess=%llu values=%u guards=%u\n",
      enabled?1u:0u,setup?1u:0u,independent_pending?1u:0u,static_cast<unsigned long long>(independent_access),
      static_cast<unsigned long long>(standard_access),static_cast<unsigned long long>(custom_access),
      static_cast<unsigned long long>(partial_full),static_cast<unsigned long long>(break_access),values?1u:0u,guards?1u:0u);
  Require(name,"actual eligibility/disjoint continuation/guest visibility/current bytes/guards",
      setup&&independent_policy&&acquire_policy&&partial_policy&&break_policy&&values&&guards&&transfer_native&&other_full_events==3u,
      "real configuration visibility/protocol/numeric control failed; no safe flag trial");
  const auto AllCommands=VkPipelineStageFlags2(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
  const auto Compute=VkPipelineStageFlags2(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
  const auto Transfer=VkPipelineStageFlags2(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT);
  const auto Allowed=Compute|Transfer; // Neither fixture producer lowers to a color attachment clear.
  bool preserved=observed_count==2u,narrow=observed_count==2u;
  for(u32 i=0;i<observed.size();++i) {
    const auto& event=observed[i];
    const auto required=Compute|(i==1u?Transfer:0u);
    const bool source_covers=(event.source&AllCommands)!=0||(event.source&required)==required;
    preserved=preserved&&source_covers&&(event.destination&AllCommands)!=0&&
      (event.source_access&VK_ACCESS_2_MEMORY_WRITE_BIT)!=0&&
      (event.destination_access&(VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT))==
        (VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT);
    narrow=narrow&&(event.source&~Allowed)==0u;
    std::printf("[cs-partial-cost] event=%u src=%llx dst=%llx srcAccess=%llx dstAccess=%llx\n",i,
      static_cast<unsigned long long>(event.source),static_cast<unsigned long long>(event.destination),
      static_cast<unsigned long long>(event.source_access),static_cast<unsigned long long>(event.destination_access));
  }
  Require(name,"actual compute/transfer visibility coverage after complete cleanup",preserved,
      "required producer or consumer coverage was lost; this is a semantic failure");
  // This LAST assertion is the intended cost RED. Numerical controls and cleanup above must pass first.
  Require(name,"CS partial waits only supported compute-lowering producer stages",narrow,
      "current CS partial flush includes unrelated full graphics/ALL_COMMANDS producer stages");
}
