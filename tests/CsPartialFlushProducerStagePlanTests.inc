// CPU NEIGHBORS ONLY, after the unchanged real old-API native fixture proves cost RED.
// Invoke actual production planner; no duplicate expected GPU lookup or game capture.
void CheckCsPartialFlushProducerStagePlan() {
  using namespace Libs::Graphics;
  constexpr const char* name="CsPartialFlushProducerStagePlan";
  const auto compute=vk::PipelineStageFlagBits2::eComputeShader;
  const auto transfer=vk::PipelineStageFlagBits2::eAllTransfer;
  const auto color=vk::PipelineStageFlagBits2::eColorAttachmentOutput;
  const auto all=vk::PipelineStageFlagBits2::eAllCommands;
  const auto writes=vk::AccessFlagBits2::eMemoryWrite;
  const auto consumers=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite;
  CsPartialFlushProducerStages state;
  const auto Check=[&](vk::PipelineStageFlags2 source,const char* control) {
    const auto first=state.Plan(),second=state.Plan();
    Require(name,control,first.srcStageMask==source&&second.srcStageMask==source&&
      first.srcAccessMask==writes&&second.srcAccessMask==writes&&
      first.dstStageMask==all&&second.dstStageMask==all&&
      first.dstAccessMask==consumers&&second.dstAccessMask==consumers,
      "producer coverage, all-consumer visibility or non-consuming planning changed");
  };
  Check(compute|transfer,"native CS and transfer substitutions do not wait unrelated graphics");
  state.Note(transfer);state.Note(compute);
  Check(compute|transfer,"duplicate/native-stage notes are idempotent");
  state.Note(color);
  Check(compute|transfer|color,"actual rendering loadOp clear preserves its native producer");
  state.Note(transfer);
  Check(compute|transfer|color,"intervening unrelated producers do not erase prior color substitution");
  state.CoveredByRecordedCsPartialFlush();
  Check(compute|transfer,"only a recorded covering flush consumes substitution epoch");
  state.Note(vk::PipelineStageFlagBits2::eFragmentShader);
  Check(all,"unsupported new substitution retains conservative full dependency");
  state.Note(color);state.Note(transfer);
  Check(all,"later supported notes do not erase unsupported-stage fallback");
  state.CoveredByRecordedCsPartialFlush();
  Check(compute|transfer,"covering fallback completion begins next safe epoch");
  state.Note(vk::PipelineStageFlagBits2::eHost);
  Check(all|vk::PipelineStageFlagBits2::eHost,"host pseudo-stage is preserved explicitly by fallback");
  state.CoveredByRecordedCsPartialFlush();
  Check(compute|transfer,"host fallback epoch clears only at covering flush");
}
