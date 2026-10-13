// Existing Image::GetBarriers on real tiny Image objects. Planner only: no GPU
// command is submitted and no shader or game bytes are used.
ImageInfo ReadScopePlannerInfo() {
  ImageInfo info{};
  info.pixel_format = vk::Format::eR8Unorm;
  info.guest_format = Prospero::BufferFormat::k8UNorm;
  info.type = Prospero::ImageType::kColor2D;
  info.extent = {4, 4, 1};
  info.resources = {2, 2};
  info.pitch = 4;
  info.bytes_per_block = 1;
  info.samples = 1;
  info.tile_mode = Prospero::TileMode::kLinear;
  return info;
}

void CheckImageCoveredReadPlannerCost(RenderContext &renderer) {
  constexpr const char *name = "ImageCoveredReadPlannerCost";
  auto &context = renderer.GetGraphics();
  CommandScheduler scheduler(renderer, context);
  Image image(context, scheduler, ReadScopePlannerInfo());
  const auto reset = [&] {
    image.backing.state = {};
    image.backing.subresource_states.clear();
  };
  const auto general = vk::ImageLayout::eGeneral;
  const auto all = vk::PipelineStageFlagBits2::eAllCommands;
  const auto compute = vk::PipelineStageFlagBits2::eComputeShader;
  const auto fragment = vk::PipelineStageFlagBits2::eFragmentShader;
  const auto transfer = vk::PipelineStageFlagBits2::eTransfer;
  const auto shader_read = vk::AccessFlagBits2::eShaderRead;
  const auto storage_read = vk::AccessFlagBits2::eShaderStorageRead;
  const auto transfer_read = vk::AccessFlagBits2::eTransferRead;
  const ImageSubresourceRange first{0, 1, 0, 1};
  const ImageSubresourceRange other{1, 1, 1, 1};

  // All existing semantic controls precede the intended redundant-read budget.
  (void)image.GetBarriers(general, vk::AccessFlagBits2::eTransferWrite, transfer, {});
  auto barriers = image.GetBarriers(general, shader_read | transfer_read, all, {});
  Require(name, "writer publication", barriers.size() == 1 &&
              barriers[0].srcStageMask == transfer &&
              barriers[0].srcAccessMask == vk::AccessFlagBits2::eTransferWrite &&
              barriers[0].dstStageMask == all &&
              barriers[0].dstAccessMask == (shader_read | transfer_read),
          "initial writer was not made visible to the whole requested read scope");
  Require(name, "identical read", image.GetBarriers(general, shader_read | transfer_read, all, {}).empty(),
          "existing identical read elision changed");

  reset();
  (void)image.GetBarriers(general, vk::AccessFlagBits2::eShaderWrite, compute, {});
  barriers = image.GetBarriers(general, vk::AccessFlagBits2::eShaderWrite, compute, {});
  Require(name, "repeated writer", barriers.size() == 1 &&
              barriers[0].srcAccessMask == vk::AccessFlagBits2::eShaderWrite,
          "a required same-layout write dependency was dropped");

  reset();
  (void)image.GetBarriers(general, shader_read | transfer_read, all, {});
  barriers = image.GetBarriers(vk::ImageLayout::eShaderReadOnlyOptimal, shader_read, all, {});
  Require(name, "layout negative", barriers.size() == 1 && barriers[0].oldLayout != barriers[0].newLayout,
          "a real layout transition was elided");

  reset();
  (void)image.GetBarriers(general, vk::AccessFlagBits2::eTransferWrite, transfer, {});
  (void)image.GetBarriers(general, vk::AccessFlagBits2::eShaderSampledRead, fragment, {});
  barriers = image.GetBarriers(general, storage_read, compute, {});
  Require(name, "new stage and access negative", barriers.size() == 1 &&
              barriers[0].srcStageMask == fragment && barriers[0].dstStageMask == compute,
          "a reader outside the existing visibility scope was treated as covered");

  reset();
  (void)image.GetBarriers(general, shader_read | transfer_read, all, first);
  barriers = image.GetBarriers(general, shader_read, all, other);
  Require(name, "disjoint subresource negative", barriers.size() == 1 &&
              barriers[0].oldLayout == vk::ImageLayout::eUndefined &&
              barriers[0].subresourceRange.baseMipLevel == 1u &&
              barriers[0].subresourceRange.baseArrayLayer == 1u &&
              image.backing.subresource_states.size() == 4u,
          "one subresource's read visibility leaked to a disjoint mip/layer");

  // Primary: visibility already covers the narrower read and its SAME stage.
  reset();
  (void)image.GetBarriers(general, vk::AccessFlagBits2::eTransferWrite, transfer, {});
  (void)image.GetBarriers(general, shader_read | transfer_read, all, {});
  const auto covered_cost = image.GetBarriers(general, shader_read, all, {}).size();
  const auto covered_state = image.backing.state;
  barriers = image.GetBarriers(general, vk::AccessFlagBits2::eTransferWrite, transfer, {});
  const bool covered_writer_order = barriers.size() == 1 &&
      barriers[0].srcStageMask == all && barriers[0].dstStageMask == transfer;
  Require(name, "later writer semantic control", covered_writer_order,
          "the narrowed read bypassed the subsequent write dependency");

  // Differing read coverage must survive a full-range reuse, rather than union
  // publication to a global scope which some mip/layer never acquired.
  reset();
  (void)image.GetBarriers(general, storage_read, compute, {});
  (void)image.GetBarriers(general, storage_read | shader_read, compute, first);
  const auto heterogeneous_cost = image.GetBarriers(general, storage_read, compute, {}).size();
  const bool heterogeneous_retained = image.backing.subresource_states.size() == 4u &&
      image.backing.subresource_states[0].access_mask == (storage_read | shader_read) &&
      image.backing.subresource_states[3].access_mask == storage_read;
  barriers = image.GetBarriers(general, shader_read, compute, other);
  Require(name, "heterogeneous next-reader negative", barriers.size() == 1 &&
              barriers[0].subresourceRange.baseMipLevel == 1u &&
              barriers[0].subresourceRange.baseArrayLayer == 1u,
          "full-range reader reuse fabricated visibility on a disjoint subresource");

  std::printf("[image-read-scope] coveredCost=%zu heterogeneousCost=%zu "
              "heterogeneousRetained=%u writerOrdered=%u\n", covered_cost,
              heterogeneous_cost, unsigned(heterogeneous_retained), unsigned(covered_writer_order));
  Require(name, "redundant covered-read cost", covered_cost == 0u && heterogeneous_cost == 0u,
          "same-layout reads inside the already-published stage/access scope emitted barriers");
  Require(name, "retained covered scope", covered_state.pl_stage == all &&
              covered_state.access_mask == (shader_read | transfer_read) && heterogeneous_retained,
          "eliding the redundant read discarded publication/reader provenance");
  std::printf("[image-read-scope] covered-read planner controls ok\n");
}

