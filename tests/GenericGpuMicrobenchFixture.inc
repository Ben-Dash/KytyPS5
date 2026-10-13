// Include globally AFTER GenericBufferCodegenTests.inc (method is inside GenericGpuPipeline).
namespace Libs::Graphics {
namespace {
void CheckGenericGpuMicrobench(VulkanHarness& vulkan) {
  GenericCompilerProfile();FetchGpuGuest guest;
  auto& graphics=vulkan.RuntimeContext();
  vk::PhysicalDeviceSubgroupProperties subgroup{};
  vk::PhysicalDeviceProperties2 properties{};properties.pNext=&subgroup;
  graphics.physical_device.getProperties2(&properties);
  Require("GenericGpuMicrobench","actual subgroup capability setup",subgroup.subgroupSize==32u &&
      static_cast<bool>(subgroup.supportedStages&vk::ShaderStageFlagBits::eCompute) &&
      static_cast<bool>(subgroup.supportedOperations&vk::SubgroupFeatureFlagBits::eBallot),
      "synthetic wave32/64 profile requires actual32lane ballot support");
  HW::Context registers{};HW::UserConfig user_config{};HW::Shader shaders{};
  vulkan.RuntimeRenderer().GetCommandScheduler().Begin(registers,user_config,shaders);
  std::array<unsigned,6> repeats{};
  if(const auto* text=std::getenv("KYTY_GPU_BENCH_REPEATS"))
    Require("GenericGpuMicrobench","same baseline repeats setup",
      std::sscanf(text,"%u,%u,%u,%u,%u,%u",&repeats[0],&repeats[1],&repeats[2],
                  &repeats[3],&repeats[4],&repeats[5])==6,
      "expected six repeat counts for wave32channels1/3/4 then wave64channels1/3/4");
  uint32_t index=0;
  for(uint32_t wave : {32u,64u}) {
    GenericCompilation shader(32,wave);
    FetchGpuBuffer input(graphics,GenericPrefixBytes+512u,vk::BufferUsageFlagBits::eStorageBuffer);
    FetchGpuBuffer output(graphics,wave*512u+16u,vk::BufferUsageFlagBits::eStorageBuffer);
    GenericGpuPipeline pipeline(vulkan,shader,input,output);
    for(uint32_t channels : {1u,3u,4u})pipeline.BenchmarkGenericLoads(channels,repeats[index++]);
  }
}
} // namespace
} // namespace Libs::Graphics
