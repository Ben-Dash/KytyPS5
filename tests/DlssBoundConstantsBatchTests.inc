// Independent native GPU snapshot batching regression. No NGX or guest bytes.
int RunBoundConstantsBatchTest() {
    const auto* validation_environment = std::getenv("KYTY_DLSS_SMOKE_VALIDATION");
    Require(validation_environment && std::strcmp(validation_environment, "1") == 0,
            "constant batch regression requires actual Vulkan validation");
    _putenv_s("KYTY_DEBUG_VK_ERRORS_NONFATAL", "0");
    const auto* record_environment = std::getenv("KYTY_DLSS_BOUND_CONSTANTS_RECORD_THREAD");
    Require(!record_environment || std::strcmp(record_environment, "0") == 0 ||
            std::strcmp(record_environment, "1") == 0, "constant batch recording mode must be0 or1");
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
    RendererValidation validation; // Outlives final actual WindowContext teardown.
    WindowContext window;
    window.graphic_ctx.screen_width = 1920; window.graphic_ctx.screen_height = 1080;
    window.window = SDL_CreateWindow("Kyty constant batch regression", 1920, 1080,
                                     SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    Require(window.window != nullptr, SDL_GetError());
    window.CreateVulkan(); // Ordinary production renderer, no NGX project.
    validation.Attach(window.graphic_ctx, true);
    Require(window.render_context && !window.requested_dlss_project &&
            CommandRecordingDeferred() == record_thread, "constant batch renderer bootstrap mismatch");
    auto& context = *window.render_context;
    auto& graphics = window.graphic_ctx;
    auto& scheduler = context.GetCommandScheduler();
    HW::Context registers{}; HW::UserConfig user_config{}; HW::Shader shaders{};
    scheduler.Begin(registers, user_config, shaders);
    constexpr size_t Count = 144;
    constexpr uint32_t Adjustment = 12;
    const auto alignment = std::max<uint64_t>(graphics.StorageMinAlignment(), 256u);
    const uint64_t descriptor_offset = 2 * alignment;
    const uint64_t selected_offset = descriptor_offset + Adjustment;
    Libs::Graphics::Buffer source(graphics, scheduler, MemoryUsage::DeviceLocal, 0,
                                  AllFlags, selected_offset + Count + 16);
    Libs::Graphics::Buffer independent_a(graphics, scheduler, MemoryUsage::Download, 0, AllFlags, Count);
    Libs::Graphics::Buffer independent_b(graphics, scheduler, MemoryUsage::Download, 0, AllFlags, Count);
    Libs::Graphics::Buffer continued(graphics, scheduler, MemoryUsage::Download, 0, AllFlags, Count);
    const vk::DescriptorBufferInfo view{source.Handle(), descriptor_offset, Count + Adjustment};
    std::array<uint32_t, Count / 4> words_a{}, words_b{};
    for (size_t i = 0; i < words_a.size(); ++i) {
        words_a[i] = 0x12345678u ^ (uint32_t(i) * 0x03050709u);
        words_b[i] = 0xd3c2b1a0u ^ (uint32_t(i) * 0x11131517u);
    }
    std::array<uint8_t, Count> expected_a{}, expected_b{};
    std::memcpy(expected_a.data(), words_a.data(), Count);
    std::memcpy(expected_b.data(), words_b.data(), Count);
    const auto write_and_control = [&](const auto& words, Libs::Graphics::Buffer* control) {
        auto& current = scheduler.Current(); current.EndRendering();
        auto command = current.Handle();
        const vk::MemoryBarrier overwrite{
            .srcAccessMask=vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
            .dstAccessMask=vk::AccessFlagBits::eTransferWrite};
        command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer,
                                {}, 1, &overwrite, 0, nullptr, 0, nullptr);
        command.updateBuffer(source.Handle(), selected_offset, Count, words.data());
        if (control) {
            const vk::MemoryBarrier readable{.srcAccessMask=vk::AccessFlagBits::eTransferWrite,
                                             .dstAccessMask=vk::AccessFlagBits::eTransferRead};
            command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
                                    {}, 1, &readable, 0, nullptr, 0, nullptr);
            const vk::BufferCopy copy{selected_offset, 0, Count};
            command.copyBuffer(source.Handle(), control->Handle(), 1, &copy);
            const vk::MemoryBarrier host{.srcAccessMask=vk::AccessFlagBits::eTransferWrite,
                                        .dstAccessMask=vk::AccessFlagBits::eHostRead};
            command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
                                    {}, 1, &host, 0, nullptr, 0, nullptr);
        }
        scheduler.NoteCommandRecorded();
    };
    DlssBoundConstantSnapshot first, second;
    write_and_control(words_a, &independent_a);
    bool retired = false;
    scheduler.DeferOperation([&retired] { retired = true; });
    auto* const wrapper = &scheduler.Current();
    const auto before_pair = scheduler.CurrentTick();
    CheckDlss(QueueDlssBoundConstants(context, view, Adjustment, Count, first), "queue exact native snapshot A");
    write_and_control(words_b, &independent_b); // SAME handle/offset/range, changed bytes.
    CheckDlss(QueueDlssBoundConstants(context, view, Adjustment, Count, second), "queue exact native snapshot B");
    words_a.fill(0xc3c3c3c3); words_b.fill(0xa1a1a1a1); // Caller arrays are no longer the oracle.
    std::array<uint8_t, Count> actual_a{}, actual_b{};
    const std::array pair{DlssBoundConstantRead{&first, actual_a}, DlssBoundConstantRead{&second, actual_b}};
    CheckDlss(FinishDlssBoundConstants(context, pair), "finish native A/B pair");
    const auto pair_submits = scheduler.CurrentTick() - before_pair;
    Require(!retired && &scheduler.Current() == wrapper && !wrapper->IsInvalid(),
            "constant batch retired caller work or invalidated its current wrapper");
    Require(actual_a == expected_a && actual_b == expected_b && actual_a != actual_b,
            "two snapshots lost exact A/B bytes across a same-descriptor overwrite");
    // Independent native copies and continued recording must pass before cost is
    // interpreted. Setup, stale CPU bytes and unrelated failures are not RED.
    auto command = scheduler.Current().Handle();
    const vk::MemoryBarrier readable{.srcAccessMask=vk::AccessFlagBits::eMemoryWrite,
                                     .dstAccessMask=vk::AccessFlagBits::eTransferRead};
    command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer,
                            {}, 1, &readable, 0, nullptr, 0, nullptr);
    const vk::BufferCopy copy{selected_offset, 0, Count};
    command.copyBuffer(source.Handle(), continued.Handle(), 1, &copy);
    const vk::MemoryBarrier host{.srcAccessMask=vk::AccessFlagBits::eTransferWrite,
                                .dstAccessMask=vk::AccessFlagBits::eHostRead};
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
                            {}, 1, &host, 0, nullptr, 0, nullptr);
    scheduler.NoteCommandRecorded();
    CompleteBounded(graphics, scheduler, scheduler.Submit());
    independent_a.Invalidate(0, Count); independent_b.Invalidate(0, Count); continued.Invalidate(0, Count);
    Require(std::memcmp(independent_a.Mapped().data(), expected_a.data(), Count) == 0 &&
            std::memcmp(independent_b.Mapped().data(), expected_b.data(), Count) == 0 &&
            std::memcmp(continued.Mapped().data(), expected_b.data(), Count) == 0 && retired,
            "independent GPU A/B producer, continuation or retirement controls failed");
    std::puts("CONSTANT BATCH CONTROL PASS: independent native A/B exact, same descriptor overwrite detected, continuation/lifetime valid");

    // Equal snapshots must remain equal, still paying only one explicit batch wait.
    scheduler.Begin(registers, user_config, shaders);
    std::memcpy(words_a.data(), expected_a.data(), Count);
    write_and_control(words_a, nullptr);
    DlssBoundConstantSnapshot equal_a, equal_b;
    const auto before_equal = scheduler.CurrentTick();
    CheckDlss(QueueDlssBoundConstants(context, view, Adjustment, Count, equal_a), "queue equal A1");
    CheckDlss(QueueDlssBoundConstants(context, view, Adjustment, Count, equal_b), "queue equal A2");
    const std::array equal{DlssBoundConstantRead{&equal_a, actual_a}, DlssBoundConstantRead{&equal_b, actual_b}};
    CheckDlss(FinishDlssBoundConstants(context, equal), "finish equal native pair");
    const auto equal_submits = scheduler.CurrentTick() - before_equal;
    Require(actual_a == expected_a && actual_b == expected_a, "equal native constants no longer associate");

    // Unrelated queue submissions may already own both copy ticks by Finish.
    // Resolving must wait their maximum actual tick, without submitting the
    // caller's new empty command buffer or running retirement callbacks.
    DlssBoundConstantSnapshot submitted_a, submitted_b;
    bool submitted_retired = false;
    scheduler.DeferOperation([&submitted_retired] { submitted_retired = true; });
    write_and_control(words_a, nullptr);
    CheckDlss(QueueDlssBoundConstants(context, view, Adjustment, Count, submitted_a),
              "queue externally submitted A");
    const auto external_a = scheduler.Submit();
    scheduler.BeginCommand();
    std::memcpy(words_b.data(), expected_b.data(), Count);
    write_and_control(words_b, nullptr);
    CheckDlss(QueueDlssBoundConstants(context, view, Adjustment, Count, submitted_b),
              "queue externally submitted B");
    const auto external_b = scheduler.Submit();
    scheduler.BeginCommand();
    const auto before_submitted_finish = scheduler.CurrentTick();
    const std::array submitted{DlssBoundConstantRead{&submitted_a, actual_a},
                               DlssBoundConstantRead{&submitted_b, actual_b}};
    CheckDlss(FinishDlssBoundConstants(context, submitted), "finish already submitted A/B tickets");
    Require(actual_a == expected_a && actual_b == expected_b &&
            scheduler.CurrentTick() == before_submitted_finish && !submitted_retired &&
            !scheduler.Current().IsInvalid(),
            "already submitted tickets lost bytes, submitted again or retired caller callbacks");
    CompleteBounded(graphics, scheduler, std::max(external_a, external_b));
    Require(submitted_retired, "external native copy tick never published its retirement control");
    std::puts("CONSTANT BATCH SUBMITTED PASS: distinct prior copy ticks, exact A/B, finish_submits=0, no early callbacks");

    // A host-stream ticket owns the bound bytes even if the caller changes its source.
    std::array<uint8_t, Count + Adjustment> host_source{};
    for (size_t i = 0; i < host_source.size(); ++i) host_source[i] = uint8_t(i * 7u + 13u);
    const auto expected_stream = host_source;
    auto& stream = context.GetBufferCache().ActiveStream();
    Require((stream.MemoryProperties() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0,
            "constant batch fixture requires actual immutable host stream");
    const auto offset = stream.Copy(host_source.data(), host_source.size(), alignment);
    host_source.fill(0xdd);
    DlssBoundConstantSnapshot stream_ticket;
    const auto before_stream = scheduler.CurrentTick();
    CheckDlss(QueueDlssBoundConstants(context, {stream.Handle(), offset, expected_stream.size()},
        Adjustment, Count, stream_ticket), "queue immutable host stream");
    std::array<uint8_t, Count> actual_stream{};
    const std::array stream_batch{DlssBoundConstantRead{&stream_ticket, actual_stream}};
    CheckDlss(FinishDlssBoundConstants(context, stream_batch), "finish immutable host stream");
    Require(scheduler.CurrentTick() == before_stream &&
            std::memcmp(actual_stream.data(), expected_stream.data() + Adjustment, Count) == 0,
            "host-stream batch submitted GPU work or used later source bytes");

    std::array<uint8_t, Count> guard{}; guard.fill(0x7d); const auto untouched = guard;
    const auto before_guards = scheduler.CurrentTick();
    uint32_t guard_cases = 0;
    const auto refuse_batch = [&](std::span<const DlssBoundConstantRead> request) {
        Require(!FinishDlssBoundConstants(context, request) && guard == untouched &&
                scheduler.CurrentTick() == before_guards, "invalid batch changed outputs or submitted");
        ++guard_cases;
    };
    refuse_batch({});
    DlssBoundConstantSnapshot missing;
    const std::array absent{DlssBoundConstantRead{&missing, guard}}; refuse_batch(absent);
    const std::array short_output{DlssBoundConstantRead{&stream_ticket, std::span(guard).first(Count - 1)}};
    refuse_batch(short_output);
    const std::array late_failure{DlssBoundConstantRead{&stream_ticket, guard}, DlssBoundConstantRead{nullptr, guard}};
    refuse_batch(late_failure); // Must validate every request before the first memcpy.
    const std::array too_many{DlssBoundConstantRead{&stream_ticket, guard}, DlssBoundConstantRead{&stream_ticket, guard},
        DlssBoundConstantRead{&stream_ticket, guard}, DlssBoundConstantRead{&stream_ticket, guard}};
    refuse_batch(too_many);
    auto invalid_view = view; invalid_view.range = Count + Adjustment - 1;
    Require(!QueueDlssBoundConstants(context, invalid_view, Adjustment, Count, missing) &&
            !missing.state && scheduler.CurrentTick() == before_guards,
            "invalid queued descriptor changed ticket or submitted");
    const auto saved_ticket = stream_ticket.state;
    Require(!QueueDlssBoundConstants(context, view, Adjustment, Count, stream_ticket) &&
            stream_ticket.state == saved_ticket && scheduler.CurrentTick() == before_guards,
            "occupied destination ticket was replaced");
    // The declared maximum is usable; already resolved tickets need no new submit.
    const std::array maximum{DlssBoundConstantRead{&first, actual_a}, DlssBoundConstantRead{&second, actual_b},
        DlssBoundConstantRead{&stream_ticket, actual_stream}};
    CheckDlss(FinishDlssBoundConstants(context, maximum), "finish maximum three resolved tickets");
    Require(actual_a == expected_a && actual_b == expected_b && scheduler.CurrentTick() == before_guards,
            "resolved ticket reuse changed bytes or submitted");
    CompleteBounded(graphics, scheduler, scheduler.Submit());
    std::printf("CONSTANT BATCH NEIGHBORS PASS: recorder=%d guards=%u stream_submits=0 max_batch=3; teardown validation pending\n",
                record_thread ? 1 : 0, guard_cases + 2);
    std::printf("CONSTANT BATCH COST: changed_pair_submits=%llu equal_pair_submits=%llu expected_each=1\n",
                static_cast<unsigned long long>(pair_submits), static_cast<unsigned long long>(equal_submits));
    const bool success = pair_submits == 1 && equal_submits == 1;
    std::puts(success ? "CONSTANT BATCH PASS: exact snapshots with one submit per native pair" :
        "CONSTANT BATCH INTENDED RED: exact snapshots still submit twice per native pair");
    // Return normally even for intended RED, so final device teardown is observed.
    return success ? 0 : 1;
}
