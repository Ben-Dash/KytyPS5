void CheckImageReadStageSubsetPlannerCost(RenderContext &renderer) {
  constexpr const char *name = "ImageReadStageSubsetPlannerCost";
  auto &context = renderer.GetGraphics();
  CommandScheduler scheduler(renderer, context);
  Image image(context, scheduler, ReadScopePlannerInfo());
  const auto general = vk::ImageLayout::eGeneral;
  const auto transfer = vk::PipelineStageFlagBits2::eTransfer;
  const auto fragment = vk::PipelineStageFlagBits2::eFragmentShader;
  const auto compute = vk::PipelineStageFlagBits2::eComputeShader;
  const auto graphics = vk::PipelineStageFlagBits2::eAllGraphics;
  const auto read = vk::AccessFlagBits2::eShaderRead;
  const auto transfer_read = vk::AccessFlagBits2::eTransferRead;
  const auto reset = [&] {
    image.backing.state = {};
    image.backing.subresource_states.clear();
  };

  // Independent visibility negatives before the single intended cost oracle.
  (void)image.GetBarriers(general, vk::AccessFlagBits2::eTransferWrite, transfer, {});
  (void)image.GetBarriers(general, read, fragment, {});
  auto barriers = image.GetBarriers(general, read, compute, {});
  Require(name, "new reader stage", barriers.size() == 1u &&
              barriers[0].srcStageMask == fragment && barriers[0].dstStageMask == compute,
          "unpublished compute visibility was fabricated");
  barriers = image.GetBarriers(general, read, compute | fragment, {});
  Require(name, "wider stage", barriers.size() == 1u,
          "a newly-added reader stage was treated as already-published");
  reset();
  (void)image.GetBarriers(general, read, compute, {});
  barriers = image.GetBarriers(general, read | transfer_read, compute | transfer, {});
  Require(name, "wider access and stage", barriers.size() == 1u &&
              barriers[0].dstStageMask == (compute | transfer),
          "new transfer access did not acquire visibility");
  reset();
  (void)image.GetBarriers(general, read, vk::PipelineStageFlagBits2::eAllCommands, {});
  barriers = image.GetBarriers(general, read, compute, {});
  Require(name, "aggregate stage is not a raw bitwise subset", barriers.size() == 1u,
          "aggregate stage semantics were guessed by the narrow raw-bit proof");
  reset();
  (void)image.GetBarriers(general, read, compute, {});
  barriers = image.GetBarriers(general, read, vk::PipelineStageFlagBits2::eNone, {});
  Require(name, "malformed nonzero access with empty stage", barriers.size() == 1u,
          "elision hid a malformed stage/access tuple from API validation");

  // Exact metadata emitted by current Upload/TryUpload post-state and sampled
  // descriptor consumers. No aggregate-mask expansion is needed for this subset.
  const auto upload_read_stages = graphics | compute | transfer;
  const auto sampled_read_stages = graphics | compute;
  reset();
  (void)image.GetBarriers(general, vk::AccessFlagBits2::eTransferWrite, transfer, {});
  barriers = image.GetBarriers(general, read | transfer_read, upload_read_stages, {});
  Require(name, "upload publication", barriers.size() == 1u &&
              barriers[0].srcAccessMask == vk::AccessFlagBits2::eTransferWrite &&
              barriers[0].dstStageMask == upload_read_stages &&
              barriers[0].dstAccessMask == (read | transfer_read),
          "real upload post-state did not publish all requested reader scopes");
  const auto cost = image.GetBarriers(general, read, sampled_read_stages, {}).size();
  const auto retained = image.backing.state;
  barriers = image.GetBarriers(general, vk::AccessFlagBits2::eShaderWrite, compute, {});
  Require(name, "next writer ordered", barriers.size() == 1u &&
              barriers[0].dstStageMask == compute &&
              barriers[0].dstAccessMask == vk::AccessFlagBits2::eShaderWrite,
          "covered reader lost its next-writer dependency");
  std::printf("[image-read-scope] uploadConsumerStageSubsetCost=%zu retainedStages=%llu\n",
              cost, static_cast<unsigned long long>(
                        static_cast<vk::PipelineStageFlags2::MaskType>(retained.pl_stage)));
  Require(name, "redundant already-published upload-consumer cost", cost == 0u,
          "a consumer inside the upload's published stage/access scope emitted a barrier");
  Require(name, "prior published scope retained", retained.pl_stage == upload_read_stages &&
              retained.access_mask == (read | transfer_read) &&
              barriers[0].srcStageMask == upload_read_stages &&
              barriers[0].srcAccessMask == (read | transfer_read),
          "stage-subset elision narrowed remembered publication/reader scope");
  std::printf("[image-read-scope] upload-consumer stage subset controls ok\n");
}
