// Test-only bounded loop regression in ShaderRecompilerComputeTests.cpp's existing
// namespace, after RunCase/ISA helpers. No title code, addresses or hashes.
// Public guest compiler + real GPU readback, with independent closed-form CPU
// goldens. Passing this probe does not prove a game shader or justify a fix.

TestCase Wave64StridedJobLoop1135() {
  using O = ShaderOpcode;
  constexpr u32 threads = 512u, jobs = 1135u, guard_words = 4u;
  constexpr u32 sentinel = 0xdeadbeefu, tag = 0x71000000u;
  constexpr u32 row_plane = guard_words;
  constexpr u32 count_plane = row_plane + jobs;
  constexpr u32 error_plane = count_plane + threads;

  TestCase test;
  test.name = "Wave64StridedJobLoop1135";
  test.initial.assign(error_plane + threads + guard_words, sentinel);
  test.expected = test.initial;
  for (u32 row = 0u; row < jobs; ++row)
    test.expected[row_plane + row] = tag + row;
  for (u32 local = 0u; local < threads; ++local) {
    // Independent arithmetic progression cardinality. In particular, locals
    // 0..110 execute three stores; locals111..511 execute two stores.
    test.expected[count_plane + local] =
        local < jobs ? 1u + (jobs - 1u - local) / threads : 0u;
    test.expected[error_plane + local] = 0u;
  }

  auto& code = test.code;
  code.push_back(EncodeSop1(0x04u, 20u, 126u)); // s[20:21] = actual initial EXEC
  AppendSMovLiteral(&code, 8u, jobs);
  AppendSMovLiteral(&code, 9u, threads);
  AppendSMovLiteral(&code, 10u, tag);
  code.push_back(EncodeSMovB32(11u, InlineU32(0u))); // error witness
  code.push_back(EncodeSMovB32(12u, InlineU32(0u))); // scalar check budget
  code.push_back(EncodeVop1(0x01u, 5u, Vgpr(0u))); // index = logical LocalX
  AppendVMovU32(&code, 8u, 0u);                  // per-logical-invocation stores

  const auto loop = code.size();
  // Four checks bound this guest loop even if its vector induction variable
  // fails to advance. Correct execution needs <=3 stores and a final EXECZ
  // check. No barrier, atomics, replay, shader clock or device reset is needed.
  code.push_back(EncodeSopc(0x0au, 12u, InlineU32(4u))); // S_CMP_LT_U32
  const auto budget_exit = code.size();
  code.push_back(0u);
  code.push_back(EncodeSop2(0x00u, 12u, 12u, InlineU32(1u)));
  code.push_back(EncodeVopc(0xd4u, 8u, 5u)); // V_CMPX_GT_U32 count,index
  const auto exhausted = code.size();
  code.push_back(0u);
  code.push_back(EncodeVop2(0x25u, 6u, 10u, 5u)); // marker = tag + index
  AppendStoreVgprAtLaneDwordOffset(&code, 6u, 5u, row_plane);
  code.push_back(EncodeVop2(0x25u, 8u, InlineU32(1u), 8u));
  code.push_back(EncodeVop2(0x25u, 5u, 9u, 5u)); // index += 512
  const auto backedge = code.size();
  code.push_back(0u);
  const auto bailout = code.size();
  code.push_back(EncodeSMovB32(11u, InlineU32(1u)));
  const auto done = code.size();
  code.push_back(EncodeSop1(0x04u, 126u, 20u)); // scalar restore, even EXEC=0
  AppendStoreVgprAtLaneDwordOffset(&code, 8u, 0u, count_plane);
  AppendStoreSgprAtLaneDwordOffset(&code, 11u, 0u, error_plane);
  AppendEnd(&code);

  const auto branch = [&](u32 opcode, size_t from, size_t to) {
    const auto delta = static_cast<int64_t>(to) -
                       static_cast<int64_t>(from) - 1;
    Require(test.name, "bounded branch displacement",
            delta >= -32768 && delta <= 32767,
            "synthetic branch target is outside the SOPP signed16 range");
    return EncodeSopp(opcode, static_cast<u32>(delta));
  };
  code[budget_exit] = branch(0x04u, budget_exit, bailout); // SCC0
  code[exhausted] = branch(0x08u, exhausted, done);        // EXECZ
  code[backedge] = branch(0x02u, backedge, loop);          // unconditional

  test.opcodes = {O::S_MOV_B64, O::S_MOV_B32, O::S_CMP_LT_U32,
                  O::S_CBRANCH_SCC0, O::S_ADD_U32, O::V_CMPX_GT_U32,
                  O::S_CBRANCH_EXECZ, O::S_BRANCH, O::V_MOV_B32,
                  O::V_ADD_NC_U32, O::V_LSHLREV_B32,
                  O::BUFFER_STORE_DWORD, O::S_ENDPGM};
  test.required_spirv = {"OpLoopMerge"};
  test.compute_info.wave_size = 64u;
  test.compute_info.threads_num[0] = threads;
  test.compute_info.threads_num[1] = test.compute_info.threads_num[2] = 1u;
  test.compute_info.dispatch_threads_num[0] = threads;
  test.compute_info.dispatch_threads_num[1] =
      test.compute_info.dispatch_threads_num[2] = 1u;
  test.compute_info.thread_ids_num = 1u;
  test.has_compute_info = true;
  test.dispatch_x = test.dispatch_y = test.dispatch_z = 1u;
  return test;
}

void CheckWave64StridedJobLoop(VulkanHarness& vulkan) {
  Require("Wave64StridedJobLoop1135", "actual native32 subgroup",
          vulkan.SubgroupSize() == 32u,
          "fixture requires actual guest64/native32 halves; native64 is separate coverage");
  RunCase(&vulkan, Wave64StridedJobLoop1135());
}
