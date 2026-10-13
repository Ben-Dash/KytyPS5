// Include after FusedTiledUploadCostTests.inc; apply the tiny optional timing hook first.
// Existing public cache upload+native exact byte fixture; no clean-cache no-op samples.
namespace Libs::Graphics {
namespace {
void CheckFusedTiledUploadGpuMicrobench(VulkanHarness& vulkan) {
  constexpr const char* name="FusedTiledUploadGpuMicrobench";
  auto& graphics=vulkan.RuntimeContext();
  const auto families=graphics.physical_device.getQueueFamilyProperties();
  Require(name,"real upload timestamp capability setup",graphics.queue_family<families.size() &&
      families[graphics.queue_family].timestampValidBits>0 &&
      graphics.physical_device_properties.limits.timestampPeriod>0,
      "actual queue timestamps unavailable; not a passing performance screen");
  vk::QueryPoolCreateInfo query{};
  query.queryType=vk::QueryType::eTimestamp; query.queryCount=2;
  vk::QueryPool timestamps=nullptr;
  RequireVulkanSuccess(vulkan.Device().createQueryPool(&query,nullptr,&timestamps),
                       "fused upload timestamp pool");
  for(uint32_t bpe : {4u,8u}) {
    std::vector<double> samples;
    std::vector<uint32_t> actual_copies;
    for(uint32_t sample=0;sample<9;++sample) {
      // Every call maps a freshly seeded bounded input, executes the PUBLIC cache
      // upload, checks ALL exact native bytes/canaries and waits before reuse.
      // First two samples warm shader/native image allocation;7remain identical.
      const auto measured=RunFusedTiledUploadCase(vulkan,bpe,timestamps);
      Require(name,"same real upload workload setup",measured.gpu_ns>0 &&
          measured.gpu_ns<25000000.0 && measured.intermediate_copies<=1,
          "single actual upload exceeds25ms or the setup emitted extra copies; stop");
      if(sample>=2) {
        samples.push_back(measured.gpu_ns);
        actual_copies.push_back(measured.intermediate_copies);
        std::printf("[fused-upload-gpu-sample] bpe=%u width=128 height=128 sample=%u gpu_ns=%.1f actualCopies=%u golden=PASS\n",
            bpe,sample-2,measured.gpu_ns,measured.intermediate_copies);
      }
    }
    Require(name,"unchanged sampled upload route",std::all_of(actual_copies.begin(),actual_copies.end(),
        [&](uint32_t value){return value==actual_copies.front();}),
        "different native routes/work inside one fixed comparison; not a speed result");
    std::sort(samples.begin(),samples.end());
    std::printf("[fused-upload-gpu-bench] bpe=%u width=128 height=128 bytes_live=%u samples=7 median_ns=%.1f min_ns=%.1f max_ns=%.1f actualCopies=%u golden=PASS\n",
        bpe,128u*128u*bpe,samples[3],samples.front(),samples.back(),actual_copies.front());
  }
  vulkan.Device().destroyQueryPool(timestamps,nullptr);
}
} // namespace
} // namespace Libs::Graphics