void CheckImageNewReadStagePlanner(RenderContext &renderer) {
  constexpr const char *name = "ImageNewReadStagePlanner";
  auto &context = renderer.GetGraphics();
  CommandScheduler scheduler(renderer, context);
  Image image(context, scheduler, ReadScopePlannerInfo());
  const auto general = vk::ImageLayout::eGeneral;
  const auto transfer = vk::PipelineStageFlagBits2::eTransfer;
  const auto fragment = vk::PipelineStageFlagBits2::eFragmentShader;
  const auto compute = vk::PipelineStageFlagBits2::eComputeShader;
  const auto read = vk::AccessFlagBits2::eShaderRead;
  (void)image.GetBarriers(general, vk::AccessFlagBits2::eTransferWrite, transfer, {});
  auto barriers = image.GetBarriers(general, read, fragment, {});
  Require(name, "first reader publication", barriers.size() == 1 &&
              barriers[0].srcStageMask == transfer && barriers[0].dstStageMask == fragment,
          "transfer writer was not published to the initial reader");
  barriers = image.GetBarriers(general, read, compute, {});
  std::printf("[image-read-scope] sameAccessNewStage barriers=%zu\n", barriers.size());
  Require(name, "new reader visibility", barriers.size() == 1 &&
              barriers[0].srcStageMask == fragment && barriers[0].dstStageMask == compute &&
              barriers[0].srcAccessMask == read && barriers[0].dstAccessMask == read,
          "same access/layout hid a new compute reader outside fragment publication");
  barriers = image.GetBarriers(general, vk::AccessFlagBits2::eShaderWrite, compute, {});
  Require(name, "following write", barriers.size() == 1 &&
              barriers[0].srcStageMask == compute &&
              barriers[0].dstAccessMask == vk::AccessFlagBits2::eShaderWrite,
          "the new reader did not order a following write");
  std::printf("[image-read-scope] new-stage planner controls ok\n");
}

