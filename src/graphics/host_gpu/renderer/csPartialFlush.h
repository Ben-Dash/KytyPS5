// Attach one instance to the scheduler's stable CommandBuffer wrapper.
// It tracks native producer stages conservatively, not resources or shader/title identities.
#pragma once
#include "common/switches.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include <cstring>

namespace Libs::Graphics {
// Host scheduling policy only: no shader/SPIR-V/pipeline cache changes. Pin before
// the first CS event; different policy trials use separate native processes.
[[nodiscard]] inline bool CsPartialFlushStagesEnabled() noexcept {
  static const bool enabled=[] {
    const auto* value=KYTY_GETENV("KYTY_CS_PARTIAL_STAGE_SCOPED");
    return value!=nullptr&&std::strcmp(value,"1")==0;
  }();
  return enabled;
}
class CsPartialFlushProducerStages {
public:
  // Current guest CS substitutions may issue native transfer clears/copies, and an
  // aliased ClearImage may issue a rendering loadOp clear at ColorAttachmentOutput.
  // Note at the ACTUAL native producer branch. Recording any such clear, including
  // clears from another guest queue, is a conservative overapproximation on our one host queue.
  void Note(vk::PipelineStageFlags2 source) noexcept { m_substitutions |= source; }

  [[nodiscard]] vk::MemoryBarrier2 Plan() const noexcept {
    const auto compute = vk::PipelineStageFlagBits2::eComputeShader;
    const auto transfer = vk::PipelineStageFlagBits2::eAllTransfer;
    const auto color = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    const auto allowed = compute | transfer | color;
    vk::MemoryBarrier2 barrier{};
    // Preserve an explicit conservative fallback for any newly added unsupported
    // native substitution. Do not silently drop a source stage from the plan.
    barrier.srcStageMask = (m_substitutions & ~allowed)
        ? (vk::PipelineStageFlagBits2::eAllCommands |
           (m_substitutions & vk::PipelineStageFlagBits2::eHost))
        : (compute | transfer | m_substitutions);
    barrier.srcAccessMask = vk::AccessFlagBits2::eMemoryWrite;
    barrier.dstStageMask = vk::PipelineStageFlagBits2::eAllCommands;
    barrier.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
    return barrier;
  }

  // ONLY after the actual covering CS07 pipelineBarrier2 has been recorded in order.
  // Merely constructing a plan, a guest slice switch, Begin/BeginNext, submit, fault,
  // allocation retirement or resetting a pooled VkCommandBuffer MUST NOT clear it.
  // The CommandScheduler has one stable wrapper, so earlier submitted native color
  // producers remain represented until this queue-ordered dependency covers them.
  void CoveredByRecordedCsPartialFlush() noexcept { m_substitutions = {}; }

private:
  vk::PipelineStageFlags2 m_substitutions{};
};
} // namespace Libs::Graphics
