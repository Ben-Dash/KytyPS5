// External, opt-in real-input experiment. No game bytes or selection heuristics are
// embedded here. Every image/constant file is explicit and SHA256 verified. This
// proves an offline evaluation only; it never reports a presented game DLSS frame.
using CaptureJson = nlohmann::ordered_json;

std::string CaptureSha256(const void* data, size_t bytes) {
    Require(bytes <= 128u * 1024u * 1024u, "capture SHA256 input exceeds bound");
    struct Crypto {
        HMODULE library = LoadLibraryExW(L"bcrypt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        BCRYPT_ALG_HANDLE algorithm{};
        BCRYPT_HASH_HANDLE hash{};
        decltype(&BCryptCloseAlgorithmProvider) close{};
        decltype(&BCryptDestroyHash) destroy{};
        ~Crypto() {
            if (hash && destroy) destroy(hash);
            if (algorithm && close) close(algorithm, 0);
            if (library) FreeLibrary(library);
        }
    } crypto;
    Require(crypto.library != nullptr, "system SHA256 provider unavailable");
    const auto open = reinterpret_cast<decltype(&BCryptOpenAlgorithmProvider)>(
        GetProcAddress(crypto.library, "BCryptOpenAlgorithmProvider"));
    const auto create = reinterpret_cast<decltype(&BCryptCreateHash)>(
        GetProcAddress(crypto.library, "BCryptCreateHash"));
    const auto update = reinterpret_cast<decltype(&BCryptHashData)>(
        GetProcAddress(crypto.library, "BCryptHashData"));
    const auto finish = reinterpret_cast<decltype(&BCryptFinishHash)>(
        GetProcAddress(crypto.library, "BCryptFinishHash"));
    crypto.close = reinterpret_cast<decltype(crypto.close)>(
        GetProcAddress(crypto.library, "BCryptCloseAlgorithmProvider"));
    crypto.destroy = reinterpret_cast<decltype(crypto.destroy)>(
        GetProcAddress(crypto.library, "BCryptDestroyHash"));
    Require(open && create && update && finish && crypto.close && crypto.destroy,
            "system SHA256 entry points missing");
    Require(open(&crypto.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
            create(crypto.algorithm, &crypto.hash, nullptr, 0, nullptr, 0, 0) >= 0 &&
            update(crypto.hash, const_cast<PUCHAR>(static_cast<const unsigned char*>(data)),
                ULONG(bytes), 0) >= 0, "capture SHA256 setup/update failed");
    std::array<unsigned char, 32> digest{};
    Require(finish(crypto.hash, digest.data(), ULONG(digest.size()), 0) >= 0,
            "capture SHA256 finish failed");
    static constexpr char hex[] = "0123456789abcdef";
    std::string result; result.reserve(64);
    for (auto value : digest) { result += hex[value >> 4]; result += hex[value & 15]; }
    return result;
}

std::vector<unsigned char> CaptureRead(const CaptureJson& description, size_t bytes) {
    const auto path = std::filesystem::u8path(description.at("path").get<std::string>());
    Require(path.is_absolute() && std::filesystem::is_regular_file(path),
            "capture input requires an absolute regular file path");
    Require(bytes > 0 && bytes <= 128u * 1024u * 1024u &&
            std::filesystem::file_size(path) == bytes,
            "capture input file size differs from exact payload");
    std::vector<unsigned char> data(bytes);
    std::ifstream input(path, std::ios::binary);
    Require(input.is_open() && bool(input.read(reinterpret_cast<char*>(data.data()),
                std::streamsize(bytes))), "capture input read failed");
    Require(CaptureSha256(data.data(), data.size()) == description.at("sha256").get<std::string>(),
            "capture input SHA256 mismatch");
    return data;
}

DlssExtent CaptureExtent(const CaptureJson& value) {
    Require(value.is_array() && value.size() == 2, "capture extent needs width and height");
    const auto width = value.at(0).get<uint64_t>(), height = value.at(1).get<uint64_t>();
    Require(width && height && width <= 7680 && height <= 4320,
            "capture extent exceeds bounded experiment");
    return {uint32_t(width), uint32_t(height)};
}

void CaptureWriteJson(const std::filesystem::path& path, const CaptureJson& value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    const auto text = value.dump(2);
    Require(output.is_open() && bool(output.write(text.data(), std::streamsize(text.size()))),
            "capture ledger write failed");
    output.flush(); Require(bool(output), "capture ledger flush failed");
}

int RunCapturedInputs(const std::filesystem::path& manifest_path,
    const std::filesystem::path& runtime, const std::filesystem::path& artifacts,
    const DlssProjectInfo& project) {
    Require(manifest_path.is_absolute() && std::filesystem::is_regular_file(manifest_path) &&
            std::filesystem::file_size(manifest_path) <= 65536,
            "capture manifest needs an absolute regular JSON file <=64KiB");
    std::ifstream manifest_stream(manifest_path, std::ios::binary);
    const auto manifest = CaptureJson::parse(manifest_stream);
    Require(manifest.at("version") == 1 &&
            manifest.at("scope") == "experimental-captured-input" &&
            manifest.at("assumptions").is_array() && !manifest.at("assumptions").empty(),
            "capture manifest requires explicit experimental assumptions");
    Require(manifest.at("frames").is_array() && !manifest.at("frames").empty() &&
            manifest.at("frames").size() <= 2,
            "capture experiment supports one or two isolated-reset frames");
    const auto render = CaptureExtent(manifest.at("render"));
    const auto output = CaptureExtent(manifest.at("output"));
    Require(render.width <= output.width && render.height <= output.height,
            "capture output must contain render extent");
    const size_t pixels = size_t(render.width) * render.height;
    const size_t output_payload = size_t(output.width) * output.height * 8;
    Require(output_payload <= 128u * 1024u * 1024u,
            "capture output payload exceeds readback budget");
    const auto ledger_path = artifacts / "captured-dlss-result.json";
    Require(!std::filesystem::exists(ledger_path), "capture artifact ledger already exists");
    for (size_t i = 0; i < manifest.at("frames").size(); ++i)
        Require(!std::filesystem::exists(artifacts / ("captured-output-" + std::to_string(i) + ".rgba16f")),
                "capture output already exists");
    CaptureJson ledger = {{"scope", "experimental-offline-real-input"}, {"status", "starting"},
        {"manifest", manifest}, {"backend_initialized", 0}, {"feature_created", 0},
        {"evaluate_attempted", 0}, {"evaluate_returned_success", 0},
        {"output_copy_recorded", 0}, {"submits_completed", 0}, {"readbacks_completed", 0},
        {"reset_evaluations", 0}, {"game_evaluations", 0}, {"game_presented_dlss_frames", 0},
        {"validation_through_teardown", false}, {"frames", CaptureJson::array()}};
    const auto save = [&] { CaptureWriteJson(ledger_path, ledger); };
    const auto increment = [&](const char* field) { ledger[field] = ledger[field].get<unsigned>() + 1; };
    save();
    Vulkan vk;
    DlssVulkan dlss;
    try {
        vk.Initialize(project);
        Require(vk.validation_enabled, "captured experiment requires Vulkan validation enabled");
        CheckDlss(vk.instance_query, "Captured QueryInstanceExtensions");
        CheckDlss(vk.device_query, "Captured QueryDeviceExtensions");
        DlssInitializeInfo initialize{}; initialize.project = project;
        initialize.instance = vk.instance; initialize.physical_device = vk.physical;
        initialize.device = vk.device; initialize.get_instance_proc_addr = vk.gipa;
        initialize.get_device_proc_addr = vk.gdpa;
        initialize.enabled_instance_extensions = vk.instance_extensions;
        initialize.enabled_device_extensions = vk.device_extensions;
        CheckDlss(dlss.Initialize(initialize), "Captured Initialize");
        ledger["backend_initialized"] = 1; save();
        DlssConfiguration config{}; config.render = render; config.output = output;
        config.quality = DlssQuality::Performance; config.preset = DlssPreset::TransformerM;
        config.hdr = true; config.auto_exposure = true;
        config.depth_inverted = manifest.at("depth_inverted").get<bool>();
        config.motion_vectors_jittered = manifest.at("motion_vectors_jittered").get<bool>();
        Require(manifest.at("quality") == "performance", "captured fixture requires explicit Performance quality");
        DlssOptimalSettings optimal{};
        CheckDlss(dlss.GetOptimalSettings(output, config.quality, optimal), "Captured GetOptimalSettings");
        ledger["runtime_settings"] = {{"optimal", {optimal.optimal.width, optimal.optimal.height}},
            {"minimum", {optimal.minimum.width, optimal.minimum.height}},
            {"maximum", {optimal.maximum.width, optimal.maximum.height}}};
        const size_t ci = vk.MakeImage(render, VK_FORMAT_R16G16B16A16_SFLOAT);
        const size_t di = vk.MakeImage(render, VK_FORMAT_R32_SFLOAT);
        const size_t mi = vk.MakeImage(render, VK_FORMAT_R16G16_SFLOAT);
        const size_t oi = vk.MakeImage(output, VK_FORMAT_R16G16B16A16_SFLOAT, true);
        const size_t cu = vk.MakeBuffer(pixels * 8), du = vk.MakeBuffer(pixels * 4);
        const size_t mu = vk.MakeBuffer(pixels * 4), readback = vk.MakeBuffer(output_payload + ReadbackGuardBytes);
        vk.Begin(); CheckDlss(dlss.RecordCreate(vk.command, config), "Captured RecordCreate"); vk.Submit();
        ledger["feature_created"] = 1; increment("submits_completed"); save();
        for (size_t frame = 0; frame < manifest.at("frames").size(); ++frame) {
            const auto& source = manifest.at("frames").at(frame);
            Require(source.at("reset_history").get<bool>(), "captured snapshots must use isolated history resets");
            auto color = CaptureRead(source.at("color"), pixels * 8);
            auto depth = CaptureRead(source.at("depth"), pixels * 4);
            auto motion = CaptureRead(source.at("motion"), pixels * 4);
            auto constants = CaptureRead(source.at("constants"), 144);
            // Reject malformed inputs; no fabricated zero/identity resources or cleanup values.
            for (size_t i = 0; i < pixels; ++i) {
                float z{}; std::memcpy(&z, depth.data() + i * 4, 4);
                Require(std::isfinite(z) && z >= 0 && z <= 1, "captured depth is not finite device Z");
                for (size_t channel = 0; channel < 4; ++channel) {
                    uint16_t value{}; std::memcpy(&value, color.data() + (i * 4 + channel) * 2, 2);
                    Require(std::isfinite(Unhalf(value)), "captured color contains nonfinite values");
                }
                for (size_t channel = 0; channel < 2; ++channel) {
                    uint16_t value{}; std::memcpy(&value, motion.data() + (i * 2 + channel) * 2, 2);
                    Require(std::isfinite(Unhalf(value)), "captured motion contains nonfinite values");
                }
            }
            std::memcpy(vk.buffers[cu].mapped, color.data(), color.size());
            std::memcpy(vk.buffers[du].mapped, depth.data(), depth.size());
            std::memcpy(vk.buffers[mu].mapped, motion.data(), motion.size());
            std::memset(vk.buffers[readback].mapped, 0xa5, size_t(vk.buffers[readback].bytes));
            vk.Begin(); vk.Upload(ci, cu); vk.Upload(di, du); vk.Upload(mi, mu);
            vk.Barrier(vk.images[oi], VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT);
            DlssFrameInputs inputs{}; inputs.color = vk.Describe(ci); inputs.depth = vk.Describe(di);
            inputs.motion_vectors = vk.Describe(mi); inputs.output = vk.Describe(oi); inputs.render = render;
            for (size_t axis = 0; axis < 2; ++axis) {
                const auto offset = source.at("jitter_cb_offsets").at(axis).get<size_t>();
                const auto sign = source.at("jitter_signs").at(axis).get<float>();
                Require(offset <= constants.size() - 4 && (offset & 3) == 0 &&
                        (sign == 1.0f || sign == -1.0f), "invalid explicit captured jitter mapping");
                float value{}; std::memcpy(&value, constants.data() + offset, 4);
                (axis == 0 ? inputs.jitter_x : inputs.jitter_y) = sign * value;
            }
            inputs.motion_scale_x = source.at("motion_scale").at(0).get<float>();
            inputs.motion_scale_y = source.at("motion_scale").at(1).get<float>();
            inputs.pre_exposure = source.at("pre_exposure").get<float>();
            inputs.exposure_scale = source.at("exposure_scale").get<float>();
            inputs.frame_time_ms = source.at("frame_time_ms").get<float>();
            inputs.reset_history = true;
            CheckDlss(DlssVulkan::ValidateFrame(config, inputs), "Captured ValidateFrame");
            ledger["status"] = "evaluating"; increment("evaluate_attempted"); save();
            CheckDlss(dlss.RecordEvaluate(vk.command, inputs), "Captured RecordEvaluate");
            increment("evaluate_returned_success"); increment("reset_evaluations");
            vk.Download(oi, readback); increment("output_copy_recorded"); save();
            vk.Submit(); increment("submits_completed");
            if (frame == 0) LogLoadedRuntime(runtime / "nvngx_dlss.dll");
            VerifyGuard(vk.buffers[readback], output_payload);
            const auto* pixels_out = static_cast<const uint16_t*>(vk.buffers[readback].mapped);
            size_t finite_pixels = 0; float minimum = INFINITY, maximum = -INFINITY;
            for (size_t i = 0; i < output_payload / 8; ++i) {
                bool finite = true;
                for (size_t channel = 0; channel < 3; ++channel) {
                    const float value = Unhalf(pixels_out[i * 4 + channel]);
                    finite &= std::isfinite(value); minimum = std::min(minimum, value); maximum = std::max(maximum, value);
                }
                finite_pixels += finite;
            }
            Require(finite_pixels == output_payload / 8 && maximum > minimum + 0.001f,
                    "captured NGX output is nonfinite or spatially constant");
            const auto output_path = artifacts / ("captured-output-" + std::to_string(frame) + ".rgba16f");
            std::ofstream output_stream(output_path, std::ios::binary);
            Require(output_stream.is_open() && bool(output_stream.write(
                static_cast<const char*>(vk.buffers[readback].mapped), std::streamsize(output_payload))),
                "captured output write failed");
            output_stream.flush(); Require(bool(output_stream), "captured output flush failed");
            increment("readbacks_completed");
            ledger["frames"].push_back({{"index", frame}, {"label", source.at("label")},
                {"jitter", {inputs.jitter_x, inputs.jitter_y}}, {"reset_history", true},
                {"finite_rgb_pixels", finite_pixels}, {"pixel_count", output_payload / 8},
                {"minimum_rgb", minimum}, {"maximum_rgb", maximum},
                {"output", output_path.generic_string()},
                {"output_sha256", CaptureSha256(vk.buffers[readback].mapped, output_payload)}});
            save();
        }
        CheckDlss(dlss.DestroyFeatureAfterGpuIdle(), "Captured DestroyFeatureAfterGpuIdle");
        CheckDlss(dlss.ShutdownAfterGpuIdle(), "Captured ShutdownAfterGpuIdle");
        vk.ValidateAndCleanup();
        ledger["validation_through_teardown"] = true;
        ledger["validation_errors"] = vk.validation_errors.load(std::memory_order_relaxed);
        ledger["status"] = "completed-experimental-offline"; save();
        std::printf("CAPTURED INPUT PASS: evaluated=%u copied=%u completed_readbacks=%u isolated_resets=%u; "
                    "game_evaluations=0 game_presented_dlss_frames=0; conventions experimental\n",
            ledger["evaluate_returned_success"].get<unsigned>(), ledger["output_copy_recorded"].get<unsigned>(),
            ledger["readbacks_completed"].get<unsigned>(), ledger["reset_evaluations"].get<unsigned>());
        return 0;
    } catch (const std::exception& error) {
        ledger["status"] = "failed"; ledger["error"] = error.what();
        // Every successful Submit already completed its bounded fence. An unsubmitted
        // recording can be discarded; SDK teardown precedes device destruction.
        (void)dlss.ShutdownAfterGpuIdle(); vk.Cleanup();
        ledger["validation_errors"] = vk.validation_errors.load(std::memory_order_relaxed);
        ledger["validation_through_teardown"] = vk.validation_enabled; save();
        throw;
    }
}
