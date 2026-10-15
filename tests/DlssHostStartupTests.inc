// Native host-startup reproduction. It calls the SAME production prefix as the
// emulator after Config/Log, without loading any ELF, game or guest commands.
int RunHostStartupTest() {
    const auto* validation = std::getenv("KYTY_DLSS_SMOKE_VALIDATION");
    Require(validation && validation[0] == '1', "host startup regression requires actual Vulkan validation");
    _putenv_s("KYTY_DEBUG_VK_ERRORS_NONFATAL", "0");
    // Do not parse/cache temporal metadata, query NGX or inspect/load its runtime
    // before production startup. Their first-use timing is part of this repro.
    const auto* metadata = std::getenv("KYTY_DLSS_TEMPORAL_METADATA");
    Require(metadata && *metadata, "host startup regression requires an external metadata path");
    const auto metadata_path = std::filesystem::u8path(metadata);
    Require(metadata_path.is_absolute() && std::filesystem::is_regular_file(metadata_path),
            "host startup metadata must be an absolute regular file path");
    Common::InitializeThreads();
    {
        Common::Subsystems subsystems(true);
        subsystems.Initialize<Config::Lifecycle>();
        Config::ConfigOptions options;
        options.printf_direction = Config::LogDirection::Console;
        options.screen_width = 1920; options.screen_height = 1080;
        options.bindless_images_enabled = true;
        options.hardware_buffer_bounds = true;
        options.vulkan_validation_enabled = true;
        Config::Load(options);
        subsystems.Initialize<Log::Lifecycle>();
        // Optional numeric footprint matches host reservation inputs without
        // reading proprietary game metadata. Omission retains production default.
        if (const auto* memory_mib = std::getenv("KYTY_DLSS_HOST_FLEXIBLE_MEMORY_MIB")) {
            char* end{};
            const auto value = std::strtoull(memory_mib, &end, 10);
            Require(end && end != memory_mib && *end == '\0' && value > 0 && value <= 2048,
                    "host startup flexible memory must be an integer in1..2048 MiB");
            Libs::LibKernel::Memory::SetFlexibleMemorySize(value * 1024u * 1024u);
        }
        std::puts("HOST STARTUP BEGIN: shared production prefix; no ELF/game/guest commands; "
                  "NGX requested by explicit metadata; production Vulkan errors fatal");
        std::fflush(nullptr);
        Emulator::InitializeHostSubsystems(subsystems);
        std::puts("HOST STARTUP INITIALIZED: production prefix returned; beginning normal reverse teardown");
        std::fflush(nullptr);
        subsystems.Destroy();
    }
    // Production validation remains fatal from instance creation through device
    // teardown. Global WindowContext has no observer accessor: no counter claim.
    std::puts("HOST STARTUP PASS: shared prefix and normal teardown; no ELF/game/guest commands; "
              "mandatory fatal Vulkan validation (no aggregate counter); "
              "NGX backend status is separately logged by temporal-dlss initialize");
    std::fflush(nullptr);
    return 0;
}