void CheckImagePartialReadStagePlanner(RenderContext &renderer) {
  constexpr const char *name = "ImagePartialReadStagePlanner";
  auto &context = renderer.GetGraphics();
  CommandScheduler scheduler(renderer, context);
  Image image(context, scheduler, ReadScopePlannerInfo());
  const auto general = vk::ImageLayout::eGeneral;
  const auto transfer = vk::PipelineStageFlagBits2::eTransfer;
  const auto fragment = vk::PipelineStageFlagBits2::eFragmentShader;
  const auto compute = vk::PipelineStageFlagBits2::eComputeShader;
  const auto read = vk::AccessFlagBits2::eShaderRead;
  const ImageSubresourceRange first{0, 1, 0, 1};
  const ImageSubresourceRange other{1, 1, 1, 1};
  (void)image.GetBarriers(general, vk::AccessFlagBits2::eTransferWrite, transfer, {});
  auto barriers = image.GetBarriers(general, read, fragment, first);
  Require(name, "first partial publication", barriers.size() == 1 &&
              barriers[0].srcStageMask == transfer && barriers[0].dstStageMask == fragment &&
              image.backing.subresource_states.size() == 4u,
          "first fragment reader did not retain authoritative split state");
  barriers = image.GetBarriers(general, read, compute, other);
  Require(name, "disjoint partial publication", barriers.size() == 1 &&
              barriers[0].srcAccessMask == vk::AccessFlagBits2::eTransferWrite &&
              image.backing.subresource_states[0].pl_stage == fragment &&
              image.backing.subresource_states[3].pl_stage == compute,
          "disjoint compute reader overwrote another mip/layer's publication");
  barriers = image.GetBarriers(general, read, compute, first);
  Require(name, "partial new-stage visibility", barriers.size() == 1 &&
              barriers[0].srcStageMask == fragment && barriers[0].dstStageMask == compute &&
              barriers[0].subresourceRange.baseMipLevel == 0u &&
              barriers[0].subresourceRange.baseArrayLayer == 0u &&
              image.backing.subresource_states[3].pl_stage == compute &&
              image.backing.subresource_states[1].access_mask == vk::AccessFlagBits2::eTransferWrite &&
              image.backing.subresource_states[2].access_mask == vk::AccessFlagBits2::eTransferWrite,
          "same access/layout hid a new partial reader or changed disjoint state");
  Require(name, "existing partial reader remains covered",
          image.GetBarriers(general, read, compute, other).empty(),
          "a disjoint already-published reader gained a spurious dependency");
  barriers = image.GetBarriers(general, read, compute, {});
  const bool remaining_writers = barriers.size() == 2u &&
      std::ranges::all_of(barriers, [&](const auto &barrier) {
        return barrier.srcStageMask == transfer &&
               barrier.srcAccessMask == vk::AccessFlagBits2::eTransferWrite &&
               barrier.dstStageMask == compute && barrier.dstAccessMask == read;
      });
  Require(name, "full-range reconciliation", remaining_writers &&
              image.backing.subresource_states.empty() &&
              image.backing.state.pl_stage == compute && image.backing.state.access_mask == read,
          "full read lost remaining writers or failed to collapse truly identical scopes");
  barriers = image.GetBarriers(general, vk::AccessFlagBits2::eShaderWrite, compute, {});
  Require(name, "post-reconciliation writer", barriers.size() == 1u &&
              barriers[0].srcStageMask == compute && barriers[0].srcAccessMask == read &&
              barriers[0].dstAccessMask == vk::AccessFlagBits2::eShaderWrite,
          "all reconciled readers did not order the next writer");

  // Same read-access mask, but per-mip/layer publication is still heterogeneous.
  image.backing.state = {};
  image.backing.subresource_states.clear();
  (void)image.GetBarriers(general, read, fragment, {});
  (void)image.GetBarriers(general, read, compute, other);
  Require(name, "heterogeneous stages retained",
          image.backing.subresource_states.size() == 4u &&
              image.backing.subresource_states[0].pl_stage == fragment &&
              image.backing.subresource_states[3].pl_stage == compute,
          "a partial stage change fabricated global reader visibility");
  barriers = image.GetBarriers(general, read, fragment, {});
  Require(name, "all-mip stage normalization", barriers.size() == 1u &&
              barriers[0].srcStageMask == compute && barriers[0].dstStageMask == fragment &&
              barriers[0].subresourceRange.baseMipLevel == 1u &&
              barriers[0].subresourceRange.baseArrayLayer == 1u &&
              image.backing.subresource_states.empty() &&
              image.backing.state.pl_stage == fragment,
          "full-range read did not normalize just the differently-published subresource");
  std::printf("[image-read-scope] partial/new-stage/full-mip reconciliation controls ok\n");
}
