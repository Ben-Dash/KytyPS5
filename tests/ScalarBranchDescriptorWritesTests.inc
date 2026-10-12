// Uncompiled proposal for root integration into ShaderRecompilerComputeTests.cpp.
// Include after RunCase/CompileCase and VulkanHarness are defined; expose an exact
// CLI mode calling RunScalarBranchDescriptorWrites(&vulkan). No game bytes.
// This proves host descriptor choice + native direct-binding SPIR-V GPU writes.
// It does not exercise production BufferCache guest-address-to-backing resolution.
void RunScalarBranchDescriptorWrites(VulkanHarness *vulkan) {
  using O = ShaderOpcode;
  for (const bool carry : {false, true}) {
    for (const u32 flag : {0u, 1u, 0x80000000u}) {
      TestCase test;
      test.name = "ScalarBranchDescriptorWrites";
      test.has_user_data = true;
      // Immutable descriptors and scalar selector; one arm crosses low DWORD 0.
      test.user_data[0] = carry ? 0xffffffe0u : 0x1000u;
      test.user_data[1] = carry ? 0u : 1u;
      test.user_data[2] = 16u;
      test.user_data[3] = 0x16204u;
      test.user_data[4] = 0x2000u;
      test.user_data[5] = 2u;
      test.user_data[6] = 16u;
      test.user_data[7] = 0x16204u;
      test.user_data[8] = flag;
      test.user_data[9] = 0x40u;
      test.user_data[16] = 0x3000u;
      test.user_data[17] = 3u | (4u << 16u);
      test.user_data[18] = 16u;
      test.user_data[19] = 0x16204u;
      test.compute_info.threads_num[0] = 64;
      test.compute_info.threads_num[1] = 1;
      test.compute_info.threads_num[2] = 1;
      test.compute_info.thread_ids_num = 1;
      test.compute_info.wave_size = 64;
      test.has_compute_info = true;

      auto &code = test.code;
      // One active lane: side effects must occur exactly once.
      code.push_back(EncodeSop1(0x04, 126, InlineU32(1)));
      code.push_back(EncodeSopc(0x06, 8, InlineU32(0)));
      const auto second_branch = code.size();
      code.push_back(0);
      const auto arm = [&](u32 base) {
        code.push_back(EncodeSop2(0x00, 20, base, 9));
        code.push_back(EncodeSop2(0x04, 21, base + 1u, InlineU32(0)));
        code.push_back(EncodeSMovB32(22, base + 2u));
        code.push_back(EncodeSMovB32(23, base + 3u));
      };
      arm(0);
      const auto merge_branch = code.size();
      code.push_back(0);
      const auto second = code.size();
      arm(4);
      const auto merge = code.size();
      code[second_branch] = EncodeSopp(0x04, second - second_branch - 1u);
      code[merge_branch] = EncodeSopp(0x02, merge - merge_branch - 1u);
      // Nested expression over the merged high-word Phi, as S_BITSET1 would do.
      code.push_back(EncodeSop2(0x10, 21, 21, 255));
      code.push_back(0x40000u);
      AppendVMovU32(&code, 1, 0x71335599u);
      AppendVMovU32(&code, 30, 4u);
      code.push_back(EncodeMubuf0(0x1c));
      code.push_back(EncodeMubuf1(1, 5, 30));
      AppendVMovU32(&code, 1, 1u);
      code.push_back(EncodeMubuf0(0x32));
      code.push_back(EncodeMubuf1(1, 4, 30));
      AppendEnd(&code);
      test.opcodes = {O::S_MOV_B64, O::S_CMP_EQ_U32, O::S_CBRANCH_SCC0,
                      O::S_BRANCH, O::S_ADD_U32, O::S_ADDC_U32,
                      O::S_MOV_B32, O::S_OR_B32, O::V_MOV_B32,
                      O::BUFFER_STORE_DWORD, O::BUFFER_ATOMIC_ADD, O::S_ENDPGM};
      test.initial.assign(80, 0xdeadbeefu);
      test.initial[49] = 0;
      test.expected = test.initial;
      const bool first = flag == 0u;
      test.expected[first ? 17u : 33u] = 0x71335599u;
      test.expected[49] = 1u;
      test.storage_buffer_offsets = {first ? 64u : 128u, 192u};
      test.storage_buffer_range_bytes = 64u;
      test.expected_buffer_resources = std::vector<u32>{0, 1};
      test.required_spirv = {"OpAtomicIAdd"};
      test.forbidden_spirv = {"OpConvertUToPtr", "PhysicalStorageBuffer"};

      auto compiled = CompileCase(test, vulkan->SubgroupSize());
      const u32 base = first ? 0u : 4u;
      const uint64_t sum = uint64_t(test.user_data[base]) + test.user_data[9];
      const std::array<u32, 4> expected_descriptor{
          u32(sum), (test.user_data[base + 1u] + u32(sum >> 32u)) | 0x40000u,
          test.user_data[base + 2u], test.user_data[base + 3u]};
      Require(test.name, "host scalar descriptor selection",
              !compiled.program.info.uses_dma && compiled.resources.buffers.size() == 2u &&
                  std::equal(expected_descriptor.begin(), expected_descriptor.end(),
                             compiled.resources.buffers[0].dwords.begin()) &&
                  std::equal(test.user_data.begin() + 16, test.user_data.begin() + 20,
                             compiled.resources.buffers[1].dwords.begin()),
              "selected address/carry or independent neighbor descriptor mismatch");
      auto buffer = vulkan->CreateStorageBuffer(test.name, test.initial, test.initial.size());
      vulkan->Dispatch(test, compiled, buffer);
      auto actual = vulkan->ReadBuffer(test.name, buffer, test.expected.size());
      vulkan->DestroyBuffer(&buffer);
      CompareWords(test, "selected store, atomic once and complete canaries",
                   test.expected, actual);
      std::printf("[compute] ScalarBranchDescriptorWrites flag=%08x carry=%u ok\n",
                  flag, carry ? 1u : 0u);
    }
  }
}
