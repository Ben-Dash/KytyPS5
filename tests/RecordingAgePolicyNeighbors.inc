// Include INSIDE VulkanHarness. Uncompiled neighbor proposal AFTER valid RED/GREEN.
// Existing runtime/native buffer helpers; the clock adapter calls the SAME age policy.
void CheckSchedulerRecordingAgePolicy(bool disabled = false, bool asynchronous = false) {
  constexpr const char* name = "SchedulerRecordingAgePolicy";
  using Access = RecordingAgeSchedulerTestAccess;
  using Micros = std::chrono::microseconds;
  EnsureRuntimeContext();
  CommandScheduler scheduler(Renderer(), m_runtime_context);
  if (asynchronous) scheduler.EnableAsyncSubmit();
  const auto initialized = scheduler.CurrentTick();
  Access::CheckAt(scheduler, std::chrono::steady_clock::now() + std::chrono::hours(1));
  Require(name, "inactive has no submission", scheduler.CurrentTick() == initialized,
          "inactive/invalid scheduler auto-submitted work");
  HW::Context registers{};
  HW::UserConfig user_config{};
  HW::Shader shaders{};
  scheduler.Begin(registers, user_config, shaders);
  auto* const command = &scheduler.Current();
  const auto beginning = Access::RecordingBegin(scheduler);
  const auto empty_tick = scheduler.CurrentTick();
  (void)command->Handle();
  command->SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DispatchDirect), 1, 1, 1, 1);
  Access::CheckAt(scheduler, beginning + Micros(8000));
  Require(name, "handle/debug state is not execution work",
          scheduler.CurrentTick() == empty_tick,
          "acquiring a command handle or changing diagnostics submitted an empty command");

  auto output = CreateHostBuffer(name, 4u * sizeof(u32),
                                vk::BufferUsageFlagBits::eTransferDst,
                                std::vector<u32>(4, 0xdeadbeefu));
  const auto RecordFill = [&](u32 word) {
    const auto native = scheduler.Current().Handle();
    native.fillBuffer(output.buffer, 0, 4u * sizeof(u32), word);
    vk::MemoryBarrier visibility{};
    visibility.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    visibility.dstAccessMask = vk::AccessFlagBits::eHostRead;
    native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                           vk::PipelineStageFlagBits::eHost, {},
                           1, &visibility, 0, nullptr, 0, nullptr);
    scheduler.NoteCommandRecorded(); // after actual native work, not a fake handle
  };
  RecordFill(0x11223344u);
  Access::CheckAt(scheduler, beginning - Micros(1));
  Require(name, "negative age has no submission", scheduler.CurrentTick() == empty_tick,
          "a clock value before command begin triggered a submission");
  Access::CheckAt(scheduler, beginning + Micros(999));
  Require(name, "below1000us budget", scheduler.CurrentTick() == empty_tick,
          "recording submitted before the configured budget");

  std::atomic<bool> callback_done{false};
  scheduler.DeferPriorityOperation([&] {
    // A deferred callback must never initiate a submit. The producer is idle in Wait below.
    Access::CheckAt(scheduler, beginning + std::chrono::hours(1));
    callback_done.store(true, std::memory_order_release);
  });
  Access::CheckAt(scheduler, beginning + Micros(1000));
  const auto at_budget = scheduler.CurrentTick();
  Require(name, "exact threshold splits at most once",
          at_budget == empty_tick + (disabled ? 0u : 1u),
          "enabled exact threshold failed to submit, or disabled policy submitted");
  Require(name, "stable guest context bindings",
          &scheduler.Current() == command &&
            &command->GetRegisters() == &registers &&
            &command->GetUserConfig() == &user_config &&
            &command->GetShaders() == &shaders,
          "recording restart replaced the command object or guest bindings");
  if (!disabled) {
    const auto renewed = Access::RecordingBegin(scheduler);
    Access::CheckAt(scheduler, renewed + Micros(8000));
    Require(name, "new empty recording stays empty", scheduler.CurrentTick() == at_budget,
            "age alone resubmitted an empty command after the earlier work");
    RecordFill(0x55667788u);
    Access::CheckAt(scheduler, renewed + Micros(1001));
    Require(name, "over-budget real work splits once", scheduler.CurrentTick() == at_budget + 1u,
            "fresh native work failed to submit after the configured budget");
    const auto empty_again = Access::RecordingBegin(scheduler);
    Access::CheckAt(scheduler, empty_again + std::chrono::hours(1));
    Require(name, "repeated empty checks do not submit",
            scheduler.CurrentTick() == at_budget + 1u,
            "empty recording produced repeated automatic submissions");
  }
  const auto before_wait = scheduler.CurrentTick();
  // Finish/Wait may legitimately make one explicit empty submission; the auto-policy cost
  // above is recorded first. It still must not submit recursively from the deferred callback.
  scheduler.Finish();
  scheduler.WaitPriorityOperations(empty_tick);
  Require(name, "deferred callback completed without recursive submit",
          callback_done.load(std::memory_order_acquire) &&
            scheduler.CurrentTick() == before_wait + 1u,
          "pending callback did not complete or initiated an extra automatic submission");
  const auto actual = ReadBuffer(name, output, 4);
  const auto expected = disabled ? 0x11223344u : 0x55667788u;
  Require(name, "actual native fill preserved through policy splits",
          actual == std::vector<u32>(4, expected),
          "automatic recording restart lost or reordered native buffer writes");
  DestroyBuffer(&output);
  scheduler.Shutdown();
  std::printf("[gpu] %-32s ok disabled=%u async=%u\n", name, disabled ? 1u : 0u,
              asynchronous ? 1u : 0u);
}
