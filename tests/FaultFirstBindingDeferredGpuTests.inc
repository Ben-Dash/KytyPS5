// Actual first FaultBuffer binding through the renderer's deferred descriptor payload.
// This checks recording order, not shader arithmetic or rendered pixels.
#include "graphics/host_gpu/renderer/commandHooks.h"
#include <atomic>

namespace Libs::Graphics {
namespace {
void ConfigureFaultFirstBindingDeferredGpu() {
  _putenv_s("KYTY_RECORD_THREAD", "1");
  _putenv_s("KYTY_BINDING_PLAN", "1");
  _putenv_s("KYTY_DEBUG_AB", "");
}

struct FaultDeferredProbe {
  std::atomic<bool>* observed;
};

void ObserveFaultDeferredProbe(VkCommandBuffer, const uint8_t* payload) {
  FaultDeferredProbe probe{};
  std::memcpy(&probe, payload, sizeof(probe));
  probe.observed->store(true, std::memory_order_release);
}

void CheckFaultFirstBindingDeferredGpu(VulkanHarness& vulkan) {
  constexpr const char* name = "FaultFirstBindingDeferredGpu";
  auto& context = vulkan.RuntimeRenderer();
  auto& scheduler = context.GetCommandScheduler();
  HW::Context registers{};
  HW::UserConfig user_config{};
  HW::Shader shaders{};
  scheduler.Begin(registers, user_config, shaders);

  // Prove this thread really owns a routed command buffer; merely enabling an
  // environment switch would otherwise permit the direct path to pass.
  std::atomic<bool> observed{false};
  auto* payload = ReserveRecordedCall(
      static_cast<VkCommandBuffer>(scheduler.Current().Handle()), sizeof(FaultDeferredProbe));
  Require(name, "actual deferred stream setup", CommandRecordingDeferred() && payload != nullptr,
          "no real routed reservation; this is a setup failure, not a regression");
  const FaultDeferredProbe probe{&observed};
  std::memcpy(payload, &probe, sizeof(probe));
  CommitRecordedCall(ObserveFaultDeferredProbe);
  const auto probe_tick = scheduler.CurrentTick();
  scheduler.Finish();
  scheduler.WaitPriorityOperations(probe_tick);
  Require(name, "actual deferred stream positive control",
          observed.load(std::memory_order_acquire), "the reserved callback did not execute");

  ShaderRecompiler::IR::CompiledShaderInfo program{};
  program.stage = ShaderType::Compute;
  program.bindings.descriptors.push_back(
      {ShaderRecompiler::IR::DescriptorBindingKind::FaultBuffer, {}});
  ShaderRecompiler::IR::ResourceSnapshot snapshot{};
  ShaderStageRuntime runtime{&program, &snapshot};
  PreparedBindings prepared;
  // The first resource exposure must happen through the real preparation path.
  // In particular, do not initialize the bitmap by calling GetFaultBuffer here.
  context.GetRenderExecutor().PrepareBindings(runtime, prepared);

  vk::DescriptorSetLayoutBinding binding{};
  binding.binding = ShaderRecompiler::IR::NativeBinding(
      ShaderType::Compute, ShaderRecompiler::IR::DescriptorBindingKind::FaultBuffer);
  binding.descriptorType = vk::DescriptorType::eStorageBuffer;
  binding.descriptorCount = 1u;
  binding.stageFlags = vk::ShaderStageFlagBits::eCompute;
  vk::DescriptorSetLayoutCreateInfo set_info{};
  set_info.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
  set_info.bindingCount = 1u;
  set_info.pBindings = &binding;
  PipelineCache::Pipeline pipeline{};
  pipeline.uses_push_descriptors = true;
  RequireVulkanSuccess(vulkan.Device().createDescriptorSetLayout(
      &set_info, nullptr, &pipeline.descriptor_set_layout), "first fault native descriptor layout");
  vk::PipelineLayoutCreateInfo layout_info{};
  layout_info.setLayoutCount = 1u;
  layout_info.pSetLayouts = &pipeline.descriptor_set_layout;
  RequireVulkanSuccess(vulkan.Device().createPipelineLayout(
      &layout_info, nullptr, &pipeline.pipeline_layout), "first fault native pipeline layout");

  std::array<PreparedBindings*, 1> stages{&prepared};
  std::printf("[host] first fault binding: routed=1 probe=1 planned=1 first_exposure=PrepareBindings; shader_dispatches=0\n");
  context.GetRenderExecutor().CommitBindings(scheduler.Current(),
      vk::PipelineBindPoint::eCompute, pipeline, stages);
  // Finish all recorded work before destroying objects referenced by the payload.
  const auto binding_tick = scheduler.CurrentTick();
  scheduler.Finish();
  scheduler.WaitPriorityOperations(binding_tick);
  vulkan.Device().destroyPipelineLayout(pipeline.pipeline_layout, nullptr);
  vulkan.Device().destroyDescriptorSetLayout(pipeline.descriptor_set_layout, nullptr);
  std::printf("[host] %-32s ok (actual preparation/planned deferred binding; no shader dispatch)\n", name);
}
} // namespace
} // namespace Libs::Graphics
