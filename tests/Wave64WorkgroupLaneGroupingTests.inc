// Included inside ShaderRecompilerComputeTests.cpp's existing test namespace,
// after RunCase and ISA helpers. Entirely synthetic guest code, no loops/gates.
// The CPU oracle defines guest64 waves by logical local IDs, independently of
// the recompiler's native32 virtual-half mapping and shuffle implementation.
TestCase Wave64WorkgroupLaneGrouping(u32 threads, bool sparse_exec) {
  using O = ShaderOpcode;
  constexpr u32 guard_words = 4u, sentinel = 0xdeadbeefu;
  Require("Wave64WorkgroupLaneGrouping", "bounded full guest waves",
          threads == 64u || threads == 128u || threads == 512u,
          "fixture supports only finite64/128/512 logical invocations; not a semantic RED");
  TestCase test;
  if (threads == 64u)
    test.name = sparse_exec ? "Wave64Grouping64Sparse" : "Wave64Grouping64Full";
  else if (threads == 128u)
    test.name = sparse_exec ? "Wave64Grouping128Sparse" : "Wave64Grouping128Full";
  else
    test.name = sparse_exec ? "Wave64Grouping512Sparse" : "Wave64Grouping512Full";
  test.initial.assign(guard_words + threads * 4u + guard_words, sentinel);
  test.expected = test.initial;
  for (u32 id = 0u; id < threads; ++id) {
    const u32 wave_base = (id / 64u) * 64u;
    test.expected[guard_words + id] = id;
    test.expected[guard_words + threads + id] = wave_base + 31u;
    test.expected[guard_words + threads * 2u + id] = wave_base + 63u;
    test.expected[guard_words + threads * 3u + id] = wave_base + (sparse_exec ? 5u : 0u);
  }
  auto& code = test.code;
  if (sparse_exec) {
    // v0 was initialized for all logical invocations at entry. Preserve the
    // real initial mask, enable31/63 in every guest wave, first active lane5.
    code.push_back(EncodeSop1(0x04, 20u, 126u)); // S_MOV_B64 s[20:21],EXEC
    AppendSMovLiteral(&code, 126u, 0xaaaaaaa0u);
    AppendSMovLiteral(&code, 127u, 0xaaaaaaaau);
  }
  AppendVop3(&code, 0x360u, 8u, Vgpr(0u), InlineU32(31u));
  AppendVop3(&code, 0x360u, 9u, Vgpr(0u), InlineU32(63u));
  code.push_back(EncodeVop1(0x02u, 10u, Vgpr(0u))); // V_READFIRSTLANE_B32 s10,v0
  if (sparse_exec)
    code.push_back(EncodeSop1(0x04, 126u, 20u)); // Restore actual EXEC BEFORE any store.
  AppendStoreVgprAtLaneDwordOffset(&code, 0u, 0u, guard_words);
  AppendStoreSgprAtLaneDwordOffset(&code, 8u, 0u, guard_words + threads);
  AppendStoreSgprAtLaneDwordOffset(&code, 9u, 0u, guard_words + threads * 2u);
  AppendStoreSgprAtLaneDwordOffset(&code, 10u, 0u, guard_words + threads * 3u);
  AppendEnd(&code);
  test.opcodes = {O::V_READLANE_B32, O::V_READFIRSTLANE_B32, O::V_MOV_B32,
                  O::V_ADD_NC_U32, O::V_LSHLREV_B32, O::BUFFER_STORE_DWORD, O::S_ENDPGM};
  if (sparse_exec) test.opcodes.push_back(O::S_MOV_B32);
  if (sparse_exec) test.opcodes.push_back(O::S_MOV_B64);
  test.required_spirv = {"OpGroupNonUniformShuffle", "OpGroupNonUniformBallot"};
  test.forbidden_spirv = {"OpLoopMerge"};
  test.compute_info.wave_size = 64u;
  test.compute_info.threads_num[0] = threads;
  test.compute_info.threads_num[1] = test.compute_info.threads_num[2] = 1u;
  test.compute_info.dispatch_threads_num[0] = threads;
  test.compute_info.dispatch_threads_num[1] = test.compute_info.dispatch_threads_num[2] = 1u;
  test.compute_info.thread_ids_num = 1;
  test.has_compute_info = true;
  return test;
}

void CheckWave64WorkgroupLaneGrouping(VulkanHarness& vulkan) {
  Require("Wave64WorkgroupLaneGrouping", "actual native32 subgroup",
          vulkan.SubgroupSize() == 32u,
          "regression requires actual guest64/native32 halves; native64 is separate coverage");
  for (const bool sparse_exec : {false, true})
    for (const u32 threads : {64u, 128u, 512u})
      RunCase(&vulkan, Wave64WorkgroupLaneGrouping(threads, sparse_exec));
}
