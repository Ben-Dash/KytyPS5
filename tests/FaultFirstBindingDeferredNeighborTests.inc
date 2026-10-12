// First preparation precedes a new rendering instance or compute chain.
// These are actual command-buffer state/protocol checks, with no shader dispatch.
namespace Libs::Graphics {
namespace {
void CheckFaultFirstBindingDeferredNeighborsGpu(VulkanHarness& vulkan) {
  constexpr const char* name = "FaultFirstBindingDeferredNeighborsGpu";
  for (const bool graphics : {true, false}) {
    bool before = false, after_prepare = false, after_commit = false, probe_observed = false;
    {
      RenderContext context(vulkan.RuntimeContext());
      auto& scheduler = context.GetCommandScheduler();
      HW::Context registers{};
      HW::UserConfig user_config{};
      HW::Shader shaders{};
      scheduler.Begin(registers, user_config, shaders);
      std::atomic<bool> observed{false};
      auto* payload = ReserveRecordedCall(
          static_cast<VkCommandBuffer>(scheduler.Current().Handle()), sizeof(FaultDeferredProbe));
      Require(name, "actual deferred routing", CommandRecordingDeferred() && payload != nullptr,
              "fresh actual scheduler did not route the descriptor command stream");
      const FaultDeferredProbe probe{&observed};
      std::memcpy(payload, &probe, sizeof(probe));
      CommitRecordedCall(ObserveFaultDeferredProbe);

      ShaderRecompiler::IR::CompiledShaderInfo program{};
      program.stage = graphics ? ShaderType::Vertex : ShaderType::Compute;
      program.bindings.descriptors.push_back(
          {ShaderRecompiler::IR::DescriptorBindingKind::FaultBuffer, {}});
      ShaderRecompiler::IR::ResourceSnapshot snapshot{};
      ShaderStageRuntime runtime{&program, &snapshot};
      PreparedBindings prepared;
      context.GetRenderExecutor().PrepareBindings(runtime, prepared);
      vk::DescriptorSetLayoutBinding binding{};
      binding.binding = ShaderRecompiler::IR::NativeBinding(
          program.stage, ShaderRecompiler::IR::DescriptorBindingKind::FaultBuffer);
      binding.descriptorType = vk::DescriptorType::eStorageBuffer;
      binding.descriptorCount = 1u;
      binding.stageFlags = graphics ? vk::ShaderStageFlagBits::eVertex
                                   : vk::ShaderStageFlagBits::eCompute;
      vk::DescriptorSetLayoutCreateInfo set_info{};
      set_info.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
      set_info.bindingCount = 1u;
      set_info.pBindings = &binding;
      PipelineCache::Pipeline pipeline{};
      pipeline.uses_push_descriptors = true;
      RequireVulkanSuccess(vulkan.Device().createDescriptorSetLayout(
          &set_info, nullptr, &pipeline.descriptor_set_layout), "first fault neighbor descriptor layout");
      vk::PipelineLayoutCreateInfo layout_info{};
      layout_info.setLayoutCount = 1u;
      layout_info.pSetLayouts = &pipeline.descriptor_set_layout;
      RequireVulkanSuccess(vulkan.Device().createPipelineLayout(
          &layout_info, nullptr, &pipeline.pipeline_layout), "first fault neighbor pipeline layout");

      auto& command = scheduler.Current();
      if (graphics) {
        RenderState rendering{};
        rendering.width = rendering.height = rendering.num_layers = 1u;
        command.BeginRendering(rendering);
      } else {
        command.ContinueComputeChain();
      }
      before = graphics ? command.IsRendering() : command.ComputeChainPending();
      // Already initialized preparation must also be a pure resource lookup.
      context.GetRenderExecutor().PrepareBindings(runtime, prepared);
      after_prepare = graphics ? command.IsRendering() : command.ComputeChainPending();
      std::array<PreparedBindings*, 1> stages{&prepared};
      context.GetRenderExecutor().CommitBindings(command,
          graphics ? vk::PipelineBindPoint::eGraphics : vk::PipelineBindPoint::eCompute,
          pipeline, stages, !graphics);
      after_commit = graphics ? command.IsRendering() : command.ComputeChainPending();
      const auto tick = scheduler.CurrentTick();
      scheduler.Finish();
      scheduler.WaitPriorityOperations(tick);
      vulkan.Device().destroyPipelineLayout(pipeline.pipeline_layout, nullptr);
      vulkan.Device().destroyDescriptorSetLayout(pipeline.descriptor_set_layout, nullptr);
      probe_observed = observed.load(std::memory_order_acquire);
    } // Destroy the fresh renderer after its recorded work completes, before assertions.
    std::printf("[host] first fault neighbor: graphics=%u probe=%u before=%u afterPrepare=%u afterCommit=%u; shader_dispatches=0\n",
        graphics ? 1u : 0u, probe_observed ? 1u : 0u, before ? 1u : 0u,
        after_prepare ? 1u : 0u, after_commit ? 1u : 0u);
    Require(name, "deferred callback positive control", probe_observed,
            "fresh real command stream did not consume the reserved callback");
    Require(name, graphics ? "rendering preservation" : "compute-chain preservation",
            before && after_prepare && after_commit,
            "initialized preparation or deferred binding consumed the later command-buffer state");
  }
  std::printf("[host] %-32s ok (actual rendering/compute-chain protocol; no shader dispatch)\n", name);
}
} // namespace
} // namespace Libs::Graphics
