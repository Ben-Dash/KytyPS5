// Neighboring native regression: external descriptor ownership retires per GPU
// use, permits more than the simultaneous-pool budget over time, and never caches
// a descriptor referring to a view destroyed between completed uses.
int RunRgbMergeLeaseTest(const DlssProjectInfo& project) {
    Vulkan vk; vk.Initialize(project, false);
    Require(vk.validation_enabled, "RGB merge lease regression requires Vulkan validation");
    ResourceControl(vk);
    const DlssExtent backing{17, 11}, active{13, 9};
    const size_t count = size_t(backing.width) * backing.height, bytes = count * 8;
    const size_t source = vk.MakeImage(backing, VK_FORMAT_R16G16B16A16_SFLOAT, true);
    const size_t a = vk.MakeImage(backing, VK_FORMAT_R16G16B16A16_SFLOAT, true);
    const size_t b = vk.MakeImage(backing, VK_FORMAT_R16G16B16A16_SFLOAT, true);
    const size_t source_upload = vk.MakeBuffer(bytes), a_upload = vk.MakeBuffer(bytes);
    const size_t b_upload = vk.MakeBuffer(bytes);
    const size_t a_readback = vk.MakeBuffer(bytes + ReadbackGuardBytes);
    const size_t b_readback = vk.MakeBuffer(bytes + ReadbackGuardBytes);
    DlssRgbMerge merge;
    CheckDlss(merge.Init(vk.device, vk.gdpa), "RGB lease Init");
    constexpr unsigned Iterations = 24;
    unsigned rejected_early_destroy = 0, recreated_views = 0;
    for (unsigned iteration = 0; iteration < Iterations; ++iteration) {
        auto* source_data = static_cast<uint16_t*>(vk.buffers[source_upload].mapped);
        auto* a_data = static_cast<uint16_t*>(vk.buffers[a_upload].mapped);
        auto* b_data = static_cast<uint16_t*>(vk.buffers[b_upload].mapped);
        for (size_t i = 0; i < count; ++i) {
            for (size_t channel = 0; channel < 3; ++channel) {
                source_data[i * 4 + channel] = Half(2.0f + float(iteration) * 0.125f +
                    float((i + channel) % 5) * 0.0625f);
                a_data[i * 4 + channel] = Half(0.125f + float(channel) * 0.0625f);
                b_data[i * 4 + channel] = Half(0.5f + float(channel) * 0.0625f);
            }
            source_data[i * 4 + 3] = Half(1.0f);
            a_data[i * 4 + 3] = Half(0.25f + float((i + iteration) % 7) * 0.125f);
            b_data[i * 4 + 3] = i % 2 ? Half(-0.5f - float(iteration % 3) * 0.25f) : 0;
        }
        std::memset(vk.buffers[a_readback].mapped, 0xa5, size_t(vk.buffers[a_readback].bytes));
        std::memset(vk.buffers[b_readback].mapped, 0xa5, size_t(vk.buffers[b_readback].bytes));
        vk.Begin(); vk.Upload(source, source_upload); vk.Upload(a, a_upload); vk.Upload(b, b_upload);
        vk.Barrier(vk.images[source], VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT);
        vk.Barrier(vk.images[a], VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        vk.Barrier(vk.images[b], VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        std::shared_ptr<void> lease;
        CheckDlss(merge.Record(vk.command, vk.images[source].view, vk.images[a].view,
            vk.images[b].view, active, &lease), "RGB external lease Record");
        Require(bool(lease), "RGB merge failed to publish owning descriptor lease");
        vk.Download(a, a_readback); vk.Download(b, b_readback); vk.Submit();
        // Fence has completed, but ownership is still explicit. The helper must
        // not silently destroy native state while its external lease is held.
        Require(merge.DestroyAfterGpuIdle().code == DlssStatusCode::InvalidState,
                "RGB merge destroyed pipeline with an unretired external lease");
        ++rejected_early_destroy;
        const auto* got_a = static_cast<const uint16_t*>(vk.buffers[a_readback].mapped);
        const auto* got_b = static_cast<const uint16_t*>(vk.buffers[b_readback].mapped);
        for (size_t i = 0; i < count; ++i) {
            const bool inside = i % backing.width < active.width && i / backing.width < active.height;
            for (size_t channel = 0; channel < 4; ++channel) {
                Require(got_a[i * 4 + channel] == (inside && channel < 3 ?
                    source_data[i * 4 + channel] : a_data[i * 4 + channel]),
                    "external lease RGB/alpha/padding A mismatch");
                Require(got_b[i * 4 + channel] == (inside && channel < 3 ?
                    source_data[i * 4 + channel] : b_data[i * 4 + channel]),
                    "external lease RGB/alpha/padding B mismatch");
            }
        }
        VerifyGuard(vk.buffers[a_readback], bytes); VerifyGuard(vk.buffers[b_readback], bytes);
        lease.reset(); // Retire pool/set ONLY after completed fence and consumers.
        if (iteration + 1 < Iterations) {
            auto& image = vk.images[a];
            vk.vkDestroyImageView(vk.device, image.view, nullptr); image.view = VK_NULL_HANDLE;
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = image.handle; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = image.format;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            Check(vk.vkCreateImageView(vk.device, &view, nullptr, &image.view), "recreate retired RGB target view");
            ++recreated_views;
        }
    }
    CheckDlss(merge.DestroyAfterGpuIdle(), "RGB lease final DestroyAfterGpuIdle");
    vk.ValidateAndCleanup();
    std::printf("RGB LEASE PASS: completed=%u external_retirements=%u rejected_early_destroy=%u "
                "view_recreations=%u exact_rgb_alpha_padding=1 guards=1 final_validation=0; no NGX/game\n",
        Iterations, Iterations, rejected_early_destroy, recreated_views);
    return 0;
}
