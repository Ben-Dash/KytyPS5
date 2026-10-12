// Include at GLOBAL scope in ShaderRecompilerComputeTests.cpp, after headers.
// Production header friends this test adapter only; no runtime implementation here.
namespace Libs::Graphics {
struct RecordingAgeSchedulerTestAccess {
  static auto RecordingBegin(const CommandScheduler& scheduler) {
    return scheduler.m_recording_begin;
  }
  static void CheckAt(CommandScheduler& scheduler,
                      std::chrono::steady_clock::time_point now) {
    scheduler.FlushIfRecordingAgedAt(now);
  }
};
} // namespace Libs::Graphics
