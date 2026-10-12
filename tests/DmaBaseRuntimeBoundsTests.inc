// Renderer neighboring contracts. Include beside FirstUseDmaBaseTests.inc.
namespace Libs::Graphics {
namespace {
void CheckDmaBaseRuntimeBounds(VulkanHarness& vulkan) {
  const char* name="DmaBaseRuntimeBounds";
  constexpr uint64_t unused=FetchGpuGuest::Base+0xd000u;
  constexpr uint64_t indexed=FetchGpuGuest::Base+0x3000u;
  constexpr uint64_t outside=FetchGpuGuest::Base+FetchGpuGuest::Bytes;
  constexpr uint64_t unmatched=FetchGpuGuest::Base+0x8000u;
  {
    FetchGpuGuest guest;
    auto& context=vulkan.RuntimeRenderer();
    auto& scheduler=context.GetCommandScheduler();
    auto& cache=context.GetBufferCache();
    context.MapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
    HW::Context registers{}; HW::UserConfig user_config{}; HW::Shader shaders{};
    scheduler.Begin(registers,user_config,shaders);
    FetchGpuIR::CompiledShaderInfo program{};
    program.stage=ShaderType::Compute; program.user_data_base=8u; program.user_data_count=4u;
    program.info.uses_dma=true;
    FetchGpuIR::ResourceSnapshot resources;
    resources.user_data={static_cast<uint32_t>(unused),static_cast<uint32_t>(unused>>32u),
      static_cast<uint32_t>(indexed),static_cast<uint32_t>(indexed>>32u)};
    ShaderStageRuntime runtime{&program,&resources};
    Require(name,"initialabsence",cache.PeekPage(unused).first==nullptr && cache.PeekPage(indexed).first==nullptr,
      "setup did not start uncached; not an indexing/boundsproof");
    program.info.dma_base_registers={{7u,UINT32_MAX},{11u,UINT32_MAX},{UINT32_MAX,UINT32_MAX}};
    context.CacheDmaBases(runtime);
    Require(name,"belowbase and truncatedpair refusal",cache.PeekPage(unused).first==nullptr && cache.PeekPage(indexed).first==nullptr,
      "invalid registerindices acquired a resource");
    program.info.dma_base_registers={{8u,UINT32_MAX}};
    resources.user_data[1]=static_cast<uint32_t>(unused>>32u)|0x10000u;
    context.CacheDmaBases(runtime);
    Require(name,"rawhighbits are not silentlymasked",cache.PeekPage(unused).first==nullptr,
      "an invalid raw48-bit+ address became a different validaddress");
    resources.user_data[0]=resources.user_data[1]=0u;
    context.CacheDmaBases(runtime);
    resources.user_data[0]=static_cast<uint32_t>(outside);
    resources.user_data[1]=static_cast<uint32_t>(outside>>32u);
    context.CacheDmaBases(runtime);
    Require(name,"null and unmappedwindows stayunowned",cache.PeekPage(outside).first==nullptr &&
      cache.PeekPage(unused).first==nullptr && cache.PeekPage(indexed).first==nullptr,
      "null or unmappedaddresses acquired a resource");
    // Exactaddress-space end is valid; crossingby one and unrelatedhigh regions
    // are invalid. All three are unmapped and mustnever reach FindBuffer.
    static_assert(GuestRange{LOWER_ADDRESS_SIZE-BufferCache::CACHING_PAGESIZE,BufferCache::CACHING_PAGESIZE}.Valid());
    static_assert(!GuestRange{LOWER_ADDRESS_SIZE-BufferCache::CACHING_PAGESIZE+1u,BufferCache::CACHING_PAGESIZE}.Valid());
    for(const auto base:{LOWER_ADDRESS_SIZE-BufferCache::CACHING_PAGESIZE,
                         LOWER_ADDRESS_SIZE-BufferCache::CACHING_PAGESIZE+1u,uint64_t{1u}<<48u}) {
      resources.user_data[0]=static_cast<uint32_t>(base);
      resources.user_data[1]=static_cast<uint32_t>(base>>32u);
      context.CacheDmaBases(runtime);
    }
    Require(name,"addressspace boundaries stayunowned",cache.PeekPage(unused).first==nullptr && cache.PeekPage(indexed).first==nullptr,
      "an unmapped or overflowingwindow acquired unrelatedmemory");
    // SGPR10 maps to snapshotindex2 because user_data_base is8. The unrelated
    // index0 pair staysuncached; only mappedintersections of this16KiBwindow enter.
    resources.user_data[0]=static_cast<uint32_t>(unused);
    resources.user_data[1]=static_cast<uint32_t>(unused>>32u);
    program.info.dma_base_registers={{10u,UINT32_MAX}};
    context.CacheDmaBases(runtime);
    Require(name,"nonzero userdatabase indexing",cache.PeekPage(indexed).first!=nullptr &&
      cache.PeekPage(unused).first==nullptr && cache.PeekPage(unmatched).first==nullptr,
      "preparation ignored user_data_base or scanned unrelateduserdata");
    // Actualdescriptor metadata selects high16. Its window crosses themapped
    // allocationend; preparation clips it rather than reading backingoutside.
    resources.user_data[1]=static_cast<uint32_t>(unused>>32u)|0x04000000u;
    program.info.dma_base_registers={{8u,0xffffu}};
    context.CacheDmaBases(runtime);
    Require(name,"descriptorhigh16 and mappedend clipping",cache.PeekPage(unused).first!=nullptr &&
      cache.PeekPage(outside).first==nullptr && cache.PeekPage(unmatched).first==nullptr,
      "descriptorflags changed theaddress, window crossed unmappedbytes, or wholememory was acquired");
    context.PrepareBda();
    const auto tick=scheduler.CurrentTick();
    scheduler.Finish(); scheduler.WaitPriorityOperations(tick);
    context.UnmapMemory(FetchGpuGuest::Base,FetchGpuGuest::Bytes);
  }
  std::printf("[dma-runtime-bounds] registerindex/raw48/null/unmapped/spaceend/descriptorhigh16/clippedwindow PASS\n");
}
} // namespace
} // namespace Libs::Graphics
