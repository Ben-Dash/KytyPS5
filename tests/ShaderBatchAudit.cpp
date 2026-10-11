#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "graphics/host_gpu/renderer/pipeline/shaderPrecompile.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "xxhash.h"

#if defined(KYTY_RESOURCE_REGRESSION)
#include "TextureLayoutRegression.h"
#endif

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <spirv-tools/libspirv.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {
using Json = nlohmann::ordered_json;
using namespace Libs::Graphics;

constexpr std::array EnvironmentNames {
    "KYTY_BINDLESS_TYPED", "KYTY_NGG_VERTEX_COUNT", "KYTY_WAVE_BRANCH_FULL",
    "KYTY_NATIVE_FP16", "KYTY_TABLE_MODE", "KYTY_DEBUG_NO_SELECT_SINKING",
    "KYTY_DEBUG_WAVE_HALVES", "KYTY_DEBUG_INDEXED_SWITCH", "KYTY_DEBUG_LOOP_GUARD",
    "KYTY_DEBUG_LOOP_GUARD_PCS", "KYTY_HALF_UNIFORM", "KYTY_STRICT_SHADER_CFG",
    "KYTY_DEBUG_CS_TAP", "KYTY_DEBUG_PS_TAP", "KYTY_DEBUG_LOOP_HEAT", "KYTY_LEGACY_MAD",
    "KYTY_VTX_EXPLODE_CHECK", "KYTY_VTX_EXPLODE_LIMIT", "KYTY_BDA_FAST_DWORD",
    "KYTY_BDA_NO_WORD_MASKS", "KYTY_BDA_SINGLE_CROSS_BRANCH", "KYTY_BDA_ZERO_PAGE",
    "KYTY_INDIRECT_BOUNDS_ONCE", "KYTY_STRICT_RESOURCE_TRACKING",
    "KYTY_BUFFER_SPECIALIZE", "KYTY_SAMPLER_COMPARE_SPLIT"};

void Progress(const char* phase) {
    std::printf("KYTY_SHADER_AUDIT_PHASE %s\n", phase);
    std::fflush(stdout);
}

bool Boolean(const Json& object, const char* field) {
    if (!object.contains(field) || !object.at(field).is_boolean())
        throw std::runtime_error(std::string("explicit compiler settings require boolean ") + field);
    return object.at(field).get<bool>();
}

void RestoreSettings(const Json& settings) {
    if (settings.at("schema_version") != 1 || !settings.at("profile_label").is_string())
        throw std::runtime_error("invalid explicit journal compiler settings");
    const auto& environment = settings.at("environment");
    if (!environment.is_object() || environment.size() != EnvironmentNames.size())
        throw std::runtime_error("compiler environment must explicitly list every supported switch");
    for (const char* name : EnvironmentNames) {
        const auto& value = environment.at(name);
        if (!value.is_null() && !value.is_string()) throw std::runtime_error("compiler switch is not string/null");
        const auto text = value.is_null() ? std::string{} : value.get<std::string>();
        if (text.find('\0') != std::string::npos) throw std::runtime_error("compiler switch contains NUL");
#if defined(_WIN32)
        if (_putenv_s(name, text.c_str()) != 0) throw std::runtime_error("cannot restore compiler switch");
#else
        if (value.is_null() ? unsetenv(name) != 0 : setenv(name, text.c_str(), 1) != 0)
            throw std::runtime_error("cannot restore compiler switch");
#endif
    }
    ShaderRecompiler::SetHardwareStorageBufferBounds(Boolean(settings, "hardware_storage_buffer_bounds"));
    ShaderRecompiler::SetNativeFloat16(Boolean(settings, "native_float16"));
    ShaderRecompiler::SetTableMode(Boolean(settings, "table_mode"));
    (void)Boolean(settings, "bindless_images");
    if (ShaderRecompiler::NativeFloat16() != Boolean(settings, "native_float16"))
        throw std::runtime_error("compiler native FP16 setting conflicts with its environment gate");
}

std::vector<uint8_t> ReadBody(const std::filesystem::path& manifest_path, const Json& manifest) {
    const auto name = std::filesystem::u8path(manifest.at("code_file").get<std::string>());
    if (name.empty() || name.has_parent_path() || name.has_root_path())
        throw std::runtime_error("journal record body must be a sibling file");
    const auto path = manifest_path.parent_path() / name;
    if (std::filesystem::canonical(path).parent_path() != std::filesystem::canonical(manifest_path.parent_path()))
        throw std::runtime_error("journal record sibling resolves outside its capture directory");
    const auto bytes = std::filesystem::file_size(path);
    if (bytes == 0 || bytes > 4u * 1024u * 1024u || manifest.at("record_bytes") != bytes)
        throw std::runtime_error("invalid journal record body size");
    std::vector<uint8_t> body(bytes);
    std::ifstream stream(path, std::ios::binary);
    if (!stream.read(reinterpret_cast<char*>(body.data()), static_cast<std::streamsize>(bytes)))
        throw std::runtime_error("incomplete journal record body");
    const auto checksum_text = manifest.at("record_xxh3_64").get<std::string>();
    size_t consumed = 0;
    const auto checksum = std::stoull(checksum_text, &consumed, 16);
    if (consumed != checksum_text.size() || checksum_text.empty() || checksum_text.size() > 16u ||
        XXH3_64bits(body.data(), body.size()) != checksum)
        throw std::runtime_error("journal record checksum mismatch");
    return body;
}

int Audit(const char* filename, bool dump_ir) {
    const char* phase = "input";
    Json result{{"scope", "journal_specialized_permutation"},
                {"original_input_equivalent", false}, {"original_spirv_equivalent", false},
                {"userdata_values_captured", false}, {"userdata_values_used", false},
                {"resource_materialization_checked", false}, {"guest_memory_read", false},
                {"driver_pipeline_checked", false}, {"gpu_execution_checked", false},
                {"reconstructed_defaults", true}, {"ir_dump_requested", dump_ir}};
    try {
        Progress(phase);
        const auto manifest_path = std::filesystem::u8path(filename);
        if (std::filesystem::file_size(manifest_path) > 1024u * 1024u)
            throw std::runtime_error("journal manifest exceeds 1MiB bound");
        std::ifstream stream(manifest_path);
        const auto manifest = Json::parse(stream);
        const auto& declared_version = manifest.at("journal_version");
        if (manifest.at("schema_version") != 3 || manifest.at("kind") != "journal_record" ||
            !declared_version.is_number_integer() ||
            (declared_version != ShaderPrecompile::LegacyRecordVersion &&
             declared_version != ShaderPrecompile::CurrentRecordVersion))
            throw std::runtime_error("worker accepts only explicitly extracted v8/v9 journal records");
        const auto record_version = declared_version.get<uint32_t>();
        result["journal_version"] = record_version;
        const auto body = ReadBody(manifest_path, manifest);
        ShaderPrecompile::PermutationRecord record;
        if (!ShaderPrecompile::Decode(body, record, record_version))
            throw std::runtime_error("native declared-version codec rejected record");
        std::vector<uint8_t> canonical;
        if (!ShaderPrecompile::Encode(record, canonical, record_version) || canonical != body)
            throw std::runtime_error("native declared-version codec roundtrip changed record bytes");
        result["capture_attempt"] = manifest.at("capture_attempt");
        result["record_index"] = manifest.at("capture_attempt").at("record_index");
        result["shader_hash"] = record.hash;
        result["stage"] = static_cast<uint32_t>(record.stage);
        result["record_bytes"] = body.size();
        result["code_bytes"] = record.code.size() * sizeof(uint32_t);
        result["back_code_bytes"] = record.back_code.size() * sizeof(uint32_t);
        result["compiler_settings"] = manifest.at("compiler_settings");
        RestoreSettings(manifest.at("compiler_settings"));

        static Common::Subsystems subsystems;
        Common::InitializeThreads();
        subsystems.Initialize<Config::Lifecycle>();
        Config::ConfigOptions config;
        config.printf_direction = Config::LogDirection::Silent;
        Config::Load(config);
        subsystems.Initialize<Log::Lifecycle>();
        ShaderInit();

        // The journal compile contract uses the count only. TranslateProgram forwards size()
        // to the frontend; no runtime materialization or userdata value evaluation occurs here.
        std::array<uint32_t, 40> count_only_storage;
        count_only_storage.fill(0xcdcdcdcdu);
        if (record.user_data_count > count_only_storage.size()) throw std::runtime_error("invalid recorded userdata count");
        ShaderRecompiler::CompileOptions options;
        options.stage = record.stage;
        options.shader_hash = record.hash;
        options.user_data = std::span(count_only_storage).first(record.user_data_count);
        options.user_data_base = record.user_data_base;
        options.wave_size = record.wave_size;
        options.back_code = record.back_code;
        options.bindless_images = Boolean(manifest.at("compiler_settings"), "bindless_images");
        options.dump_ir = dump_ir;
        options.early_dump = false;
        options.dump_label = "ShaderJournalAudit";
        std::visit([&](auto& info) {
            using Info = std::decay_t<decltype(info)>;
            if constexpr (std::is_same_v<Info, ShaderVertexInputInfo>) options.input_info.vertex = &info;
            else if constexpr (std::is_same_v<Info, ShaderPixelInputInfo>) {
                options.input_info.pixel = &info;
                result["omitted_stage_fields"] = record_version == ShaderPrecompile::LegacyRecordVersion
                    ? Json{"sample_vgpr", "target_shader_mask", "parameter_mode"} : Json::array();
            } else options.input_info.compute = &info;
        }, record.info);

        phase = "decode"; Progress(phase);
        ShaderRecompiler::Decoder::Program decoded, back;
        if (!record.back_code.empty() || record.stage == ShaderType::Local)
            decoded = ShaderRecompiler::Decoder::DecodeFrontProgram(record.code);
        else ShaderRecompiler::Decoder::DecodeProgram(record.code, decoded);
        if (!record.back_code.empty()) ShaderRecompiler::Decoder::DecodeProgram(record.back_code, back);
        Json unsupported = Json::array();
        const auto inspect = [&](const auto& program, uint32_t offset, const char* segment) {
            for (const auto& instruction : program.instructions) {
                if (instruction.opcode == ShaderRecompiler::Decoder::Opcode::UNKNOWN ||
                    instruction.opcode == ShaderRecompiler::Decoder::Opcode::UNSUPPORTED)
                    unsupported.push_back({{"pc", instruction.pc + offset}, {"body_pc", instruction.pc},
                        {"code_segment", segment}, {"family", static_cast<uint32_t>(instruction.family)},
                        {"opcode_id", instruction.opcode_id}, {"reason", instruction.unsupported_reason},
                        {"instruction", ShaderRecompiler::Decoder::InstructionToString(instruction)}});
            }
        };
        inspect(decoded, 0u, "main");
        inspect(back, static_cast<uint32_t>(decoded.code.size_bytes()), "back");
        result["instructions"] = decoded.instructions.size() + back.instructions.size();
        result["unsupported_instructions"] = unsupported;
        if (!unsupported.empty()) throw std::runtime_error("unsupported captured instruction encodings");
        phase = "translate"; Progress(phase);
        auto translated = ShaderRecompiler::TranslateProgram(record.code, options);
        if (translated.skip_dispatch) throw std::runtime_error("shared translator rejected recorded permutation; no skip accepted as pass");
        const auto save_text = [&](const char* name, std::string_view text) {
            const auto path = std::filesystem::absolute(name);
            std::ofstream file(path, std::ios::binary);
            file.write(text.data(), static_cast<std::streamsize>(text.size()));
            file.close();
            if (!file) {
                phase = "input";
                throw std::runtime_error(std::string("cannot preserve audit artifact ") + name);
            }
            return path.string();
        };
        if (dump_ir) {
            result["decoded_file"] = save_text("shader.decoded.txt", translated.decoded_dump);
            result["translated_ir_file"] = save_text("shader.translated.ir.txt",
                "CFG:\n" + translated.cfg_dump + "\nIR:\n" +
                ShaderRecompiler::IR::ProgramToString(translated.program));
        }
        phase = "resource_plan"; Progress(phase);
        if (!translated.program.resource_tracking_complete ||
            translated.program.info.buffers.size() != record.specialization.buffers.size() ||
            translated.program.info.images.size() > record.specialization.images.size())
            throw std::runtime_error("recorded specialization does not match translated resource shape");
        phase = "emit"; Progress(phase);
        auto compiled = ShaderRecompiler::CompileProgram(std::move(translated), options,
            record.specialization, record.push_data_start_dword);
        if (dump_ir) result["compiled_ir_file"] = save_text("shader.compiled.ir.txt", compiled.ir_dump);
        const auto output = std::filesystem::absolute("shader.spv");
        std::ofstream artifact(output, std::ios::binary);
        artifact.write(reinterpret_cast<const char*>(compiled.spirv.data()),
            static_cast<std::streamsize>(compiled.spirv.size() * sizeof(uint32_t)));
        artifact.close();
        if (!artifact) { phase = "input"; throw std::runtime_error("cannot preserve emitted SPIR-V"); }
        result["spirv_file"] = output.string();
        result["spirv_words"] = compiled.spirv.size();
        result["spirv_emitted"] = true;
        result["specialization_origin"] = "recorded_journal";
        phase = "validation"; Progress(phase);
        spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_3);
        std::string diagnostic;
        validator.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t&, const char* message) {
            if (diagnostic.size() < 8192u) diagnostic += std::string(message) + '\n';
        });
        if (!validator.Validate(compiled.spirv)) throw std::runtime_error("SPIR-V validation failed: " + diagnostic);
        result["status"] = "passed";
        result["checked_through"] = "spirv_validation";
        result["phase"] = phase;
        std::printf("KYTY_SHADER_AUDIT_RESULT %s\n", result.dump().c_str());
        std::fflush(stdout);
        return 0;
    } catch (const std::exception& error) {
        result["status"] = std::string_view(phase) == "input" ? "input_error" : "failed";
        result["phase"] = phase;
        result["error"] = error.what();
        std::printf("KYTY_SHADER_AUDIT_RESULT %s\n", result.dump().c_str());
        std::fflush(stdout);
        return 1;
    }
}
}

int main(int argc, char* argv[]) {
#if defined(KYTY_RESOURCE_REGRESSION)
    if (argc == 3 && std::string_view(argv[1]) == "--texture-layout-regression")
        return Libs::Graphics::RunTextureLayoutRegression(argv[2]);
#endif
    if (argc >= 3 && argc <= 5 && std::string_view(argv[1]) == "--audit-shader") {
        bool emit_spirv = false, dump_ir = false, valid = true;
        for (int i = 3; i < argc; ++i) {
            const std::string_view option(argv[i]);
            if (option == "--emit-spirv" && !emit_spirv) emit_spirv = true;
            else if (option == "--dump-ir" && !dump_ir) dump_ir = true;
            else valid = false;
        }
        if (valid) return Audit(argv[2], dump_ir);
    }
    std::fprintf(stderr, "Usage: shader_batch_audit --audit-shader journal-record.json [--emit-spirv] [--dump-ir]\n");
    return 2;
}
