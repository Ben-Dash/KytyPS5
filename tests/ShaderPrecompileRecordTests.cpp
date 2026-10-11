#include "graphics/host_gpu/renderer/pipeline/shaderPrecompile.h"

#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <xxhash.h>

using namespace Libs::Graphics;
using namespace Libs::Graphics::ShaderPrecompile;

namespace {

void Check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "ShaderPrecompileRecordTests: %s\n", message);
    std::abort();
  }
}

struct TestDirectory {
  std::filesystem::path path;
  TestDirectory() {
    path = std::filesystem::temp_directory_path() /
           ("KytyShaderRecords-" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
    Check(std::filesystem::create_directory(path),
          "create unique test directory");
  }
  ~TestDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

PermutationRecord MakeRecord(ShaderType stage) {
  PermutationRecord r;
  r.stage = stage;
  r.hash = 0x0123456789abcdefull;
  r.code = {
      0xbf810000u}; // s_endpgm; codec tests do not require a Vulkan device.
  r.back_code = {0x80000001u, 0xbf810000u};
  r.user_data_count = 19;
  r.user_data_base = 8;
  r.push_data_start_dword = 7;
  r.specialization.buffers.push_back({.packed_stride = 16u});
  r.specialization.buffers.push_back({.packed_stride = 4u,
                                      .indirect_root = 0u,
                                      .indirect_mapping_offset = 24u,
                                      .indirect_search_iterations = 3u});
  r.specialization.images.push_back({
      .numeric_class = Prospero::TextureNumericClass::Float,
      .dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D,
      .mip_count = 4u,
      .cube = true,
      .fmask = true,
  });
  if (stage == ShaderType::Pixel) {
    ShaderPixelInputInfo i;
    i.input_num = 2;
    i.interpolator_settings[0] = 0x01234567;
    i.interpolator_settings[1] = 0x76543210;
    i.ps_perspective_center_vgpr = 2;
    i.ps_perspective_sample_vgpr = 8;
    i.ps_perspective_centroid_vgpr = 4;
    i.ps_system_input_base = 6;
    i.target_shader_mask = 0x000f000fu;
    i.parameter_mode = ShaderPixelParameterMode::LastVertex;
    i.dual_source_blending = true;
    i.ps_sample_shading = true;
    i.ps_execute_on_noop = true;
    i.target_output_mode[3] = 7;
    i.target_export_mapping[3].packed = 0xc6;
    r.info = i;
  } else if (stage == ShaderType::Compute) {
    ShaderComputeInputInfo i;
    i.threads_num[0] = 8;
    i.threads_num[1] = 4;
    i.threads_num[2] = 2;
    i.host_subgroup_size = 32;
    i.thread_ids_num = 3;
    i.workgroup_register = 12;
    i.lds_size_dwords = 1024;
    i.scratch_size_dwords = 7;
    i.group_id[2] = true;
    i.tg_size_en = true;
    i.dispatch_threads_num[0] = 12345;
    i.float_mode = 0x30;
    i.lds_storage = true;
    r.info = i;
  } else {
    ShaderVertexInputInfo i{};
    i.logical_stage = stage;
    i.resources_num = 1;
    i.resources[0].fields[0] = 0x12345678u;
    i.resources[0].fields[1] = 0x0010abcdu;
    i.resources[0].fields[2] = 4096;
    i.resources[0].fields[3] = DstSel(4, 5, 6, 7);
    i.resources_dst[0] = {.register_start = 5,
                          .registers_num = 4,
                          .attr_id = 7,
                          .fetch_index = 1,
                          .buffer_index = 1};
    i.buffers_num = 2;
    i.buffers[0].stride = 8;
    i.buffers[1].addr = 0x765432100000ull;
    i.buffers[1].stride = 16;
    i.buffers[1].num_records = 4096;
    i.buffers[1].fetch_index = 1;
    i.fetch_external = true;
    i.fetch_embedded = true;
    i.fetch_attrib_reg = 8;
    i.fetch_buffer_reg = 10;
    i.mesh.threads_num[0] = 32;
    i.mesh.threads_num[1] = 1;
    i.mesh.threads_num[2] = 1;
    i.mesh.wave_size = 32;
    i.mesh.host_subgroup_size = 32;
    i.mesh.max_vertices = 64;
    i.mesh.max_primitives = 32;
    i.mesh.fast_launch = true;
    i.tess = {.input_control_points = 3,
              .output_control_points = 4,
              .ls_stride = 16,
              .hs_stride = 32,
              .domain = 2,
              .partitioning = 1,
              .output_topology = 3};
    i.clip_space.enabled = true;
    i.clip_space.scale[0] = std::bit_cast<float>(0x7fc12345u);
    i.clip_space.scale[1] = -0.0f;
    i.clip_space.offset[0] = 0.125f;
    i.clip_space.half_extent[1] = 8192.0f;
    if (stage == ShaderType::Mesh)
      r.wave_size = i.mesh.wave_size;
    r.info = i;
  }
  return r;
}

void TestAllStagesRoundTrip() {
  for (auto stage :
       {ShaderType::Vertex, ShaderType::Mesh, ShaderType::Local,
        ShaderType::TessellationControl, ShaderType::TessellationEvaluation,
        ShaderType::Pixel, ShaderType::Compute}) {
    auto source = MakeRecord(stage);
    std::vector<uint8_t> encoded;
    Check(Encode(source, encoded), "encode each supported stage");
    PermutationRecord result;
    Check(Decode(encoded, result), "decode each supported stage");
    Check(result.stage == source.stage && result.hash == source.hash &&
              result.code == source.code &&
              result.back_code == source.back_code &&
              result.user_data_count == 19 && result.user_data_base == 8 &&
              result.push_data_start_dword == 7 &&
              result.specialization == source.specialization,
          "code, SGPR count, push layout and resource specialization survive");
    std::vector<uint8_t> canonical;
    Check(Encode(result, canonical) && canonical == encoded,
          "decoded records have canonical bytes");
    std::visit(
        [](const auto &i) {
          Check(i.stage.program == nullptr && i.stage.resources == nullptr,
                "runtime stage pointers stay empty");
        },
        result.info);
    if (const auto *i = std::get_if<ShaderVertexInputInfo>(&result.info)) {
      Check(i->logical_stage == stage && i->mesh.host_subgroup_size == 32 &&
                i->mesh.threads_num[2] == 1 && i->mesh.wave_size == 32 &&
                i->mesh.max_vertices == 64 && i->mesh.fast_launch &&
                i->tess.output_control_points == 4 && i->tess.hs_stride == 32 &&
                i->tess.output_topology == 3,
            "mesh and tessellation compilation metadata survives");
      Check(i->resources[0].Base48() == 0 &&
                i->resources[0].NumRecords() == 0 && i->buffers[1].addr == 0 &&
                i->buffers[1].num_records == 0 && i->buffers[1].stride == 16 &&
                i->resources[0].Stride() == 16 &&
                i->resources_dst[0].attr_id == 7 &&
                i->resources_dst[0].buffer_index == 1,
            "vertex shape survives without serializing runtime addresses");
      Check(std::bit_cast<uint32_t>(i->clip_space.scale[0]) == 0x7fc12345u &&
                std::bit_cast<uint32_t>(i->clip_space.scale[1]) == 0x80000000u,
            "floating metadata preserves NaN payloads and negative zero");
    } else if (const auto *i =
                   std::get_if<ShaderPixelInputInfo>(&result.info)) {
      Check(i->ps_perspective_sample_vgpr == 8,
            "pixel sample interpolation VGPR survives the journal round trip");
      Check(i->target_shader_mask == 0x000f000fu,
            "pixel target shader mask survives the journal round trip");
      Check(i->parameter_mode == ShaderPixelParameterMode::LastVertex,
            "pixel provoking vertex mode survives the journal round trip");
      Check(i->ps_perspective_centroid_vgpr == 4 && i->dual_source_blending &&
                i->target_export_mapping[3].packed == 0xc6 &&
                i->ps_sample_shading &&
                i->interpolator_settings[1] == 0x76543210,
            "centroid, dual-source and export mappings survive");
    } else {
      const auto &compute = std::get<ShaderComputeInputInfo>(result.info);
      Check(
          compute.host_subgroup_size == 32 && compute.threads_num[2] == 2 &&
              compute.group_id[2] && compute.lds_size_dwords == 1024 &&
              compute.tg_size_en && compute.dispatch_threads_num[0] == 0 &&
              compute.float_mode == 0x30 && compute.lds_storage,
          "compute module shape survives without runtime dispatch dimensions");
    }
  }
}

void TestRuntimeInputsDoNotChangeRecords() {
  auto a = MakeRecord(ShaderType::Vertex);
  auto b = a;
  auto &info = std::get<ShaderVertexInputInfo>(b.info);
  info.stage.program =
      reinterpret_cast<const ShaderRecompiler::IR::CompiledShaderInfo *>(
          uintptr_t{0x1234});
  info.stage.resources =
      reinterpret_cast<const ShaderRecompiler::IR::ResourceSnapshot *>(
          uintptr_t{0x5678});
  info.resources[0].UpdateAddress48(0xabcdef012340ull);
  info.resources[0].fields[2] = 42;
  info.buffers[0].addr = 0xabcdef000000ull;
  info.buffers[0].num_records = 42;
  std::vector<uint8_t> first, second;
  Check(Encode(a, first) && Encode(b, second) && first == second,
        "runtime pointers, addresses and record counts cannot contaminate the "
        "journal");
  ShaderParams params;
  params.code = a.code;
  params.user_data.fill(0xdeadbeef);
  params.user_data_count = 19;
  ShaderRecompiler::CompileOptions options;
  options.stage = ShaderType::Vertex;
  options.user_data = std::span(params.user_data).first(19);
  auto captured = Capture(params, options, a.specialization, 0, info);
  Check(captured.user_data_count == 19 &&
            std::get<ShaderVertexInputInfo>(captured.info).stage.program ==
                nullptr,
        "capture keeps actual SGPR count rather than the 40-element storage "
        "capacity");
}

void SetWord(std::vector<uint8_t> &bytes, size_t offset, uint32_t value) {
  for (unsigned n = 0; n != 4; ++n)
    bytes.at(offset + n) = static_cast<uint8_t>(value >> (n * 8u));
}

void TestInvalidRecords() {
  auto good = MakeRecord(ShaderType::Compute);
  std::vector<uint8_t> bytes;
  Check(Encode(good, bytes), "make valid record for negative cases");
  for (size_t size = 0; size < bytes.size(); ++size) {
    PermutationRecord untouched;
    untouched.hash = 99;
    Check(!Decode(std::span(bytes).first(size), untouched) &&
              untouched.hash == 99,
          "every truncated prefix is rejected without modifying the output");
  }
  PermutationRecord output;
  auto invalid = bytes;
  SetWord(invalid, 0, 0xffffffffu);
  Check(!Decode(invalid, output), "unknown stage is rejected");
  invalid = bytes;
  SetWord(invalid, 20, 41u);
  Check(!Decode(invalid, output), "excessive user SGPR count is rejected");
  invalid = bytes;
  SetWord(invalid, 28, 0xffffffffu);
  Check(!Decode(invalid, output),
        "excessive code length is rejected before allocation");
  invalid = bytes;
  SetWord(invalid, invalid.size() - 4, 2u);
  Check(!Decode(invalid, output), "noncanonical bool is rejected");
  invalid = bytes;
  invalid.push_back(0);
  Check(!Decode(invalid, output), "unconsumed trailing bytes are rejected");
  auto bad_root = good;
  bad_root.specialization.buffers[1].indirect_root = 2u;
  Check(!Encode(bad_root, bytes), "indirect buffer roots must name a buffer");
  good.info = ShaderPixelInputInfo{};
  Check(!Encode(good, bytes), "stage and input metadata must match");
  good = MakeRecord(ShaderType::Vertex);
  std::get<ShaderVertexInputInfo>(good.info).resources_num = 33;
  Check(!Encode(good, bytes),
        "live record limits are checked before array access");
}

PermutationRecord MakeLegacyPixelRecord() {
  auto record = MakeRecord(ShaderType::Pixel);
  auto &pixel = std::get<ShaderPixelInputInfo>(record.info);
  pixel.ps_perspective_sample_vgpr = UINT32_MAX;
  pixel.target_shader_mask = UINT32_MAX;
  pixel.parameter_mode = ShaderPixelParameterMode::FirstVertex;
  return record;
}

void TestPixelFieldBoundariesAndMalformedTail() {
  for (uint32_t sample : std::array<uint32_t, 3>{0u, 254u, UINT32_MAX}) {
    auto source = MakeLegacyPixelRecord();
    std::get<ShaderPixelInputInfo>(source.info).ps_perspective_sample_vgpr = sample;
    std::vector<uint8_t> bytes;
    PermutationRecord decoded;
    Check(Encode(source, bytes) && Decode(bytes, decoded),
          "pixel sample VGPR boundaries encode and decode");
    const auto &pixel = std::get<ShaderPixelInputInfo>(decoded.info);
    Check(pixel.ps_perspective_sample_vgpr == sample &&
              pixel.target_shader_mask == UINT32_MAX &&
              pixel.parameter_mode == ShaderPixelParameterMode::FirstVertex,
          "sample boundaries and default mask/FirstVertex survive");
  }
  for (uint32_t mask : std::array<uint32_t, 3>{0u, 0x000f000fu, UINT32_MAX}) {
    auto source = MakeRecord(ShaderType::Pixel);
    std::get<ShaderPixelInputInfo>(source.info).target_shader_mask = mask;
    std::vector<uint8_t> bytes;
    PermutationRecord decoded;
    Check(Encode(source, bytes) && Decode(bytes, decoded) &&
              std::get<ShaderPixelInputInfo>(decoded.info).target_shader_mask == mask,
          "empty, sparse and full target masks survive");
  }
  for (uint32_t sample : std::array<uint32_t, 2>{255u, UINT32_MAX - 1u}) {
    auto invalid = MakeRecord(ShaderType::Pixel);
    std::get<ShaderPixelInputInfo>(invalid.info).ps_perspective_sample_vgpr = sample;
    std::vector<uint8_t> bytes{0x7fu};
    Check(!Encode(invalid, bytes) && bytes.empty(),
          "invalid live sample VGPR is rejected without encoded output");
  }
  auto invalid_mode = MakeRecord(ShaderType::Pixel);
  std::get<ShaderPixelInputInfo>(invalid_mode.info).parameter_mode =
      static_cast<ShaderPixelParameterMode>(2u);
  std::vector<uint8_t> bytes;
  Check(!Encode(invalid_mode, bytes), "unknown live provoking vertex mode is rejected");
  Check(Encode(MakeRecord(ShaderType::Pixel), bytes), "make complete pixel tail");
  for (size_t remove : std::array<size_t, 4>{1u, 4u, 8u, 12u}) {
    PermutationRecord output;
    output.hash = 99;
    Check(!Decode(std::span(bytes).first(bytes.size() - remove), output) && output.hash == 99,
          "missing pixel tail fields are rejected without changing output");
  }
  for (size_t offset : std::array<size_t, 2>{bytes.size() - 12u, bytes.size() - 4u}) {
    auto malformed = bytes;
    SetWord(malformed, offset, offset == bytes.size() - 12u ? 255u : 2u);
    PermutationRecord output;
    output.hash = 99;
    Check(!Decode(malformed, output) && output.hash == 99,
          "malformed encoded sample VGPR or provoking mode is rejected");
  }
}

void TestExplicitLegacyCodec() {
  const auto source = MakeLegacyPixelRecord();
  std::vector<uint8_t> legacy, current;
  Check(Encode(source, legacy, LegacyRecordVersion) && Encode(source, current),
        "legacy defaults encode only with an explicit legacy version");
  Check(current.size() == legacy.size() + 12u,
        "complete pixel records include all three additional scalar fields");
  PermutationRecord decoded;
  Check(Decode(legacy, decoded, LegacyRecordVersion), "explicit v8 decode remains available");
  const auto &pixel = std::get<ShaderPixelInputInfo>(decoded.info);
  Check(pixel.ps_perspective_sample_vgpr == UINT32_MAX &&
            pixel.target_shader_mask == UINT32_MAX &&
            pixel.parameter_mode == ShaderPixelParameterMode::FirstVertex,
        "legacy decode explicitly reconstructs omitted pixel defaults");
  std::vector<uint8_t> canonical;
  Check(Encode(decoded, canonical, LegacyRecordVersion) && canonical == legacy,
        "legacy default reconstruction preserves canonical v8 body bytes");
  decoded.hash = 99;
  Check(!Decode(legacy, decoded) && decoded.hash == 99,
        "default current decoder never treats a legacy pixel body as complete");
  Check(!Decode(current, decoded, LegacyRecordVersion) && decoded.hash == 99,
        "legacy decoder rejects a complete pixel body with an unconsumed tail");
  for (unsigned field = 0; field != 3; ++field) {
    auto nondefault = source;
    auto &info = std::get<ShaderPixelInputInfo>(nondefault.info);
    if (field == 0) info.ps_perspective_sample_vgpr = 0u;
    if (field == 1) info.target_shader_mask = 0u;
    if (field == 2) info.parameter_mode = ShaderPixelParameterMode::LastVertex;
    canonical.assign(1u, 0x7fu);
    Check(!Encode(nondefault, canonical, LegacyRecordVersion) && canonical.empty(),
          "legacy encoding refuses each known nondefault omitted field");
  }
  for (uint32_t version : std::array<uint32_t, 4>{0u, 7u, 10u, UINT32_MAX}) {
    canonical.assign(1u, 0x7fu);
    Check(!Encode(source, canonical, version) && canonical.empty() &&
              !Decode(current, decoded, version) && decoded.hash == 99,
          "unsupported explicit versions cannot encode or modify decoded output");
  }
}

void TestJournalVersionSeparation() {
  TestDirectory directory;
  const auto legacy_path = directory.path / "legacy-v8.shaders";
  const auto current_path = directory.path / "current-v9.shaders";
  const std::string key = "synthetic-version-key";
  std::vector<uint8_t> body;
  Check(Encode(MakeLegacyPixelRecord(), body, LegacyRecordVersion),
        "make a synthetic legacy record body");
  std::vector<uint8_t> blob{'K', 'Y', 'T', 'Y', 'S', 'H', 'D', 'R'};
  const auto word = [&](uint32_t value) {
    const auto offset = blob.size();
    blob.resize(offset + 4u);
    SetWord(blob, offset, value);
  };
  word(LegacyRecordVersion);
  word(static_cast<uint32_t>(key.size()));
  blob.insert(blob.end(), key.begin(), key.end());
  word(static_cast<uint32_t>(body.size()));
  const auto checksum = XXH3_64bits(body.data(), body.size());
  word(static_cast<uint32_t>(checksum));
  word(static_cast<uint32_t>(checksum >> 32u));
  blob.insert(blob.end(), body.begin(), body.end());
  {
    std::ofstream out(legacy_path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(blob.data()),
              static_cast<std::streamsize>(blob.size()));
    out.close();
    Check(static_cast<bool>(out), "write synthetic v8 journal");
  }
  Check(ReadJournal(legacy_path).empty(), "default offline reader does not accept legacy journals");
  std::string read_key;
  auto legacy = ReadJournal(legacy_path, &read_key, LegacyRecordVersion);
  Check(legacy.size() == 1u && read_key == key,
        "explicit offline v8 reader validates its header and checksum");
  Journal journal;
  Check(journal.Open(current_path, key).empty() && journal.IsOpen() &&
            journal.Append(MakeRecord(ShaderType::Pixel)),
        "runtime journal writes a separate current-version record");
  journal.Close();
  auto current = ReadJournal(current_path);
  Check(current.size() == 1u &&
            std::get<ShaderPixelInputInfo>(current[0].info).parameter_mode ==
                ShaderPixelParameterMode::LastVertex,
        "default reader preserves complete current journal metadata");
  Check(ReadJournal(current_path, nullptr, LegacyRecordVersion).empty(),
        "explicit legacy reader cannot relabel a current journal");
  Check(std::filesystem::file_size(legacy_path) == blob.size() &&
            ReadJournal(legacy_path, nullptr, LegacyRecordVersion).size() == 1u,
        "current journal work leaves the legacy corpus intact");
}

void TestJournalRepairAndKeys() {
  TestDirectory directory;
  const auto path =
      directory.path / std::filesystem::path(u8"shader-\u00e9-\u65e5.shaders");
  Journal journal;
  Check(journal.Open(path, "revision:driver:opt=0").empty() && journal.IsOpen(),
        "new journal opens");
  auto first = MakeRecord(ShaderType::Compute);
  auto second = MakeRecord(ShaderType::Pixel);
  second.hash = 2;
  Check(journal.Append(first), "append first record");
  const auto valid_prefix = std::filesystem::file_size(path);
  Check(journal.Append(second), "append second record");
  journal.Close();
  std::filesystem::resize_file(path, std::filesystem::file_size(path) - 5u);
  auto records = journal.Open(path, "revision:driver:opt=0");
  Check(records.size() == 1 && records[0].hash == first.hash &&
            std::filesystem::file_size(path) == valid_prefix,
        "torn tail is removed before appending");
  second.hash = 3;
  Check(journal.Append(second), "append after repairing a torn tail");
  journal.Close();
  records = journal.Open(path, "revision:driver:opt=0");
  Check(records.size() == 2 && records[1].hash == 3 &&
            journal.RecordedCount() == 2,
        "later records cannot splice into a previously torn record");
  journal.Close();
  {
    std::fstream corrupt(path, std::ios::binary | std::ios::in | std::ios::out);
    corrupt.seekp(static_cast<std::streamoff>(valid_prefix + 12u));
    corrupt.put('\x7f');
    Check(static_cast<bool>(corrupt), "corrupt a complete record body");
  }
  records = journal.Open(path, "revision:driver:opt=0");
  Check(records.size() == 1 && std::filesystem::file_size(path) == valid_prefix,
        "checksum rejects corrupt records and retains the prior prefix");
  Check(journal.Open(path, "revision:driver:opt=1").empty() &&
            journal.RecordedCount() == 0,
        "changed effective optimization invalidates old records");
  Check(journal.Append(second), "record after key invalidation");
  Check(journal.Open(path, "other-revision:driver:opt=1").empty(),
        "compiler revision isolates replay");
  journal.Close();
  Check(!journal.Append(first), "closed journals do not record");
  const auto invalid_path = directory.path / "invalid.shaders";
  Check(journal.Open(invalid_path, std::string(4097, 'x')).empty() &&
            !journal.IsOpen() && !std::filesystem::exists(invalid_path),
        "oversized key cannot create a journal");
}

// Stride- and format-generic buffers (DynamicBufferStride, GenericBufferFormat) are journal
// values like any other: they survive the codec and validation unchanged.
void TestGenericBufferKeysRoundTrip() {
  using namespace Libs::Graphics::ShaderRecompiler;
  auto record = MakeRecord(ShaderType::Compute);
  record.specialization.buffers[0].packed_stride =
      IR::DynamicBufferStride | (1u << 14u) | (2u << 16u) | (1u << 20u);
  record.specialization.buffers[0].descriptor_format = IR::GenericBufferFormat;
  record.specialization.buffers[1].descriptor_format = Prospero::BufferFormat::k8_8UNorm;
  std::vector<uint8_t> bytes;
  Check(Encode(record, bytes), "generic buffer keys encode");
  PermutationRecord decoded;
  Check(Decode(bytes, decoded), "generic buffer keys decode");
  Check(decoded.specialization == record.specialization,
        "generic buffer keys round-trip unchanged");
}

} // namespace

int main() {
  TestAllStagesRoundTrip();
  TestRuntimeInputsDoNotChangeRecords();
  TestInvalidRecords();
  TestPixelFieldBoundariesAndMalformedTail();
  TestExplicitLegacyCodec();
  TestJournalVersionSeparation();
  TestJournalRepairAndKeys();
  TestGenericBufferKeysRoundTrip();
  std::printf("ShaderPrecompileRecordTests: all passed\n");
  return 0;
}
