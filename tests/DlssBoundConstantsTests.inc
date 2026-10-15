// Independent actual-native-buffer regression. No NGX, guest ELF or game bytes.
// Included in DlssPresenterGpuTests.cpp after its bounded renderer helpers.
int RunBoundConstantsTest() {
    const auto* validation_environment = std::getenv("KYTY_DLSS_SMOKE_VALIDATION");
    Require(validation_environment && std::strcmp(validation_environment, "1") == 0,
            "bound constants regression requires actual Vulkan validation");
    _putenv_s("KYTY_DEBUG_VK_ERRORS_NONFATAL", "0");
    const auto* record_environment = std::getenv("KYTY_DLSS_BOUND_CONSTANTS_RECORD_THREAD");
    Require(!record_environment || std::strcmp(record_environment, "0") == 0 ||
            std::strcmp(record_environment, "1") == 0,
            "bound constants recording mode must be0 or1");
    const bool record_thread = record_environment && std::strcmp(record_environment, "1") == 0;
    _putenv_s("KYTY_RECORD_THREAD", record_thread ? "1" : "0");
    Common::InitializeThreads();
    Common::Subsystems subsystems;
    subsystems.Initialize<Config::Lifecycle>();
    Config::ConfigOptions options;
    options.printf_direction = Config::LogDirection::Console;
    options.gpu_frames_ahead = 0; options.label_flush_interval_us = 0;
    options.screen_width = 1920; options.screen_height = 1080;
    options.vulkan_validation_enabled = true;
    Config::Load(options);
    subsystems.Initialize<Log::Lifecycle>();
    subsystems.Initialize<Libs::LibKernel::Memory::Lifecycle>();
    ShaderInit();
    Require(SDL_InitSubSystem(SDL_INIT_VIDEO), SDL_GetError());
    RendererValidation validation; // Outlives actual WindowContext/device teardown.
    WindowContext window;
    window.graphic_ctx.screen_width = 1920; window.graphic_ctx.screen_height = 1080;
    window.window = SDL_CreateWindow("Kyty bound constants regression", 1920, 1080,
                                     SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    Require(window.window != nullptr, SDL_GetError());
    window.CreateVulkan(); // Plain production renderer; no NGX extension/project request.
    validation.Attach(window.graphic_ctx, true);
    Require(!window.requested_dlss_project && window.render_context && window.presenter,
            "bound constants fixture did not construct an ordinary actual renderer");
    Require(CommandRecordingDeferred() == record_thread,
            "requested bound constants recording mode was not actually installed");

    auto& context = *window.render_context;
    auto& graphics = window.graphic_ctx;
    auto& scheduler = context.GetCommandScheduler();
    HW::Context registers{}; HW::UserConfig user_config{}; HW::Shader shaders{};
    scheduler.Begin(registers, user_config, shaders);
    constexpr size_t Count = 144;
    constexpr uint32_t Adjustment = 12;
    std::array<uint8_t, Count + Adjustment> host_source{};
    for (size_t i = 0; i < host_source.size(); ++i) host_source[i] = uint8_t(3u * i + 11u);
    const auto expected_stream = host_source;
    auto& stream = context.GetBufferCache().ActiveStream();
    Require((stream.MemoryProperties() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0,
            "fixture requires the immutable host-stream configuration");
    const auto stream_offset = stream.Copy(host_source.data(), host_source.size(), 256);
    host_source.fill(0xc3); // Later CPU state cannot replace immutable bound bytes.
    const vk::DescriptorBufferInfo stream_view{stream.Handle(), stream_offset, expected_stream.size()};
    std::array<uint8_t, Count> actual_stream{};
    const auto stream_tick = scheduler.CurrentTick();
    const auto stream_status = ReadDlssBoundConstants(context, stream_view, Adjustment, actual_stream);
    Require(bool(stream_status) && scheduler.CurrentTick() == stream_tick &&
            std::memcmp(actual_stream.data(), expected_stream.data() + Adjustment, Count) == 0,
            "existing immutable stream path changed bytes or submitted GPU work");
    std::array<uint8_t, Count> rejected{}; rejected.fill(0x6d);
    const auto before_reject = rejected;
    auto short_view = stream_view; short_view.range = Count + Adjustment - 1;
    Require(!ReadDlssBoundConstants(context, short_view, Adjustment, rejected) &&
            rejected == before_reject && scheduler.CurrentTick() == stream_tick,
            "short actual descriptor did not refuse without changing output/submitting");

    const uint64_t alignment = std::max<uint64_t>(graphics.StorageMinAlignment(), 256u);
    const uint64_t descriptor_offset = alignment * 2;
    const uint64_t selected_offset = descriptor_offset + Adjustment;
    Libs::Graphics::Buffer source(graphics, scheduler, MemoryUsage::DeviceLocal, 0,
                                  AllFlags, selected_offset + Count + 16);
    Libs::Graphics::Buffer independent(graphics, scheduler, MemoryUsage::Download, 0,
                                       AllFlags, Count);
    Libs::Graphics::Buffer continued(graphics, scheduler, MemoryUsage::Download, 0,
                                     AllFlags, Count);
    std::array<uint32_t, Count / sizeof(uint32_t)> gpu_words{};
    for (size_t i = 0; i < gpu_words.size(); ++i)
        gpu_words[i] = 0x7b310d25u ^ (uint32_t(i) * 0x01020409u);
    std::array<uint8_t, Count> expected_gpu{};
    std::memcpy(expected_gpu.data(), gpu_words.data(), Count);
    std::array<uint8_t, Count> cpu_placeholder{}; cpu_placeholder.fill(0xa1);
    Require(cpu_placeholder != expected_gpu, "synthetic CPU placeholder accidentally equals GPU oracle");
    const vk::DescriptorBufferInfo device_view{source.Handle(), descriptor_offset, Count + Adjustment};
    auto* const wrapper = &scheduler.Current();
    wrapper->EndRendering();
    auto command = wrapper->Handle();
    command.updateBuffer(source.Handle(), selected_offset, Count, gpu_words.data());
    const vk::MemoryBarrier before_copy{.srcAccessMask=vk::AccessFlagBits::eTransferWrite,
                                       .dstAccessMask=vk::AccessFlagBits::eTransferRead};
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
                            {}, 1, &before_copy, 0, nullptr, 0, nullptr);
    const vk::BufferCopy region{selected_offset, 0, Count};
    command.copyBuffer(source.Handle(), independent.Handle(), 1, &region);
    const vk::MemoryBarrier host_read{.srcAccessMask=vk::AccessFlagBits::eTransferWrite,
                                     .dstAccessMask=vk::AccessFlagBits::eHostRead};
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
                            {}, 1, &host_read, 0, nullptr, 0, nullptr);
    scheduler.NoteCommandRecorded();
    cpu_placeholder.fill(0xc3);
    Require(cpu_placeholder != expected_gpu, "later CPU placeholder accidentally equals GPU oracle");
    bool retired = false;
    scheduler.DeferOperation([&retired] { retired = true; });
    std::array<uint8_t, Count> actual{}; actual.fill(0x6d);
    const auto untouched = actual;
    const auto status = ReadDlssBoundConstants(context, device_view, Adjustment, actual);
    std::printf("BOUND CONSTANTS actual-device status=%u native=0x%08x message=%s bytes=%zu adjustment=%u\n",
                unsigned(status.code), status.native_result, status.message, Count, Adjustment);
    Require(!retired && &scheduler.Current() == wrapper && !scheduler.Current().IsInvalid(),
            "bound read retired prepared work or lost the current command wrapper");
    if (!status) Require(actual == untouched, "refused actual binding changed output bytes");

    // Reacquire the native handle after a possible helper submission. This also
    // proves subsequent recording is valid, independently from helper output.
    command = scheduler.Current().Handle();
    command.copyBuffer(source.Handle(), continued.Handle(), 1, &region);
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
                            {}, 1, &host_read, 0, nullptr, 0, nullptr);
    scheduler.NoteCommandRecorded();
    CompleteBounded(graphics, scheduler, scheduler.Submit());
    independent.Invalidate(0, Count); continued.Invalidate(0, Count);
    Require(std::memcmp(independent.Mapped().data(), expected_gpu.data(), Count) == 0 &&
            std::memcmp(continued.Mapped().data(), expected_gpu.data(), Count) == 0 && retired,
            "independent native GPU producer/continuation/lifetime control failed");
    std::puts("BOUND CONSTANTS CONTROL PASS: native producer bytes and continued recording verified; no guest backing read");
    std::fflush(stdout);
    Require(bool(status), "ordinary actual-bound device constants unsupported (intended baseline RED)");
    Require(actual == expected_gpu, "actual-bound snapshot differs from independently produced native GPU bytes");
    // Cheap neighboring invalid requests must reject before any native copy or
    // submit. Keep the independently proved144-byte native oracle above intact.
    scheduler.Begin(registers, user_config, shaders);
    uint32_t guard_cases = 0;
    const auto refuses = [&](vk::DescriptorBufferInfo view, uint32_t adjustment,
                             std::span<uint8_t> destination) {
        const std::vector<uint8_t> before(destination.begin(), destination.end());
        const auto tick = scheduler.CurrentTick();
        const auto rejected_status = ReadDlssBoundConstants(context, view, adjustment, destination);
        Require(!rejected_status && rejected_status.code == DlssStatusCode::InvalidArgument &&
                scheduler.CurrentTick() == tick && !scheduler.Current().IsInvalid() &&
                std::equal(destination.begin(), destination.end(), before.begin(), before.end()),
                "invalid constant request changed output, submitted, or bypassed its bounds guard");
        ++guard_cases;
    };
    std::array<uint8_t, Count> guard_output{}; guard_output.fill(0x7d);
    auto invalid_view = device_view; invalid_view.buffer = nullptr;
    refuses(invalid_view, Adjustment, guard_output);
    invalid_view = device_view; invalid_view.range = VK_WHOLE_SIZE;
    refuses(invalid_view, Adjustment, guard_output);
    invalid_view = device_view; invalid_view.range = 0;
    refuses(invalid_view, 0, guard_output);
    refuses(device_view, uint32_t(device_view.range) + 1u, guard_output);
    invalid_view = device_view; invalid_view.range = Count + Adjustment - 1;
    refuses(invalid_view, Adjustment, guard_output);
    invalid_view = device_view; invalid_view.offset = UINT64_MAX - 3u;
    refuses(invalid_view, Adjustment, guard_output);
    invalid_view = device_view; invalid_view.offset = UINT64_MAX - Count + 1u;
    refuses(invalid_view, 0, guard_output);
    refuses(device_view, Adjustment, std::span<uint8_t>(guard_output).first(0));
    std::array<uint8_t, 1025> too_many{}; too_many.fill(0x5b);
    invalid_view = device_view; invalid_view.range = too_many.size() + Adjustment;
    refuses(invalid_view, Adjustment, too_many);
    std::printf("BOUND CONSTANTS NEIGHBORS PASS: recording_thread=%d invalid_no_submit=%u\n",
                record_thread ? 1 : 0, guard_cases);
    std::puts("BOUND CONSTANTS PASS: exact native descriptor144B; stream/bounds/producer/continuation/lifetime; teardown validation pending");
    return 0;
}
